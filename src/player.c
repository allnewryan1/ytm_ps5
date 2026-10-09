#include "player.h"
#include "net.h"

#include <SDL.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static SDL_AudioDeviceID g_dev;
static pthread_t g_thread;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_thread_live;
static atomic_int g_stop;
static atomic_int g_paused;
static int g_seek_req;
static double g_seek_to;
static char g_url[8192];
static char g_ua[200];
static char g_ref[120];
static char g_err[192];
static int g_duration_hint;
static atomic_uint_least64_t g_samples;
static atomic_int g_ended;
static atomic_int g_volume; /* 0..100 */
static unsigned char *g_owned;
static unsigned char *g_audio;
static int g_audio_n;
static int g_audio_off;

/* Full-scale PCM. The callback applies volume as it plays, so L2/R2 are not
 * stuck behind a second of already-queued audio. */
#define RING_CAP (48000 * 4 * 2)
static unsigned char g_ring[RING_CAP];
static atomic_uint g_rhead;
static atomic_uint g_rtail;

static void ring_clear(void) {
  atomic_store(&g_rtail, atomic_load(&g_rhead));
}

static void ring_write(const uint8_t *src, int n) {
  while (n > 0 && !atomic_load(&g_stop)) {
    uint32_t head = atomic_load(&g_rhead);
    uint32_t tail = atomic_load(&g_rtail);
    uint32_t used = head - tail;
    uint32_t space = RING_CAP - used;
    uint32_t chunk, pos, first;
    if (space < 4096) {
      usleep(4000);
      continue;
    }
    chunk = (uint32_t)n;
    if (chunk > space) chunk = space;
    pos = head % RING_CAP;
    first = RING_CAP - pos;
    if (first > chunk) first = chunk;
    memcpy(g_ring + pos, src, first);
    if (chunk > first) memcpy(g_ring, src + first, chunk - first);
    atomic_store(&g_rhead, head + chunk);
    src += chunk;
    n -= (int)chunk;
  }
}

static void audio_cb(void *opaque, Uint8 *stream, int len) {
  int16_t *out = (int16_t *)stream;
  int samples = len / (int)sizeof(int16_t);
  int vol = atomic_load(&g_volume);
  int i = 0;
  (void)opaque;
  if (vol < 0) vol = 0;
  if (vol > 100) vol = 100;
  while (i < samples) {
    uint32_t head = atomic_load(&g_rhead);
    uint32_t tail = atomic_load(&g_rtail);
    uint32_t avail = head - tail;
    uint32_t pos, n, room;
    const int16_t *in;
    uint32_t k;
    if (avail < 2) break;
    pos = tail % RING_CAP;
    n = (uint32_t)(samples - i);
    if (n * 2 > avail) n = avail / 2;
    room = (RING_CAP - pos) / 2;
    if (n > room) n = room;
    if (n < 1) break;
    in = (const int16_t *)(g_ring + pos);
    for (k = 0; k < n; k++) {
      int s = (in[k] * vol) / 100;
      if (s > 32767) s = 32767;
      if (s < -32768) s = -32768;
      out[i + (int)k] = (int16_t)s;
    }
    atomic_store(&g_rtail, tail + n * 2);
    i += (int)n;
  }
  if (i < samples) memset(out + i, 0, (size_t)(samples - i) * sizeof(int16_t));
}

static int interrupt_cb(void *opaque) {
  (void)opaque;
  return atomic_load(&g_stop);
}

static void set_err(const char *s) {
  pthread_mutex_lock(&g_mu);
  snprintf(g_err, sizeof g_err, "%s", s ? s : "");
  pthread_mutex_unlock(&g_mu);
}

int player_open(char *err, int err_n) {
  SDL_AudioSpec want, have;
  avformat_network_init();
  memset(&want, 0, sizeof want);
  want.freq = 48000;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = 1024;
  want.callback = audio_cb;
  g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  if (g_dev == 0) {
    snprintf(err, (size_t)err_n, "Audio: %s", SDL_GetError());
    return -1;
  }
  atomic_store(&g_volume, 80);
  SDL_PauseAudioDevice(g_dev, 0);
  err[0] = 0;
  return 0;
}

