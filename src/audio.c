#include "audio.h"

#ifndef NDEBUG
#  include <ovprintf.h>
#  include <ovutil/win32.h>
#endif

#include "audioidx.h"
#include "ffmpeg.h"

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
  bool jumped;

  int64_t video_start_time;
  int64_t current_sample_pos;
  int64_t swr_buf_sample_pos;
};

static inline void get_info(struct audio const *const a, struct info_audio *const ai) {
  ai->sample_rate = a->ffmpeg.cctx->sample_rate;
  ai->channels = g_channels;
  ai->bit_depth = sizeof(sample_t) * 8;
  ai->samples = av_rescale_q(a->ffmpeg.fctx->duration, AV_TIME_BASE_Q, av_make_q(1, a->ffmpeg.cctx->sample_rate));
#ifndef NDEBUG
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

static inline void calc_current_frame(struct audio *fp) {
  if (fp->jumped) {
    int64_t pos = fp->idx ? audioidx_get(fp->idx, fp->ffmpeg.packet->pts) : -1;
    if (pos != -1) {
      // found corrent sample position
      fp->current_sample_pos = pos;
    } else {
      // It seems pts value may be inaccurate.
      // There would be no way to correct the values except to recalculate from the first frame.
      // This program allows inaccurate values.
      // Instead, it avoids the accumulation of errors by not using
      // the received pts as long as it continues to read frames.
      int64_t const video_start_time = av_rescale_q(fp->video_start_time, AV_TIME_BASE_Q, fp->ffmpeg.stream->time_base);
      fp->current_sample_pos = av_rescale_q(fp->ffmpeg.frame->pts - video_start_time,
                                            fp->ffmpeg.stream->time_base,
                                            av_make_q(1, fp->ffmpeg.cctx->sample_rate));
    }
    fp->jumped = false;
  } else {
    fp->current_sample_pos += fp->ffmpeg.frame->nb_samples;
  }
  fp->current_samples = fp->ffmpeg.frame->nb_samples;
#ifndef NDEBUG
  char s[256];
  ov_snprintf(s,
              256,
              "a samplepos: %lld key_frame: %d, pts: %lld start_time: %lld time_base:%f sample_rate:%d",
              fp->current_sample_pos,
              fp->ffmpeg.frame->key_frame,
              fp->ffmpeg.frame->pts,
              fp->ffmpeg.stream->start_time,
              av_q2d(fp->ffmpeg.stream->time_base),
              fp->ffmpeg.cctx->sample_rate);
  OutputDebugStringA(s);
#endif
}

static NODISCARD error grab(struct audio *fp) {
  error err = ffmpeg_grab(&fp->ffmpeg);
  if (efailed(err)) {
    goto cleanup;
  }
  calc_current_frame(fp);
cleanup:
  return err;
}

static NODISCARD error seek(struct audio *fp, int64_t sample) {
  error err = eok();
  int64_t time_stamp =
      av_rescale_q(sample, av_inv_q(fp->ffmpeg.stream->time_base), av_make_q(fp->ffmpeg.cctx->sample_rate, 1));
#ifndef NDEBUG
  {
    char s[256];
    ov_snprintf(s,
                256,
                "req_pts:%lld sample: %lld tb: %f sr: %d",
                time_stamp,
                sample,
                av_q2d(av_inv_q(fp->ffmpeg.stream->time_base)),
                fp->ffmpeg.cctx->sample_rate);
    OutputDebugStringA(s);
  }
#endif
  for (;;) {
    err = ffmpeg_seek(&fp->ffmpeg, time_stamp);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
    fp->jumped = true;
    err = grab(fp);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
    if (fp->current_sample_pos > sample) {
      time_stamp = fp->ffmpeg.frame->pts - 1;
      continue;
    }
    break;
  }
  while (fp->current_sample_pos + fp->ffmpeg.frame->nb_samples < sample) {
#ifndef NDEBUG
    char s[256];
    ov_snprintf(s, 256, "csp: %lld / smp: %lld", fp->current_sample_pos + fp->ffmpeg.frame->nb_samples, sample);
    OutputDebugStringA(s);
#endif
    err = grab(fp);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
  }
cleanup:
  return err;
}

static inline int imin(int const a, int const b) { return a > b ? b : a; }

NODISCARD error
audio_read(struct audio *const fp, int64_t const offset, int const length, void *const buf, int *const written) {
#ifndef NDEBUG
  char s[256];
  ov_snprintf(s, 256, "audio_read ofs: %lld / len: %d", offset, length);
  OutputDebugStringA(s);
#endif
  error err = eok();
  uint8_t *dest = buf;
  int read = 0;
  int r = 0;
  int64_t readpos = 0;

start:
  if (read == length) {
    goto cleanup;
  }

readbuf:
  readpos = offset + read;
  if (readpos >= fp->swr_buf_sample_pos && readpos < (fp->swr_buf_sample_pos + fp->swr_buf_written)) {
    int const bufpos = (int)(readpos - fp->swr_buf_sample_pos);
    int const samples = imin(fp->swr_buf_written - bufpos, (int)(length - read));
    memcpy(dest + (read * g_sample_size), fp->swr_buf + (bufpos * g_sample_size), (size_t)(samples * g_sample_size));
    read += samples;
    goto start;
  }

  // flushswr:
  r = swr_convert(fp->swr_context, &fp->swr_buf, fp->swr_buf_len, NULL, 0);
  if (r < 0) {
    err = errffmpeg(r);
    goto cleanup;
  }
  if (r) {
    fp->swr_buf_sample_pos += fp->swr_buf_written;
    fp->swr_buf_written = r;
    goto readbuf;
  }

  // seek:
  if (readpos < fp->current_sample_pos || readpos >= fp->current_sample_pos + fp->ffmpeg.cctx->sample_rate) {
    err = seek(fp, readpos);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
    goto convert;
  }
  err = grab(fp);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }

convert:
  r = swr_convert(fp->swr_context,
                  (void *)&fp->swr_buf,
                  fp->swr_buf_len,
                  (void *)fp->ffmpeg.frame->data,
                  fp->ffmpeg.frame->nb_samples);
  if (r < 0) {
    err = errffmpeg(r);
    goto cleanup;
  }
  if (r) {
    fp->swr_buf_sample_pos = fp->current_sample_pos;
    fp->swr_buf_written = r;
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

NODISCARD error audio_create(struct audio **const app,
                             struct info_audio *const ai,
                             struct audio_options const *const opt) {
  if (!app || *app || !ai || !opt || !opt->filepath) {
    return errg(err_invalid_arugment);
  }
  struct audio *fp = NULL;
  error err = mem(&fp, 1, sizeof(struct audio));
  if (efailed(err)) {
    ereport(err);
    return NULL;
  }
  *fp = (struct audio){
      .video_start_time = opt->video_start_time,
  };
  err = ffmpeg_open(&fp->ffmpeg, opt->filepath, AVMEDIA_TYPE_AUDIO, opt->preferred_decoders);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }

  fp->jumped = true;
  err = grab(fp);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }

  if (opt->use_audio_index) {
    err = audioidx_create(&fp->idx, opt->filepath, opt->video_start_time);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
  }

  fp->swr_buf_len = fp->ffmpeg.cctx->sample_rate * g_channels;
  int r = av_samples_alloc(&fp->swr_buf, NULL, 2, fp->swr_buf_len, AV_SAMPLE_FMT_S16, 0);
  if (r < 0) {
    err = errffmpeg(r);
    goto cleanup;
  }

  r = swr_alloc_set_opts2(&fp->swr_context,
                          &(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO,
                          g_sample_format,
                          fp->ffmpeg.cctx->sample_rate,
                          &fp->ffmpeg.frame->ch_layout,
                          fp->ffmpeg.frame->format,
                          fp->ffmpeg.frame->sample_rate,
                          0,
                          NULL);
  if (r < 0) {
    err = errffmpeg(r);
    goto cleanup;
  }
  // TODO: would like to be able to use sox if it supports resampling in the future.
  // av_opt_set_int(fp->swr_context, "engine", SWR_ENGINE_SOXR, 0);
  r = swr_init(fp->swr_context);
  if (r < 0) {
    err = errffmpeg(r);
    goto cleanup;
  }

cleanup:
  if (efailed(err)) {
    audio_destroy(&fp);
  } else {
    *app = fp;
    get_info(fp, ai);
  }
  return err;
}

int64_t audio_get_start_time(struct audio *const a) {
  if (!a || !a->ffmpeg.stream) {
    return AV_NOPTS_VALUE;
  }
  return av_rescale_q(a->ffmpeg.stream->start_time, a->ffmpeg.stream->time_base, AV_TIME_BASE_Q);
}
