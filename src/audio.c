#include "audio.h"

#include <ovprintf.h>
#include <ovutil/win32.h>

#include "audioidx.h"
#include "ffmpeg.h"
#include "now.h"

#define SHOWLOG_AUDIO_GET_INFO 0
#define SHOWLOG_AUDIO_CURRENT_FRAME 0
#define SHOWLOG_AUDIO_SEEK 0
#define SHOWLOG_AUDIO_SEEK_SPEED 0
#define SHOWLOG_AUDIO_READ 0

typedef int16_t sample_t;
static int const g_channels = 2;
static int const g_sample_format = AV_SAMPLE_FMT_S16;
static int const g_sample_size = sizeof(sample_t) * g_channels;

struct audio {
  struct ffmpeg_stream ffmpeg;
  struct audioidx *idx;

  SwrContext *swr_context;
  uint8_t *swr_buf;
  int swr_buf_len;
  int swr_buf_written;

  int current_samples;
  enum audio_index_mode index_mode;
  bool jumped;
  bool wait_index;

  int64_t video_start_time;
  int64_t current_sample_pos;
  int64_t swr_buf_sample_pos;
};

void audio_get_info(struct audio const *const a, struct info_audio *const ai) {
  ai->sample_rate = a->ffmpeg.cctx->sample_rate;
  ai->channels = g_channels;
  ai->bit_depth = sizeof(sample_t) * 8;
  ai->samples = av_rescale_q(a->ffmpeg.fctx->duration, AV_TIME_BASE_Q, av_make_q(1, a->ffmpeg.cctx->sample_rate));
#if SHOWLOG_AUDIO_GET_INFO
  char s[256];
  ov_snprintf(s,
              256,
              "ainfo duration: %lld / samples: %lld / start_time: %lld",
              a->ffmpeg.fctx->duration,
              ai->samples,
              a->ffmpeg.stream->start_time);
  OutputDebugStringA(s);
#endif
}

static inline void calc_current_frame(struct audio *a) {
  if (a->jumped) {
    int64_t pos = a->idx ? audioidx_get(a->idx, a->ffmpeg.packet->pts, a->wait_index) : -1;
    if (pos != -1) {
      // found corrent sample position
      a->current_sample_pos = pos;
    } else {
      // It seems pts value may be inaccurate.
      // There would be no way to correct the values except to recalculate from the first frame.
      // This program allows inaccurate values.
      // Instead, it avoids the accumulation of errors by not using
      // the received pts as long as it continues to read frames.
      int64_t const video_start_time = av_rescale_q(a->video_start_time, AV_TIME_BASE_Q, a->ffmpeg.stream->time_base);
      a->current_sample_pos = av_rescale_q(a->ffmpeg.frame->pts - video_start_time,
                                           a->ffmpeg.stream->time_base,
                                           av_make_q(1, a->ffmpeg.cctx->sample_rate));
    }
    a->jumped = false;
  } else {
    a->current_sample_pos += a->ffmpeg.frame->nb_samples;
  }
  a->current_samples = a->ffmpeg.frame->nb_samples;
#if SHOWLOG_AUDIO_CURRENT_FRAME
  char s[256];
  ov_snprintf(s,
              256,
              "a samplepos: %lld key_frame: %d, pts: %lld start_time: %lld time_base:%f sample_rate:%d",
              a->current_sample_pos,
              a->ffmpeg.frame->key_frame,
              a->ffmpeg.frame->pts,
              a->ffmpeg.stream->start_time,
              av_q2d(a->ffmpeg.stream->time_base),
              a->ffmpeg.cctx->sample_rate);
  OutputDebugStringA(s);
#endif
}

static NODISCARD error grab(struct audio *a) {
  error err = ffmpeg_grab(&a->ffmpeg);
  if (efailed(err)) {
    goto cleanup;
  }
  calc_current_frame(a);
cleanup:
  return err;
}

static NODISCARD error seek(struct audio *a, int64_t sample) {
#if SHOWLOG_AUDIO_SEEK_SPEED
  double const start = now();
#endif
  error err = eok();
  int64_t time_stamp =
      av_rescale_q(sample, av_inv_q(a->ffmpeg.stream->time_base), av_make_q(a->ffmpeg.cctx->sample_rate, 1));
#if SHOWLOG_AUDIO_SEEK
  {
    char s[256];
    ov_snprintf(s,
                256,
                "req_pts:%lld sample: %lld tb: %f sr: %d",
                time_stamp,
                sample,
                av_q2d(av_inv_q(a->ffmpeg.stream->time_base)),
                a->ffmpeg.cctx->sample_rate);
    OutputDebugStringA(s);
  }
#endif
  for (;;) {
    err = ffmpeg_seek(&a->ffmpeg, time_stamp);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
    a->jumped = true;
    err = grab(a);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
    if (a->current_sample_pos > sample) {
      time_stamp = a->ffmpeg.frame->pts - 1;
      continue;
    }
    break;
  }
  while (a->current_sample_pos + a->ffmpeg.frame->nb_samples < sample) {
#if SHOWLOG_AUDIO_SEEK
    char s[256];
    ov_snprintf(s, 256, "csp: %lld / smp: %lld", a->current_sample_pos + a->ffmpeg.frame->nb_samples, sample);
    OutputDebugStringA(s);
#endif
    err = grab(a);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
  }
cleanup :
#if SHOWLOG_AUDIO_SEEK_SPEED
{
  double const end = now();
  char s[256];
  ov_snprintf(s, 256, "a seek: %0.4fs", end - start);
  OutputDebugStringA(s);
}
#endif
  return err;
}