static int mem_read(void *opaque, uint8_t *buf, int buf_size) {
  (void)opaque;
  if (g_audio_off >= g_audio_n) return AVERROR_EOF;
  if (buf_size > g_audio_n - g_audio_off) buf_size = g_audio_n - g_audio_off;
  memcpy(buf, g_audio + g_audio_off, (size_t)buf_size);
  g_audio_off += buf_size;
  return buf_size;
}

static int64_t mem_seek(void *opaque, int64_t off, int whence) {
  (void)opaque;
  if (whence & AVSEEK_SIZE) return g_audio_n;
  whence &= ~AVSEEK_FORCE;
  if (whence == SEEK_SET) g_audio_off = (int)off;
  else if (whence == SEEK_CUR) g_audio_off += (int)off;
  else if (whence == SEEK_END) g_audio_off = g_audio_n + (int)off;
  else return -1;
  if (g_audio_off < 0 || g_audio_off > g_audio_n) return -1;
  return g_audio_off;
}

static void emit_frame(SwrContext *swr, AVFrame *frame) {
  int out_count = swr_get_out_samples(swr, frame->nb_samples);
  uint8_t *out = NULL;
  int linesize = 0;
  int got;
  if (out_count < 1) out_count = frame->nb_samples;
  if (av_samples_alloc(&out, &linesize, 2, out_count, AV_SAMPLE_FMT_S16, 0) < 0) return;
  got = swr_convert(swr, &out, out_count, (const uint8_t **)frame->extended_data, frame->nb_samples);
  if (got > 0) {
    ring_write(out, got * 2 * (int)sizeof(int16_t));
    atomic_fetch_add(&g_samples, (uint64_t)got);
  }
  av_freep(&out);
}

/* The resampler holds a few milliseconds after the last decoded frame. */
static void flush_swr(SwrContext *swr) {
  uint8_t *out = NULL;
  int linesize = 0;
  int out_count = swr_get_out_samples(swr, 0);
  int got;
  if (out_count < 1) return;
  if (av_samples_alloc(&out, &linesize, 2, out_count, AV_SAMPLE_FMT_S16, 0) < 0) return;
  got = swr_convert(swr, &out, out_count, NULL, 0);
  if (got > 0) {
    ring_write(out, got * 2 * (int)sizeof(int16_t));
    atomic_fetch_add(&g_samples, (uint64_t)got);
  }
  av_freep(&out);
}

