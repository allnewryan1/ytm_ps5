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
static atomic_int g_eof;    /* decoder finished: all samples are in the ring */
static atomic_int g_volume; /* 0..100 */
static Uint32 g_drained_at;

/* Decoded PCM waits here for the audio callback. Volume is applied in the callback, on the
 * last ~20 ms before the speaker, so a change is heard at once instead of after everything
 * already queued. Frames are stereo S16 at 48 kHz. */
#define OUT_RATE 48000
#define RING_FRAMES (OUT_RATE * 2)
#define AHEAD_FRAMES (OUT_RATE * 3 / 2)
static int16_t g_ring[RING_FRAMES * 2];
static atomic_uint_least64_t g_wr;     /* frames written by the decoder */
static atomic_uint_least64_t g_rd;     /* frames consumed by the callback */
static atomic_uint_least64_t g_played; /* stream position in frames */
static float g_gain_now = 0.8f;        /* callback thread only */

static void audio_cb(void *ud, Uint8 *stream, int len) {
  int16_t *out = (int16_t *)stream;
  int want = len / 4;
  uint64_t rd = atomic_load(&g_rd);
  uint64_t wr = atomic_load(&g_wr);
  int have = (int)(wr - rd);
  int n = have < want ? have : want;
  float target = (float)atomic_load(&g_volume) / 100.0f;
  float step = n > 0 ? (target - g_gain_now) / (float)n : 0.0f;
  int i;
  (void)ud;
  for (i = 0; i < n; i++) {
    const int16_t *src = &g_ring[((rd + (uint64_t)i) % RING_FRAMES) * 2];
    float g = g_gain_now + step * (float)(i + 1);
    int l = (int)((float)src[0] * g);
    int r = (int)((float)src[1] * g);
    if (l > 32767) l = 32767;
    if (l < -32768) l = -32768;
    if (r > 32767) r = 32767;
    if (r < -32768) r = -32768;
    out[i * 2] = (int16_t)l;
    out[i * 2 + 1] = (int16_t)r;
  }
  g_gain_now = target;
  if (n < want) memset(out + n * 2, 0, (size_t)(want - n) * 4u);
  atomic_store(&g_rd, rd + (uint64_t)n);
  atomic_fetch_add(&g_played, (uint64_t)n);
}

/* Drop what has not been heard yet and set the clock. Safe from any thread. */
static void ring_reset(uint64_t position_frames) {
  if (g_dev) SDL_LockAudioDevice(g_dev);
  atomic_store(&g_rd, atomic_load(&g_wr));
  atomic_store(&g_played, position_frames);
  if (g_dev) SDL_UnlockAudioDevice(g_dev);
}

static int ring_fill(void) { return (int)(atomic_load(&g_wr) - atomic_load(&g_rd)); }

/* Copy frames into the ring, waiting for room. Returns -1 when stopped or seeking. */
static int ring_write(const int16_t *pcm, int frames) {
  while (frames > 0) {
    int room;
    int n;
    uint64_t wr;
    if (atomic_load(&g_stop)) return -1;
    pthread_mutex_lock(&g_mu);
    n = g_seek_req;
    pthread_mutex_unlock(&g_mu);
    if (n) return -1;
    room = AHEAD_FRAMES - ring_fill();
    if (room <= 0) {
      usleep(5000);
      continue;
    }
    n = frames < room ? frames : room;
    wr = atomic_load(&g_wr);
    for (int i = 0; i < n; i++) {
      int16_t *d = &g_ring[((wr + (uint64_t)i) % RING_FRAMES) * 2];
      d[0] = pcm[i * 2];
      d[1] = pcm[i * 2 + 1];
    }
    atomic_store(&g_wr, wr + (uint64_t)n);
    pcm += n * 2;
    frames -= n;
  }
  return 0;
}
static unsigned char *g_owned;
static unsigned char *g_audio;
static int g_audio_n;
static int g_audio_off;

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
  want.userdata = NULL;
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