static inline int imin(int const a, int const b) { return a > b ? b : a; }

NODISCARD error audio_read(struct audio *const a,
                           int64_t const offset,
                           int const length,
                           void *const buf,
                           int *const written,
                           bool const accurate) {
#if SHOWLOG_AUDIO_READ
  char s[256];
  ov_snprintf(s, 256, "audio_read ofs: %lld / len: %d", offset, length);
  OutputDebugStringA(s);
#endif
  error err = eok();
  uint8_t *dest = buf;
  int read = 0;
  int r = 0;
  int64_t readpos = 0;

  a->wait_index = (a->index_mode == aim_strict) || accurate;

start:
  if (read == length) {
    goto cleanup;
  }

readbuf:
  readpos = offset + read;
  if (readpos >= a->swr_buf_sample_pos && readpos < (a->swr_buf_sample_pos + a->swr_buf_written)) {
    int const bufpos = (int)(readpos - a->swr_buf_sample_pos);
    int const samples = imin(a->swr_buf_written - bufpos, (int)(length - read));
    memcpy(dest + (read * g_sample_size), a->swr_buf + (bufpos * g_sample_size), (size_t)(samples * g_sample_size));
    read += samples;
    goto start;
  }

  // flushswr:
  r = swr_convert(a->swr_context, &a->swr_buf, a->swr_buf_len, NULL, 0);
  if (r < 0) {
    err = errffmpeg(r);
    goto cleanup;
  }
  if (r) {
    a->swr_buf_sample_pos += a->swr_buf_written;
    a->swr_buf_written = r;
    goto readbuf;
  }

  // seek:
  if (readpos < a->current_sample_pos || readpos >= a->current_sample_pos + a->ffmpeg.cctx->sample_rate) {
    err = seek(a, readpos);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
    goto convert;
  }
  err = grab(a);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }

convert:
  r = swr_convert(
      a->swr_context, (void *)&a->swr_buf, a->swr_buf_len, (void *)a->ffmpeg.frame->data, a->ffmpeg.frame->nb_samples);
  if (r < 0) {
    err = errffmpeg(r);
    goto cleanup;
  }
  if (r) {
    a->swr_buf_sample_pos = a->current_sample_pos;
    a->swr_buf_written = r;
    goto readbuf;
  }

cleanup:
  if (efailed(err)) {
    if (eis_errno(err, AVERROR_EOF)) {
      efree(&err);
      if (read < length) {
        memset(dest + read, 0, (size_t)((length - read) * g_sample_size));
        read = length;
      }
    }
  }
  *written = read;
  return err;
}

void audio_destroy(struct audio **const app) {
  if (!app || !*app) {
    return;
  }
  struct audio *a = *app;
  if (a->swr_context) {
    swr_free(&a->swr_context);
  }
  if (a->swr_buf) {
    av_freep(&a->swr_buf);
  }
  ffmpeg_close(&a->ffmpeg);
  audioidx_destroy(&a->idx);
  ereport(mem_free(app));
}

NODISCARD error audio_create(struct audio **const app, struct audio_options const *const opt) {
  if (!app || *app || !opt || (!opt->filepath && (opt->handle == NULL || opt->handle == INVALID_HANDLE_VALUE))) {
    return errg(err_invalid_arugment);
  }
  struct audio *a = NULL;
  error err = mem(&a, 1, sizeof(struct audio));
  if (efailed(err)) {
    ereport(err);
    return NULL;
  }
  *a = (struct audio){
      .index_mode = opt->index_mode,
      .video_start_time = opt->video_start_time,
  };

  err = ffmpeg_open(&a->ffmpeg,
                    &(struct ffmpeg_open_options){
                        .filepath = opt->filepath,
                        .handle = opt->handle,
                        .media_type = AVMEDIA_TYPE_AUDIO,
                        .preferred_decoders = opt->preferred_decoders,
                    });
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }

  a->jumped = true;
  err = grab(a);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }

  if (a->index_mode != aim_noindex) {
    err = audioidx_create(&a->idx,
                          &(struct audioidx_create_options){
                              .filepath = opt->filepath,
                              .handle = opt->handle,
                              .video_start_time = opt->video_start_time,
                          });
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
  }

  a->swr_buf_len = a->ffmpeg.cctx->sample_rate * g_channels;
  int r = av_samples_alloc(&a->swr_buf, NULL, 2, a->swr_buf_len, AV_SAMPLE_FMT_S16, 0);
  if (r < 0) {
    err = errffmpeg(r);
    goto cleanup;
  }

  r = swr_alloc_set_opts2(&a->swr_context,
                          &(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO,
                          g_sample_format,
                          a->ffmpeg.cctx->sample_rate,
                          &a->ffmpeg.frame->ch_layout,
                          a->ffmpeg.frame->format,
                          a->ffmpeg.frame->sample_rate,
                          0,
                          NULL);
  if (r < 0) {
    err = errffmpeg(r);
    goto cleanup;
  }
  // TODO: would like to be able to use sox if it supports resampling in the future.
  // av_opt_set_int(a->swr_context, "engine", SWR_ENGINE_SOXR, 0);
  r = swr_init(a->swr_context);
  if (r < 0) {
    err = errffmpeg(r);
    goto cleanup;
  }
  *app = a;
cleanup:
  if (efailed(err)) {
    audio_destroy(&a);
  }
  return err;
}

int64_t audio_get_start_time(struct audio const *const a) {
  if (!a || !a->ffmpeg.stream) {
    return AV_NOPTS_VALUE;
  }
  return av_rescale_q(a->ffmpeg.stream->start_time, a->ffmpeg.stream->time_base, AV_TIME_BASE_Q);
}
