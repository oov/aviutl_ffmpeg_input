#include "video.h"

#include <ovprintf.h>
#include <ovthreads.h>
#include <ovutil/win32.h>

#include "ffmpeg.h"
#include "now.h"

#define SHOWLOG_VIDEO_GET_INFO 0
#define SHOWLOG_VIDEO_GET_INTRA_INFO 0
#define SHOWLOG_VIDEO_CURRENT_FRAME 0
#define SHOWLOG_VIDEO_INIT_BENCH 0
#define SHOWLOG_VIDEO_SEEK 0
#define SHOWLOG_VIDEO_SEEK_SPEED 0
#define SHOWLOG_VIDEO_FIND_STREAM 0
#define SHOWLOG_VIDEO_READ 0

static bool const is_output_yuy2 = true;

struct stream {
  struct ffmpeg_stream ffmpeg;
  int64_t current_frame;
  int64_t current_gop_intra_pts;
  struct timespec ts;
};

enum status {
  status_nothread,
  status_running,
  status_closing,
};

struct video {
  struct stream *streams;
  size_t len;
  size_t cap;

  struct wstr filepath;
  void *handle;
  mtx_t mtx;
  thrd_t thread;
  enum status status;

  struct SwsContext *sws_context;
  bool yuy2;
};

void video_get_info(struct video const *const v, struct info_video *const vi) {
  vi->width = v->streams[0].ffmpeg.cctx->width;
  vi->height = v->streams[0].ffmpeg.cctx->height;
  vi->bit_depth = v->yuy2 ? 16 : 24;
  vi->is_rgb = !v->yuy2;
  vi->frame_rate = v->streams[0].ffmpeg.stream->avg_frame_rate.num;
  vi->frame_scale = v->streams[0].ffmpeg.stream->avg_frame_rate.den;
  vi->frames = av_rescale_q(
      v->streams[0].ffmpeg.fctx->duration, v->streams[0].ffmpeg.stream->avg_frame_rate, av_inv_q(AV_TIME_BASE_Q));
#if SHOWLOG_VIDEO_GET_INFO
  char s[256];
  ov_snprintf(s,
              256,
              "vinfo duration: %lld / frames: %lld / start_time: %lld",
              v->streams[0].ffmpeg.fctx->duration,
              vi->frames,
              v->streams[0].ffmpeg.stream->start_time);
  OutputDebugStringA(s);
#endif
}

static inline void calc_current_frame(struct stream *const stream) {
  stream->current_frame = av_rescale_q(stream->ffmpeg.frame->pts - stream->ffmpeg.stream->start_time,
                                       stream->ffmpeg.stream->avg_frame_rate,
                                       av_inv_q(stream->ffmpeg.stream->time_base));
#if SHOWLOG_VIDEO_CURRENT_FRAME
  {
    char s[256];
    ov_snprintf(s,
                256,
                "v frame: %d pts: %d key_frame: %d start_time: %d time_base:%f avg_frame_rate:%f",
                (int)stream->current_frame,
                (int)stream->ffmpeg.frame->pts,
                stream->ffmpeg.frame->key_frame ? 1 : 0,
                (int)stream->ffmpeg.stream->start_time,
                av_q2d(stream->ffmpeg.stream->time_base),
                av_q2d(stream->ffmpeg.stream->avg_frame_rate));
    OutputDebugStringA(s);
  }
#endif
  if (stream->ffmpeg.frame->key_frame) {
    stream->current_gop_intra_pts = stream->ffmpeg.frame->pts;
#if SHOWLOG_VIDEO_GET_INTRA_INFO
    {
      char s[256];
      ov_snprintf(s,
                  256,
                  "gop intra pts: %lld packet pts: %lld packet dts: %lld",
                  stream->ffmpeg.frame->pts,
                  stream->ffmpeg.packet->pts,
                  stream->ffmpeg.packet->dts);
      OutputDebugStringA(s);
      // current_gop_intra_dts
    }
#endif
  }
}

static NODISCARD error grab(struct stream *const stream) {
  error err = ffmpeg_grab(&stream->ffmpeg);
  if (efailed(err)) {
    goto cleanup;
  }
  calc_current_frame(stream);
cleanup:
  return err;
}

