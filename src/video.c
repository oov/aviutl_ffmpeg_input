#include "video.h"

#include <ovprintf.h>
#include <ovutil/win32.h>

#include "ffmpeg.h"
#include "now.h"

#define SHOWLOG_VIDEO_GET_INFO 0
#define SHOWLOG_VIDEO_CURRENT_FRAME 0
#define SHOWLOG_VIDEO_SEEK 0
#define SHOWLOG_VIDEO_SEEK_SPEED 0
#define SHOWLOG_VIDEO_READ 0

static bool const is_output_yuy2 = true;

struct video {
  struct ffmpeg_stream ffmpeg;
  struct SwsContext *sws_context;

  int64_t current_frame;
  bool yuy2;
};

void video_get_info(struct video const *const v, struct info_video *const vi) {
  vi->width = v->ffmpeg.cctx->width;
  vi->height = v->ffmpeg.cctx->height;
  vi->bit_depth = v->yuy2 ? 16 : 24;
  vi->is_rgb = !v->yuy2;
  vi->frame_rate = v->ffmpeg.stream->avg_frame_rate.num;
  vi->frame_scale = v->ffmpeg.stream->avg_frame_rate.den;
  vi->frames = av_rescale_q(v->ffmpeg.fctx->duration, v->ffmpeg.stream->avg_frame_rate, av_inv_q(AV_TIME_BASE_Q));
#if SHOWLOG_VIDEO_GET_INFO
  char s[256];
  ov_snprintf(s,
              256,
              "vinfo duration: %lld / frames: %lld / start_time: %lld",
              v->ffmpeg.fctx->duration,
              vi->frames,
              v->ffmpeg.stream->start_time);
  OutputDebugStringA(s);
#endif
}

static inline void calc_current_frame(struct video *fp) {
  fp->current_frame = av_rescale_q(fp->ffmpeg.frame->pts - fp->ffmpeg.stream->start_time,
                                   fp->ffmpeg.stream->avg_frame_rate,
                                   av_inv_q(fp->ffmpeg.stream->time_base));
#if SHOWLOG_VIDEO_CURRENT_FRAME
  char s[256];
  ov_snprintf(s,
              256,
              "v frame: %d pts: %d key_frame: %d start_time: %d time_base:%f avg_frame_rate:%f",
              (int)fp->current_frame,
              (int)fp->ffmpeg.frame->pts,
              fp->ffmpeg.frame->key_frame ? 1 : 0,
              (int)fp->ffmpeg.stream->start_time,
              av_q2d(fp->ffmpeg.stream->time_base),
              av_q2d(fp->ffmpeg.stream->avg_frame_rate));
  OutputDebugStringA(s);
#endif
}

static NODISCARD error grab(struct video *fp) {
  error err = ffmpeg_grab(&fp->ffmpeg);
  if (efailed(err)) {
    goto cleanup;
  }
  calc_current_frame(fp);
cleanup:
  return err;
}

static NODISCARD error seek(struct video *fp, int frame) {
#if SHOWLOG_VIDEO_SEEK_SPEED
  double const start = now();
#endif
  error err = eok();
  int64_t time_stamp = av_rescale_q(frame, av_inv_q(fp->ffmpeg.stream->time_base), fp->ffmpeg.stream->avg_frame_rate);
#if SHOWLOG_VIDEO_SEEK
  char s[256];
  ov_snprintf(s,
              256,
              "req_pts:%lld, frame: %d tb: %f fr: %f",
              time_stamp,
              frame,
              av_q2d(av_inv_q(fp->ffmpeg.stream->time_base)),
              av_q2d(fp->ffmpeg.stream->avg_frame_rate));
  OutputDebugStringA(s);
#endif
  for (;;) {
    err = ffmpeg_seek(&fp->ffmpeg, time_stamp);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
    err = grab(fp);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
    if (fp->current_frame > frame) {
      time_stamp = fp->ffmpeg.frame->pts - 1;
      continue;
    }
    break;
  }
  while (fp->current_frame < frame) {
    err = grab(fp);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
  }
cleanup :
#if SHOWLOG_VIDEO_SEEK_SPEED
{
  double const end = now();
  char s[256];
  ov_snprintf(s, 256, "v seek: %0.4fs", end - start);
  OutputDebugStringA(s);
}
#endif
  return err;
}