/* Resample one frame (NULL flushes the resampler) to stereo S16 and hand it to the ring. */
static void emit_frame(SwrContext *swr, AVFrame *frame) {
  int in = frame ? frame->nb_samples : 0;
  int out_count = swr_get_out_samples(swr, in);
  uint8_t *out = NULL;
  int linesize = 0;
  int got;
  if (out_count < 1) out_count = in > 0 ? in : 4096;
  if (av_samples_alloc(&out, &linesize, 2, out_count, AV_SAMPLE_FMT_S16, 0) < 0) return;
  got = swr_convert(swr, &out, out_count, frame ? (const uint8_t **)frame->extended_data : NULL,
                    in);
  if (got > 0) ring_write((const int16_t *)out, got);
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
    /* Seekable lets the HTTP layer resume a dropped connection with a Range request at
     * the byte it stopped. With seekable 0 a drop near the end was read as end of file,
     * and the song was cut short. */
    av_dict_set(&opts, "multiple_requests", "1", 0);
    av_dict_set(&opts, "reconnect", "1", 0);
    av_dict_set(&opts, "reconnect_streamed", "1", 0);
    av_dict_set(&opts, "reconnect_on_network_error", "1", 0);
    av_dict_set(&opts, "reconnect_delay_max", "4", 0);
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
    int64_t last_pts = AV_NOPTS_VALUE;
    int retries = 0;
    int drained = 0;
    while (!atomic_load(&g_stop)) {
      int seek = 0;
      double seek_to = 0;
      int rc;
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
        swr_convert(swr, NULL, 0, NULL, 0);
        ring_reset((uint64_t)(seek_to * OUT_RATE));
        last_pts = AV_NOPTS_VALUE;
      }

      if (atomic_load(&g_paused) || ring_fill() >= AHEAD_FRAMES) {
        usleep(10000);
        continue;
      }

      rc = av_read_frame(fmt, pkt);
      if (rc < 0) {
        /* A network error before the end: resume at the last packet instead of skipping. */
        if (rc != AVERROR_EOF && !avio_feof(fmt->pb) && retries < 3 && last_pts != AV_NOPTS_VALUE &&
            !atomic_load(&g_stop)) {
          retries++;
          if (av_seek_frame(fmt, si, last_pts, AVSEEK_FLAG_BACKWARD) >= 0) continue;
        }
        if (rc != AVERROR_EOF && !avio_feof(fmt->pb) && !atomic_load(&g_stop)) {
          char e[96];
          av_strerror(rc, e, sizeof e);
          fprintf(stderr, "ytmusic: stream read failed: %s\n", e);
        }
        drained = 1;
        break;
      }
      if (pkt->stream_index != si) {
        av_packet_unref(pkt);
        continue;
      }
      /* After a resume, skip packets already played. */
      if (last_pts != AV_NOPTS_VALUE && pkt->pts != AV_NOPTS_VALUE && pkt->pts <= last_pts &&
          retries > 0) {
        av_packet_unref(pkt);
        continue;
      }
      if (pkt->pts != AV_NOPTS_VALUE) last_pts = pkt->pts;
      if (avcodec_send_packet(dec, pkt) == 0) {
        while (avcodec_receive_frame(dec, frame) == 0) {
          emit_frame(swr, frame);
          av_frame_unref(frame);
        }
      }
      av_packet_unref(pkt);
    }
    if (drained && !atomic_load(&g_stop)) {
      /* End of file: the decoder and resampler still hold the last few frames. */
      if (avcodec_send_packet(dec, NULL) == 0) {
        while (avcodec_receive_frame(dec, frame) == 0) {
          emit_frame(swr, frame);
          av_frame_unref(frame);
        }
      }
      emit_frame(swr, NULL);
      if (!atomic_load(&g_stop)) atomic_store(&g_eof, 1);
    }
  }

done:
  if (!atomic_load(&g_stop) && player_error()[0]) atomic_store(&g_eof, 1);
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
  atomic_store(&g_eof, 0);
  atomic_store(&g_stop, 0);
  atomic_store(&g_paused, 0);
  g_drained_at = 0;
  ring_reset(0);
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
  atomic_store(&g_eof, 0);
  atomic_store(&g_stop, 0);
  atomic_store(&g_paused, 0);
  g_drained_at = 0;
  ring_reset(0);
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
  ring_reset(0);
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
/* Ended only after the last sample has been played out, not when the file is read.
 * Moving on as soon as the read finished cleared about a second of the song's tail. */
int player_ended(void) {
  if (!atomic_load(&g_eof)) return 0;
  if (ring_fill() > 0 && !atomic_load(&g_paused)) {
    g_drained_at = 0;
    return 0;
  }
  if (ring_fill() > 0) return 0;
  if (g_drained_at == 0) {
    g_drained_at = SDL_GetTicks();
    if (g_drained_at == 0) g_drained_at = 1;
    return 0;
  }
  /* The device still holds one callback buffer (about 21 ms). */
  return SDL_GetTicks() - g_drained_at >= 120;
}

void player_ack_ended(void) {
  atomic_store(&g_eof, 0);
  g_drained_at = 0;
}

double player_position(void) {
  return (double)atomic_load(&g_played) / (double)OUT_RATE;
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