static void *decode_main(void *arg) {
  AVFormatContext *fmt = NULL;
  AVCodecContext *dec = NULL;
  AVFrame *frame = NULL;
  AVPacket *pkt = NULL;
  SwrContext *swr = NULL;
  const AVCodec *codec = NULL;
  AVDictionary *opts = NULL;
  AVIOContext *avio = NULL;
  int custom = 0;
  int si;
  char url[8192];
  char ua[200];
  char ref[120];
  int use_mem;
  (void)arg;

  pthread_mutex_lock(&g_mu);
  snprintf(url, sizeof url, "%s", g_url);
  snprintf(ua, sizeof ua, "%s", g_ua);
  snprintf(ref, sizeof ref, "%s", g_ref);
  use_mem = g_audio != NULL && g_audio_n > 32;
  pthread_mutex_unlock(&g_mu);

  fmt = avformat_alloc_context();
  if (!fmt) {
    set_err("Decoder ran out of memory");
    goto done;
  }
  fmt->interrupt_callback.callback = interrupt_cb;
  fmt->interrupt_callback.opaque = NULL;

  if (use_mem) {
    unsigned char *ab = av_malloc(8192);
    if (!ab) {
      set_err("Decoder ran out of memory");
      goto done;
    }
    g_audio_off = 0;
    avio = avio_alloc_context(ab, 8192, 0, NULL, mem_read, NULL, mem_seek);
    if (!avio) {
      av_free(ab);
      set_err("Decoder ran out of memory");
      goto done;
    }
    fmt->pb = avio;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    custom = 1;
    if (avformat_open_input(&fmt, NULL, NULL, NULL) < 0) {
      set_err("Could not open the audio stream");
      goto done;
    }
  } else {
    av_dict_set(&opts, "user_agent", ua[0] ? ua : ytm_stream_ua(), 0);
    av_dict_set(&opts, "referer", ref[0] ? ref : ytm_stream_referer(), 0);
    av_dict_set(&opts, "headers", "Origin: https://www.youtube.com\r\n", 0);
    av_dict_set(&opts, "multiple_requests", "1", 0);
    av_dict_set(&opts, "reconnect", "1", 0);
    av_dict_set(&opts, "reconnect_streamed", "1", 0);
    av_dict_set(&opts, "reconnect_delay_max", "2", 0);
    av_dict_set(&opts, "rw_timeout", "15000000", 0);
    if (avformat_open_input(&fmt, url, NULL, &opts) < 0) {
      set_err("Could not open the audio stream");
      av_dict_free(&opts);
      goto done;
    }
    av_dict_free(&opts);
    opts = NULL;
  }

  if (avformat_find_stream_info(fmt, NULL) < 0) {
    set_err("Could not read stream info");
    goto done;
  }

  si = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
  if (si < 0 || !codec) {
    set_err("No audio track in that stream");
    goto done;
  }

  dec = avcodec_alloc_context3(codec);
  if (!dec || avcodec_parameters_to_context(dec, fmt->streams[si]->codecpar) < 0 ||
      avcodec_open2(dec, codec, NULL) < 0) {
    set_err("Could not open the audio decoder");
    goto done;
  }
  if (dec->ch_layout.nb_channels <= 0) {
    int ch = fmt->streams[si]->codecpar->ch_layout.nb_channels;
    av_channel_layout_default(&dec->ch_layout, ch > 0 ? ch : 2);
  }
  if (dec->sample_rate <= 0) {
    set_err("Audio sample rate is missing");
    goto done;
  }

  {
    AVChannelLayout dst;
    av_channel_layout_default(&dst, 2);
    if (swr_alloc_set_opts2(&swr, &dst, AV_SAMPLE_FMT_S16, 48000, &dec->ch_layout,
                            dec->sample_fmt, dec->sample_rate, 0, NULL) < 0 ||
        !swr || swr_init(swr) < 0) {
      av_channel_layout_uninit(&dst);
      set_err("Could not set up the resampler");
      goto done;
    }
    av_channel_layout_uninit(&dst);
  }

  if (fmt->duration > 0) {
    pthread_mutex_lock(&g_mu);
    g_duration_hint = (int)(fmt->duration / AV_TIME_BASE);
    pthread_mutex_unlock(&g_mu);
  }

  frame = av_frame_alloc();
  pkt = av_packet_alloc();
  if (!frame || !pkt) {
    set_err("Decoder ran out of memory");
    goto done;
  }

  {
    int stalls = 0;
    while (!atomic_load(&g_stop)) {
      int seek = 0;
      double seek_to = 0;
      pthread_mutex_lock(&g_mu);
      if (g_seek_req) {
        seek = 1;
        seek_to = g_seek_to;
        g_seek_req = 0;
      }
      pthread_mutex_unlock(&g_mu);
      if (seek) {
        int64_t ts = (int64_t)(seek_to * AV_TIME_BASE);
        if (ts < 0) ts = 0;
        av_seek_frame(fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(dec);
        ring_clear();
        atomic_store(&g_samples, (uint64_t)(seek_to * 48000.0));
      }

      if (atomic_load(&g_paused)) {
        usleep(20000);
        continue;
      }

      if (av_read_frame(fmt, pkt) < 0) {
        double pos = (double)atomic_load(&g_samples) / 48000.0;
        double dur;
        pthread_mutex_lock(&g_mu);
        dur = g_duration_hint;
        pthread_mutex_unlock(&g_mu);
        /* A dropped connection near the end used to be treated as the end of the song. */
        if (stalls < 2 && dur > 8 && pos + 8 < dur &&
            av_seek_frame(fmt, -1, (int64_t)(pos * AV_TIME_BASE), AVSEEK_FLAG_ANY) >= 0) {
          stalls++;
          avcodec_flush_buffers(dec);
          continue;
        }
        break;
      }
      stalls = 0;
      if (pkt->stream_index != si) {
        av_packet_unref(pkt);
        continue;
      }
      if (avcodec_send_packet(dec, pkt) == 0) {
        while (avcodec_receive_frame(dec, frame) == 0) {
          emit_frame(swr, frame);
          av_frame_unref(frame);
        }
      }
      av_packet_unref(pkt);
    }
    if (!atomic_load(&g_stop) && dec && swr && frame) {
      avcodec_send_packet(dec, NULL);
      while (avcodec_receive_frame(dec, frame) == 0) {
        emit_frame(swr, frame);
        av_frame_unref(frame);
      }
      flush_swr(swr);
      while (!atomic_load(&g_stop) && atomic_load(&g_rhead) != atomic_load(&g_rtail))
        usleep(20000);
      if (!atomic_load(&g_stop)) atomic_store(&g_ended, 1);
    }
  }

done:
  if (g_err[0] == 0 && atomic_load(&g_stop)) {
    /* user skip, not a failure */
  } else if (g_err[0] == 0 && !g_stop) {
    /* natural end */
  }
  if (swr) swr_free(&swr);
  if (pkt) av_packet_free(&pkt);
  if (frame) av_frame_free(&frame);
  if (dec) avcodec_free_context(&dec);
  if (fmt) {
    AVIOContext *pb = custom ? fmt->pb : NULL;
    if (custom) fmt->pb = NULL;
    avformat_close_input(&fmt);
    if (pb) {
      av_freep(&pb->buffer);
      avio_context_free(&pb);
    }
  } else if (avio) {
    av_freep(&avio->buffer);
    avio_context_free(&avio);
  }
  if (opts) av_dict_free(&opts);
  return NULL;
}

static void remember_headers(void) {
  snprintf(g_ua, sizeof g_ua, "%s", ytm_stream_ua());
  snprintf(g_ref, sizeof g_ref, "%s", ytm_stream_referer());
}

static void drop_audio(void) {
  free(g_owned);
  g_owned = NULL;
  g_audio = NULL;
  g_audio_n = 0;
  g_audio_off = 0;
}

int player_start(const char *url, int duration_s) {
  player_stop();
  drop_audio();
  if (!url || !url[0]) {
    set_err("Missing audio URL");
    return -1;
  }
  if (g_dev == 0) {
    set_err("Audio output is not open");
    return -1;
  }
  pthread_mutex_lock(&g_mu);
  snprintf(g_url, sizeof g_url, "%s", url);
  remember_headers();
  g_err[0] = 0;
  g_stop = 0;
  g_paused = 0;
  g_seek_req = 0;
  g_duration_hint = duration_s > 0 ? duration_s : 0;
  pthread_mutex_unlock(&g_mu);
  atomic_store(&g_samples, 0);
  atomic_store(&g_ended, 0);
  atomic_store(&g_stop, 0);
  atomic_store(&g_paused, 0);
  atomic_store(&g_rhead, 0);
  atomic_store(&g_rtail, 0);
  SDL_PauseAudioDevice(g_dev, 0);
  if (pthread_create(&g_thread, NULL, decode_main, NULL) != 0) {
    set_err("Could not start the decoder thread");
    return -1;
  }
  g_thread_live = 1;
  return 0;
}

int player_start_mem(unsigned char *data, int n, int duration_s) {
  player_stop();
  drop_audio();
  if (!data || n < 32) {
    free(data);
    set_err("Missing audio");
    return -1;
  }
  if (g_dev == 0) {
    free(data);
    set_err("Audio output is not open");
    return -1;
  }
  g_owned = data;
  g_audio = data;
  g_audio_n = n;
  g_audio_off = 0;
  pthread_mutex_lock(&g_mu);
  g_url[0] = 0;
  remember_headers();
  g_err[0] = 0;
  g_stop = 0;
  g_paused = 0;
  g_seek_req = 0;
  g_duration_hint = duration_s > 0 ? duration_s : 0;
  pthread_mutex_unlock(&g_mu);
  atomic_store(&g_samples, 0);
  atomic_store(&g_ended, 0);
  atomic_store(&g_stop, 0);
  atomic_store(&g_paused, 0);
  atomic_store(&g_rhead, 0);
  atomic_store(&g_rtail, 0);
  SDL_PauseAudioDevice(g_dev, 0);
  if (pthread_create(&g_thread, NULL, decode_main, NULL) != 0) {
    set_err("Could not start the decoder thread");
    return -1;
  }
  g_thread_live = 1;
  return 0;
}

int player_replay(void) {
  unsigned char *keep;
  int n;
  int dur;
  char url[8192];
  if (g_owned && g_audio_n > 32) {
    keep = g_owned;
    n = g_audio_n;
    dur = g_duration_hint;
    g_owned = NULL;
    g_audio = NULL;
    return player_start_mem(keep, n, dur);
  }
  pthread_mutex_lock(&g_mu);
  snprintf(url, sizeof url, "%s", g_url);
  dur = g_duration_hint;
  pthread_mutex_unlock(&g_mu);
  if (!url[0]) return -1;
  return player_start(url, dur);
}

void player_stop(void) {
  if (!g_thread_live) return;
  atomic_store(&g_stop, 1);
  pthread_join(g_thread, NULL);
  g_thread_live = 0;
  atomic_store(&g_stop, 0);
  if (g_dev) SDL_PauseAudioDevice(g_dev, 1);
  atomic_store(&g_rhead, 0);
  atomic_store(&g_rtail, 0);
  if (g_dev) SDL_PauseAudioDevice(g_dev, 0);
}

void player_close(void) {
  player_stop();
  drop_audio();
  if (g_dev) {
    SDL_CloseAudioDevice(g_dev);
    g_dev = 0;
  }
  avformat_network_deinit();
}

void player_toggle(void) {
  if (!g_thread_live || g_dev == 0) return;
  {
    int now_paused = !atomic_load(&g_paused);
    atomic_store(&g_paused, now_paused);
    SDL_PauseAudioDevice(g_dev, now_paused ? 1 : 0);
  }
}

void player_seek_by(double delta) {
  double now;
  double dur;
  if (!g_thread_live) return;
  now = player_position() + delta;
  dur = player_duration();
  if (now < 0) now = 0;
  if (dur > 1 && now > dur - 1) now = dur - 1;
  pthread_mutex_lock(&g_mu);
  g_seek_to = now;
  g_seek_req = 1;
  pthread_mutex_unlock(&g_mu);
}

void player_volume_add(int delta) {
  int v = atomic_load(&g_volume) + delta;
  if (v < 0) v = 0;
  if (v > 100) v = 100;
  atomic_store(&g_volume, v);
}

int player_paused(void) { return atomic_load(&g_paused); }
int player_ended(void) { return atomic_load(&g_ended); }
void player_ack_ended(void) { atomic_store(&g_ended, 0); }

double player_position(void) {
  uint32_t queued = atomic_load(&g_rhead) - atomic_load(&g_rtail);
  double decoded = (double)atomic_load(&g_samples) / 48000.0;
  double q = (double)queued / (48000.0 * 4.0);
  double pos = decoded - q;
  return pos < 0 ? 0 : pos;
}

double player_duration(void) {
  int d;
  pthread_mutex_lock(&g_mu);
  d = g_duration_hint;
  pthread_mutex_unlock(&g_mu);
  return d > 0 ? (double)d : 0;
}

int player_volume(void) { return atomic_load(&g_volume); }

const char *player_error(void) {
  static char copy[192];
  pthread_mutex_lock(&g_mu);
  snprintf(copy, sizeof copy, "%s", g_err);
  pthread_mutex_unlock(&g_mu);
  return copy;
}