static NODISCARD error seek(struct stream *stream, int frame) {
#if SHOWLOG_VIDEO_SEEK_SPEED
  double const start = now();
#endif
  error err = eok();
  int64_t time_stamp =
      av_rescale_q(frame, av_inv_q(stream->ffmpeg.stream->time_base), stream->ffmpeg.stream->avg_frame_rate);
#if SHOWLOG_VIDEO_SEEK
  {
    char s[256];
    ov_snprintf(s,
                256,
                "req_pts:%lld, frame: %d tb: %f fr: %f",
                time_stamp,
                frame,
                av_q2d(av_inv_q(stream->ffmpeg.stream->time_base)),
                av_q2d(stream->ffmpeg.stream->avg_frame_rate));
    OutputDebugStringA(s);
  }
#endif
  for (;;) {
    err = ffmpeg_seek(&stream->ffmpeg, time_stamp);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
    err = grab(stream);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
    if (stream->current_frame > frame) {
      time_stamp = stream->ffmpeg.frame->pts - 1;
      continue;
    }
    break;
  }
  while (stream->current_frame < frame) {
    err = grab(stream);
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

static int create_sub_stream(void *userdata) {
  struct video *const fp = userdata;
  for (;;) {
    mtx_lock(&fp->mtx);
    size_t const cap = fp->cap;
    size_t const len = fp->len;
    enum status st = fp->status;
    mtx_unlock(&fp->mtx);
    if (st == status_closing || cap == len) {
      break;
    }
    fp->streams[len] = (struct stream){
        .current_gop_intra_pts = AV_NOPTS_VALUE,
    };
    error err = ffmpeg_open(&fp->streams[len].ffmpeg,
                            &(struct ffmpeg_open_options){
                                .filepath = fp->filepath.ptr,
                                .handle = fp->handle,
                                .media_type = AVMEDIA_TYPE_VIDEO,
                                .codec = fp->streams[0].ffmpeg.codec,
                            });
    if (efailed(err)) {
      err = ethru(err);
      ereport(err);
      break;
    }
    mtx_lock(&fp->mtx);
    ++fp->len;
    mtx_unlock(&fp->mtx);
  }
  return 0;
}

static struct stream *find_stream(struct video *const fp, int64_t const frame, bool *const need_seek) {
  struct timespec ts;
  timespec_get(&ts, TIME_UTC);
  mtx_lock(&fp->mtx);
  size_t const num_stream = fp->len;
  if (fp->status == status_nothread && fp->cap > 1) {
    if (thrd_create(&fp->thread, create_sub_stream, fp) == thrd_success) {
      fp->status = status_running;
    }
  }
  mtx_unlock(&fp->mtx);

  // find same or next frame
  for (size_t i = 0; i < num_stream; ++i) {
    struct stream *const stream = fp->streams + i;
    if (frame == stream->current_frame || frame == stream->current_frame + 1) {
      stream->ts = ts;
      *need_seek = false;
#if SHOWLOG_VIDEO_FIND_STREAM
      char s[256];
      ov_snprintf(s, 256, "find stream #%d same or next(%lld)", i, frame - nearest->current_frame);
      OutputDebugStringA(s);
#endif
      return stream;
    }
  }
  // find same gop
  if (avformat_index_get_entries_count(fp->streams[0].ffmpeg.stream) > 1) {
    int64_t const time_stamp = av_rescale_q(
        frame, av_inv_q(fp->streams[0].ffmpeg.stream->time_base), fp->streams[0].ffmpeg.stream->avg_frame_rate);
    AVIndexEntry const *const idx =
        avformat_index_get_entry_from_timestamp(fp->streams[0].ffmpeg.stream, time_stamp, AVSEEK_FLAG_BACKWARD);
    int64_t const gop_intra_pts = idx->timestamp;
    // find nearest stream
    struct stream *nearest = NULL;
    int64_t gap = INT64_MAX;
    for (size_t i = 0; i < num_stream; ++i) {
      struct stream *const stream = fp->streams + i;
      if (stream->current_gop_intra_pts != gop_intra_pts || frame < stream->current_frame) {
        continue;
      }
      if (nearest == NULL || gap > frame - stream->current_frame) {
        nearest = stream;
      }
    }
    if (nearest) {
      nearest->ts = ts;
      *need_seek = false;
#if SHOWLOG_VIDEO_FIND_STREAM
      char s[256];
      ov_snprintf(s, 256, "find stream #%d same gop(%lld)", nearest - fp->streams, frame - nearest->current_frame);
      OutputDebugStringA(s);
#endif
      return nearest;
    }
  }
  // it seems should seek, so we choose stream to use by LRU.
  // but if the frame numbers are very close, avoid seeking.
  struct stream *oldest = NULL;
  struct stream *nearest = NULL;
  for (size_t i = 0; i < num_stream; ++i) {
    struct stream *const stream = fp->streams + i;
    if (oldest == NULL || oldest->ts.tv_sec > stream->ts.tv_sec ||
        (oldest->ts.tv_sec == stream->ts.tv_sec && oldest->ts.tv_nsec > stream->ts.tv_nsec)) {
      oldest = stream;
    }
    if (stream->current_gop_intra_pts == AV_NOPTS_VALUE) {
      continue;
    }
    int64_t const dist = frame - stream->current_frame;
    if (nearest == NULL || (dist > 0 && nearest->current_frame > stream->current_frame)) {
      nearest = stream;
    }
  }
  if (nearest) {
    if (frame - nearest->current_frame < 15) {
      nearest->ts = ts;
      *need_seek = false;
#if SHOWLOG_VIDEO_FIND_STREAM
      char s[256];
      ov_snprintf(s, 256, "find stream #%d near(%lld)", nearest - fp->streams, frame - nearest->current_frame);
      OutputDebugStringA(s);
#endif
    }
  }
  oldest->ts = ts;
  *need_seek = true;
#if SHOWLOG_VIDEO_FIND_STREAM
  char s[256];
  ov_snprintf(s, 256, "find stream #%d oldest(%lld)", oldest - fp->streams, frame - oldest->current_frame);
  OutputDebugStringA(s);
#endif
  return oldest;
}

NODISCARD error video_read(struct video *const fp, int64_t frame, void *buf, size_t *written) {
  if (!fp || !fp->streams[0].ffmpeg.stream || !buf || !written) {
    return errg(err_invalid_arugment);
  }

  bool need_seek = false;
  struct stream *stream = find_stream(fp, frame, &need_seek);

  error err = eok();
#if SHOWLOG_VIDEO_READ
  {
    char s[256];
    wsprintfA(s, "reqframe: %d / now: %d", frame, (int)stream->current_frame);
    OutputDebugStringA(s);
  }
#endif

  if (need_seek) {
    err = seek(stream, (int)frame);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
  }
  int skip = (int)(frame - stream->current_frame);
#if SHOWLOG_VIDEO_READ
  {
    char s[256];
    wsprintfA(s, "skip: %d", skip);
    OutputDebugStringA(s);
  }
#endif

  for (int i = 0; i < skip; i++) {
    err = grab(stream);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
  }
  int const width = stream->ffmpeg.frame->width;
  int const height = stream->ffmpeg.frame->height;
  int const output_linesize = width * 3;
  if (fp->yuy2) {
    sws_scale(fp->sws_context,
              (const uint8_t *const *)stream->ffmpeg.frame->data,
              stream->ffmpeg.frame->linesize,
              0,
              stream->ffmpeg.frame->height,
              (uint8_t *[4]){(uint8_t *)buf, NULL, NULL, NULL},
              (int[4]){width * 2, 0, 0, 0});
    *written = (size_t)(width * height * 2);
  } else {
    sws_scale(fp->sws_context,
              (const uint8_t *const *)stream->ffmpeg.frame->data,
              stream->ffmpeg.frame->linesize,
              0,
              stream->ffmpeg.frame->height,
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
    int const f = fp->streams[0].ffmpeg.cctx->pix_fmt;
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
  return sws_getContext(fp->streams[0].ffmpeg.cctx->width,
                        fp->streams[0].ffmpeg.cctx->height,
                        fp->streams[0].ffmpeg.cctx->pix_fmt,
                        fp->streams[0].ffmpeg.cctx->width,
                        fp->streams[0].ffmpeg.cctx->height,
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
  if (v->streams) {
    if (v->status == status_running) {
      mtx_lock(&v->mtx);
      v->status = status_closing;
      mtx_unlock(&v->mtx);
      thrd_join(v->thread, NULL);
    }
    for (size_t i = 0; i < v->len; ++i) {
      ffmpeg_close(&v->streams[i].ffmpeg);
    }
    ereport(mem_free(&v->streams));
  }
  ereport(sfree(&v->filepath));
  mtx_destroy(&v->mtx);
  ereport(mem_free(vpp));
}

NODISCARD error video_create(struct video **const vpp, struct video_options const *const opt) {
  if (!vpp || *vpp || !opt || (!opt->filepath && (opt->handle == NULL || opt->handle == INVALID_HANDLE_VALUE)) ||
      !opt->num_stream) {
    return errg(err_invalid_arugment);
  }
  struct video *fp = NULL;
  error err = mem(&fp, 1, sizeof(struct video));
  if (efailed(err)) {
    ereport(err);
    return NULL;
  }
  size_t const cap = opt->num_stream;
  *fp = (struct video){
      .handle = opt->handle,
  };
  mtx_init(&fp->mtx, mtx_plain | mtx_recursive);

  if (opt->filepath) {
    err = scpy(&fp->filepath, opt->filepath);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
  }

  err = mem(&fp->streams, cap, sizeof(struct stream));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  fp->cap = cap;

#if SHOWLOG_VIDEO_INIT_BENCH
  double const start = now();
#endif
  fp->streams[0] = (struct stream){
      .current_gop_intra_pts = AV_NOPTS_VALUE,
  };
  err = ffmpeg_open(&fp->streams[0].ffmpeg,
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
  fp->len = 1;
#if SHOWLOG_VIDEO_INIT_BENCH
  {
    double const end = now();
    char s[256];
    ov_snprintf(s, 256, "v init: %0.4fs", end - start);
    OutputDebugStringA(s);
  }
#endif
  for (size_t i = 1; i < cap; ++i) {
#if SHOWLOG_VIDEO_INIT_BENCH
    double const start2 = now();
#endif
    fp->streams[i] = (struct stream){
        .current_gop_intra_pts = AV_NOPTS_VALUE,
    };
    err = ffmpeg_open(&fp->streams[i].ffmpeg,
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
    ++fp->len;
#if SHOWLOG_VIDEO_INIT_BENCH
    {
      double const end2 = now();
      char s[256];
      ov_snprintf(s, 256, "v init%d: %0.4fs", i, end2 - start2);
      OutputDebugStringA(s);
    }
#endif
  }

  err = grab(fp->streams);
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
  return av_rescale_q(v->streams[0].ffmpeg.stream->start_time, v->streams[0].ffmpeg.stream->time_base, AV_TIME_BASE_Q);
}