NODISCARD error video_read(struct video *const fp, int64_t frame, void *buf, size_t *written) {
  if (!fp || !fp->ffmpeg.stream || !buf || !written) {
    return errg(err_invalid_arugment);
  }

  error err = eok();
#if SHOWLOG_VIDEO_READ
  char s[256];
  wsprintfA(s, "reqframe: %d / now: %d", frame, (int)fp->current_frame);
  OutputDebugStringA(s);
#endif
  int64_t skip = frame - fp->current_frame;
  if (skip > 100 || skip < 0) {
    err = seek(fp, (int)frame);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
    skip = frame - fp->current_frame;
  }
  for (int i = 0; i < skip; i++) {
    err = grab(fp);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
  }
  int const width = fp->ffmpeg.frame->width;
  int const height = fp->ffmpeg.frame->height;
  int const output_linesize = width * 3;
  if (fp->yuy2) {
    sws_scale(fp->sws_context,
              (const uint8_t *const *)fp->ffmpeg.frame->data,
              fp->ffmpeg.frame->linesize,
              0,
              fp->ffmpeg.frame->height,
              (uint8_t *[4]){(uint8_t *)buf, NULL, NULL, NULL},
              (int[4]){width * 2, 0, 0, 0});
    *written = (size_t)(width * height * 2);
  } else {
    sws_scale(fp->sws_context,
              (const uint8_t *const *)fp->ffmpeg.frame->data,
              fp->ffmpeg.frame->linesize,
              0,
              fp->ffmpeg.frame->height,
              (uint8_t *[4]){(uint8_t *)buf + output_linesize * (height - 1), NULL, NULL, NULL},
              (int[4]){-(output_linesize), 0, 0, 0});
    *written = (size_t)(width * height * 3);
  }
cleanup:
  return err;
}

static inline struct SwsContext *create_sws_context(struct video *fp, enum video_format_scaling_algorithm scaling) {
  int pix_format = AV_PIX_FMT_BGR24;
  if (is_output_yuy2) {
    int const f = fp->ffmpeg.cctx->pix_fmt;
    if (f != AV_PIX_FMT_RGB24 && f != AV_PIX_FMT_RGB32 && f != AV_PIX_FMT_RGBA && f != AV_PIX_FMT_BGR0 &&
        f != AV_PIX_FMT_BGR24 && f != AV_PIX_FMT_ARGB && f != AV_PIX_FMT_ABGR && f != AV_PIX_FMT_GBRP) {
      pix_format = AV_PIX_FMT_YUYV422;
      fp->yuy2 = true;
    }
  }
  int sws_flags = 0;
  switch (scaling) {
  case video_format_scaling_algorithm_fast_bilinear:
    sws_flags |= SWS_FAST_BILINEAR;
    break;
  case video_format_scaling_algorithm_bilinear:
    sws_flags |= SWS_BILINEAR;
    break;
  case video_format_scaling_algorithm_bicubic:
    sws_flags |= SWS_BICUBIC;
    break;
  case video_format_scaling_algorithm_x:
    sws_flags |= SWS_X;
    break;
  case video_format_scaling_algorithm_point:
    sws_flags |= SWS_POINT;
    break;
  case video_format_scaling_algorithm_area:
    sws_flags |= SWS_AREA;
    break;
  case video_format_scaling_algorithm_bicublin:
    sws_flags |= SWS_BICUBLIN;
    break;
  case video_format_scaling_algorithm_gauss:
    sws_flags |= SWS_GAUSS;
    break;
  case video_format_scaling_algorithm_sinc:
    sws_flags |= SWS_SINC;
    break;
  case video_format_scaling_algorithm_lanczos:
    sws_flags |= SWS_LANCZOS;
    break;
  case video_format_scaling_algorithm_spline:
    sws_flags |= SWS_SPLINE;
    break;
  }
  return sws_getContext(fp->ffmpeg.cctx->width,
                        fp->ffmpeg.cctx->height,
                        fp->ffmpeg.cctx->pix_fmt,
                        fp->ffmpeg.cctx->width,
                        fp->ffmpeg.cctx->height,
                        pix_format,
                        sws_flags,
                        NULL,
                        NULL,
                        NULL);
}

void video_destroy(struct video **const vpp) {
  if (!vpp || !*vpp) {
    return;
  }
  struct video *v = *vpp;
  if (v->sws_context) {
    sws_freeContext(v->sws_context);
  }
  ffmpeg_close(&v->ffmpeg);
  ereport(mem_free(vpp));
}

NODISCARD error video_create(struct video **const vpp, struct video_options const *const opt) {
  if (!vpp || *vpp || !opt || (!opt->filepath && (opt->handle == NULL || opt->handle == INVALID_HANDLE_VALUE))) {
    return errg(err_invalid_arugment);
  }
  struct video *fp = NULL;
  error err = mem(&fp, 1, sizeof(struct video));
  if (efailed(err)) {
    ereport(err);
    return NULL;
  }
  *fp = (struct video){0};
  err = ffmpeg_open(&fp->ffmpeg,
                    &(struct ffmpeg_open_options){
                        .filepath = opt->filepath,
                        .handle = opt->handle,
                        .media_type = AVMEDIA_TYPE_VIDEO,
                        .preferred_decoders = opt->preferred_decoders,
                    });
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  err = grab(fp);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  fp->sws_context = create_sws_context(fp, opt->scaling);
  if (!fp->sws_context) {
    err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("sws_getContext failed")));
    goto cleanup;
  }
  *vpp = fp;
cleanup:
  if (efailed(err)) {
    video_destroy(&fp);
  }
  return err;
}

int64_t video_get_start_time(struct video const *const v) {
  if (!v) {
    return AV_NOPTS_VALUE;
  }
  return av_rescale_q(v->ffmpeg.stream->start_time, v->ffmpeg.stream->time_base, AV_TIME_BASE_Q);
}
