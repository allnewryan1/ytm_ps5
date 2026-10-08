#include "player.h"

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
static char g_url[4096];
static char g_err[192];
static int g_duration_hint;
static atomic_uint_least64_t g_samples;
static atomic_int g_ended;
static atomic_int g_volume; /* 0..100 */

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
  want.samples = 2048;
  want.callback = NULL;
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

static void *decode_main(void *arg) {
  AVFormatContext *fmt = NULL;
  AVCodecContext *dec = NULL;
  AVFrame *frame = NULL;
  AVPacket *pkt = NULL;
  SwrContext *swr = NULL;
  const AVCodec *codec = NULL;
  AVDictionary *opts = NULL;
  int si;
  char url[4096];
  (void)arg;

  pthread_mutex_lock(&g_mu);
  snprintf(url, sizeof url, "%s", g_url);
  pthread_mutex_unlock(&g_mu);

  fmt = avformat_alloc_context();
  if (!fmt) {
    set_err("Decoder ran out of memory");
    goto done;
  }
  fmt->interrupt_callback.callback = interrupt_cb;
  fmt->interrupt_callback.opaque = NULL;

  av_dict_set(&opts, "user_agent",
              "com.google.android.apps.youtube.music/7.27.52 (Linux; U; Android 11)", 0);
  av_dict_set(&opts, "reconnect", "1", 0);
  av_dict_set(&opts, "reconnect_streamed", "1", 0);
  av_dict_set(&opts, "rw_timeout", "15000000", 0);

  if (avformat_open_input(&fmt, url, NULL, &opts) < 0) {
    set_err("Could not open the audio stream");
    av_dict_free(&opts);
    goto done;
  }
  av_dict_free(&opts);
  opts = NULL;

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
      SDL_ClearQueuedAudio(g_dev);
      atomic_store(&g_samples, (uint64_t)(seek_to * 48000.0));
    }

    if (atomic_load(&g_paused)) {
      usleep(20000);
      continue;
    }
    if (SDL_GetQueuedAudioSize(g_dev) > 48000u * 4u) {
      usleep(12000);
      continue;
    }

    if (av_read_frame(fmt, pkt) < 0) {
      if (!atomic_load(&g_stop)) atomic_store(&g_ended, 1);
      break;
    }
    if (pkt->stream_index != si) {
      av_packet_unref(pkt);
      continue;
    }
    if (avcodec_send_packet(dec, pkt) == 0) {
      while (avcodec_receive_frame(dec, frame) == 0) {
        int out_count = swr_get_out_samples(swr, frame->nb_samples);
        uint8_t *out = NULL;
        int linesize = 0;
        int got;
        int vol;
        if (out_count < 1) out_count = frame->nb_samples;
        if (av_samples_alloc(&out, &linesize, 2, out_count, AV_SAMPLE_FMT_S16, 0) < 0) break;
        got = swr_convert(swr, &out, out_count, (const uint8_t **)frame->extended_data,
                          frame->nb_samples);
        vol = atomic_load(&g_volume);
        if (got > 0) {
          int16_t *pcm = (int16_t *)out;
          int ns = got * 2;
          for (int i = 0; i < ns; i++) {
            int s = (pcm[i] * vol) / 100;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            pcm[i] = (int16_t)s;
          }
          SDL_QueueAudio(g_dev, pcm, (Uint32)(got * 2 * (int)sizeof(int16_t)));
          atomic_fetch_add(&g_samples, (uint64_t)got);
        }
        av_freep(&out);
        av_frame_unref(frame);
      }
    }
    av_packet_unref(pkt);
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
  if (fmt) avformat_close_input(&fmt);
  if (opts) av_dict_free(&opts);
  return NULL;
}

int player_start(const char *url, int duration_s) {
  player_stop();
  if (!url || !url[0] || g_dev == 0) return -1;
  pthread_mutex_lock(&g_mu);
  snprintf(g_url, sizeof g_url, "%s", url);
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
  SDL_ClearQueuedAudio(g_dev);
  SDL_PauseAudioDevice(g_dev, 0);
  if (pthread_create(&g_thread, NULL, decode_main, NULL) != 0) {
    set_err("Could not start the decoder thread");
    return -1;
  }
  g_thread_live = 1;
  return 0;
}

void player_stop(void) {
  if (!g_thread_live) return;
  atomic_store(&g_stop, 1);
  pthread_join(g_thread, NULL);
  g_thread_live = 0;
  atomic_store(&g_stop, 0);
  if (g_dev) SDL_ClearQueuedAudio(g_dev);
}

void player_close(void) {
  player_stop();
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
  uint32_t queued = g_dev ? SDL_GetQueuedAudioSize(g_dev) : 0;
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
