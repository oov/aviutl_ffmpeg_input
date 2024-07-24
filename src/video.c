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
#define SHOWLOG_VIDEO_SEEK_ADJUST 0
#define SHOWLOG_VIDEO_SEEK_SPEED 0
#define SHOWLOG_VIDEO_FIND_STREAM 0
#define SHOWLOG_VIDEO_READ 0

static bool const is_output_yuy2 = true;

struct stream {
  struct ffmpeg_stream ffmpeg;
  int64_t current_frame;
  int64_t current_gop_intra_pts;
  struct timespec ts;
  bool eof_reached;
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

static size_t scale(struct video *const v, struct stream *stream, void *buf) {
  int const width = stream->ffmpeg.cctx->width;
  int const height = stream->ffmpeg.cctx->height;
  int const output_linesize = width * 3;
  if (v->yuy2) {
    sws_scale(v->sws_context,
              (const uint8_t *const *)stream->ffmpeg.frame->data,
              stream->ffmpeg.frame->linesize,
              0,
              height,
              (uint8_t *[4]){(uint8_t *)buf, NULL, NULL, NULL},
              (int[4]){width * 2, 0, 0, 0});
    return (size_t)(width * height * 2);
  }
  sws_scale(v->sws_context,
            (const uint8_t *const *)stream->ffmpeg.frame->data,
            stream->ffmpeg.frame->linesize,
            0,
            height,
            (uint8_t *[4]){(uint8_t *)buf + output_linesize * (height - 1), NULL, NULL, NULL},
            (int[4]){-(output_linesize), 0, 0, 0});
  return (size_t)(width * height * 3);
}

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
              NULL,
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
                                       av_inv_q(stream->ffmpeg.cctx->pkt_timebase));
#if SHOWLOG_VIDEO_CURRENT_FRAME
  {
    char s[256];
    ov_snprintf(s,
                256,
                NULL,
                "v frame: %d pts: %d key_frame: %d start_time: %d time_base:%f avg_frame_rate:%f",
                (int)stream->current_frame,
                (int)stream->ffmpeg.frame->pts,
                stream->ffmpeg.frame->flags & AV_FRAME_FLAG_KEY ? 1 : 0,
                (int)stream->ffmpeg.stream->start_time,
                av_q2d(stream->ffmpeg.cctx->pkt_timebase),
                av_q2d(stream->ffmpeg.stream->avg_frame_rate));
    OutputDebugStringA(s);
  }
#endif
  if (stream->ffmpeg.frame->flags & AV_FRAME_FLAG_KEY) {
    stream->current_gop_intra_pts = stream->ffmpeg.frame->pts;
#if SHOWLOG_VIDEO_GET_INTRA_INFO
    {
      char s[256];
      ov_snprintf(s,
                  256,
                  NULL,
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
  if (stream->eof_reached) {
    return eok();
  }
  int const r = ffmpeg_grab(&stream->ffmpeg);
  if (r == AVERROR_EOF) {
    stream->eof_reached = true;
    return eok();
  }
  if (r < 0) {
    return errffmpeg(r);
  }
  calc_current_frame(stream);
  return eok();
}

static NODISCARD error seek(struct stream *stream, int frame) {
#if SHOWLOG_VIDEO_SEEK_SPEED
  double const start = now();
#endif
  error err = eok();
  int64_t time_stamp =
      av_rescale_q(frame, av_inv_q(stream->ffmpeg.cctx->pkt_timebase), stream->ffmpeg.stream->avg_frame_rate);
  if (stream->ffmpeg.stream->start_time != AV_NOPTS_VALUE) {
    time_stamp += stream->ffmpeg.stream->start_time;
  }
  for (;;) {
#if SHOWLOG_VIDEO_SEEK
    {
      char s[256];
      ov_snprintf(s,
                  256,
                  NULL,
                  "req_pts:%lld, frame: %d tb: %f fr: %f",
                  time_stamp,
                  frame,
                  av_q2d(av_inv_q(stream->ffmpeg.cctx->pkt_timebase)),
                  av_q2d(stream->ffmpeg.stream->avg_frame_rate));
      OutputDebugStringA(s);
    }
#endif
    err = ffmpeg_seek(&stream->ffmpeg, time_stamp);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
    stream->eof_reached = false;
    err = grab(stream);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
    if (stream->eof_reached) {
#if SHOWLOG_VIDEO_SEEK
      OutputDebugStringA("video_seek reach eof 1");
#endif
      err = errffmpeg(AVERROR_EOF);
      goto cleanup;
    }
    if (stream->current_frame > frame) {
#if SHOWLOG_VIDEO_SEEK_ADJUST
      {
        char s[256];
        ov_snprintf(s,
                    256,
                    NULL,
                    "v adjust target: %d current: %lld rewind: %f",
                    frame,
                    stream->current_frame,
                    av_q2d(av_inv_q(stream->ffmpeg.cctx->pkt_timebase)));
        OutputDebugStringA(s);
      }
#endif
      // rewind 1s
      time_stamp -= (int64_t)(av_q2d(av_inv_q(stream->ffmpeg.cctx->pkt_timebase)));
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
    if (stream->eof_reached) {
#if SHOWLOG_VIDEO_SEEK
      OutputDebugStringA("video_seek reach eof 2");
#endif
      // There is no hope of reaching the requested frame
      // Use the last grabbed frame as it is
      goto cleanup;
    }
  }
cleanup:
#if SHOWLOG_VIDEO_SEEK_SPEED
{
  double const end = now();
  char s[256];
  ov_snprintf(s, 256, NULL, "v seek: %0.4fs", end - start);
  OutputDebugStringA(s);
}
#endif
  return err;
}

static int create_sub_stream(void *userdata) {
  struct video *const v = userdata;
  for (;;) {
    mtx_lock(&v->mtx);
    size_t const cap = v->cap;
    size_t const len = v->len;
    enum status st = v->status;
    mtx_unlock(&v->mtx);
    if (st == status_closing || cap == len) {
      break;
    }
    v->streams[len] = (struct stream){
        .current_gop_intra_pts = AV_NOPTS_VALUE,
    };
    error err = ffmpeg_open(&v->streams[len].ffmpeg,
                            &(struct ffmpeg_open_options){
                                .filepath = v->filepath.ptr,
                                .handle = v->handle,
                                .media_type = AVMEDIA_TYPE_VIDEO,
                                .codec = v->streams[0].ffmpeg.codec,
                            });
    if (efailed(err)) {
      err = ethru(err);
      ereport(err);
      break;
    }
    mtx_lock(&v->mtx);
    ++v->len;
    mtx_unlock(&v->mtx);
  }
  return 0;
}

static struct stream *find_stream(struct video *const v, int64_t const frame, bool *const need_seek) {
  struct timespec ts;
  timespec_get(&ts, TIME_UTC);
  mtx_lock(&v->mtx);
  size_t const num_stream = v->len;
  if (v->status == status_nothread && v->cap > 1) {
    if (thrd_create(&v->thread, create_sub_stream, v) == thrd_success) {
      v->status = status_running;
    }
  }
  mtx_unlock(&v->mtx);

  // find same frame or near
  {
    struct stream *nearest = NULL;
    int64_t dist = INT64_MAX;
    for (size_t i = 0; i < num_stream; ++i) {
      struct stream *const stream = v->streams + i;
      if (frame == stream->current_frame) {
        stream->ts = ts;
        *need_seek = false;
#if SHOWLOG_VIDEO_FIND_STREAM
        char s[256];
        ov_snprintf(s, 256, NULL, "find stream #%zu same or next(%lld)", i, frame - stream->current_frame);
        OutputDebugStringA(s);
#endif
        return stream;
      }
      if (stream->current_gop_intra_pts == AV_NOPTS_VALUE) {
        continue;
      }
      int64_t const d = frame - stream->current_frame;
      if (d > 0 && dist > d) {
        nearest = stream;
        dist = d;
      }
    }
    if (nearest) {
      if (frame - nearest->current_frame < 15) {
        nearest->ts = ts;
        *need_seek = false;
#if SHOWLOG_VIDEO_FIND_STREAM
        char s[256];
        ov_snprintf(s, 256, NULL, "find stream #%zu near(%lld)", nearest - v->streams, frame - nearest->current_frame);
        OutputDebugStringA(s);
#endif
        return nearest;
      }
    }
  }

  // find same gop
  if (avformat_index_get_entries_count(v->streams[0].ffmpeg.stream) > 1) {
    int64_t time_stamp = av_rescale_q(
        frame, av_inv_q(v->streams[0].ffmpeg.cctx->pkt_timebase), v->streams[0].ffmpeg.stream->avg_frame_rate);
    if (v->streams[0].ffmpeg.stream->start_time != AV_NOPTS_VALUE) {
      time_stamp += v->streams[0].ffmpeg.stream->start_time;
    }
    AVIndexEntry const *const idx =
        avformat_index_get_entry_from_timestamp(v->streams[0].ffmpeg.stream, time_stamp, AVSEEK_FLAG_BACKWARD);
    if (idx) {
      int64_t const gop_intra_pts = idx->timestamp;
      // find nearest stream
      struct stream *nearest = NULL;
      int64_t gap = INT64_MAX;
      for (size_t i = 0; i < num_stream; ++i) {
        struct stream *const stream = v->streams + i;
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
        ov_snprintf(
            s, 256, NULL, "find stream #%zu same gop(%lld)", nearest - v->streams, frame - nearest->current_frame);
        OutputDebugStringA(s);
#endif
        return nearest;
      }
    }
  }
  // it seems should seek, so we choose stream to use by LRU.
  // but if the frame numbers are very close, avoid seeking.
  struct stream *oldest = NULL;
  for (size_t i = 0; i < num_stream; ++i) {
    struct stream *const stream = v->streams + i;
    if (oldest == NULL || oldest->ts.tv_sec > stream->ts.tv_sec ||
        (oldest->ts.tv_sec == stream->ts.tv_sec && oldest->ts.tv_nsec > stream->ts.tv_nsec)) {
      oldest = stream;
    }
  }
  oldest->ts = ts;
  *need_seek = true;
#if SHOWLOG_VIDEO_FIND_STREAM
  char s[256];
  ov_snprintf(s, 256, NULL, "find stream #%zu oldest(%lld)", oldest - v->streams, frame - oldest->current_frame);
  OutputDebugStringA(s);
#endif
  return oldest;
}

NODISCARD error video_read(struct video *const v, int64_t frame, void *buf, size_t *written) {
  if (!v || !v->streams[0].ffmpeg.stream || !buf || !written) {
    return errg(err_invalid_arugment);
  }

  bool need_seek = false;
  struct stream *stream = find_stream(v, frame, &need_seek);

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
    if (stream->eof_reached) {
      stream->current_frame = frame;
    }
  }
  int skip = (int)(frame - stream->current_frame);
#if SHOWLOG_VIDEO_READ
  {
    char s[256];
    wsprintfA(s, "current_frame: %d target_frame: %d skip: %d", (int)stream->current_frame, (int)frame, skip);
    OutputDebugStringA(s);
  }
#endif

  for (int i = 0; i < skip; i++) {
    err = grab(stream);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
    if (stream->eof_reached) {
#if SHOWLOG_VIDEO_READ
      OutputDebugStringA("video_read reach eof");
#endif
      // There is no hope of reaching the requested frame
      // Use the last grabbed frame as it is
      break;
    }
  }
  *written = scale(v, stream, buf);
cleanup:
  return err;
}

static inline struct SwsContext *create_sws_context(struct video *v, enum video_format_scaling_algorithm scaling) {
  int pix_format = AV_PIX_FMT_BGR24;
  if (is_output_yuy2) {
    int const f = v->streams[0].ffmpeg.cctx->pix_fmt;
    if (f != AV_PIX_FMT_RGB24 && f != AV_PIX_FMT_RGB32 && f != AV_PIX_FMT_RGBA && f != AV_PIX_FMT_BGR0 &&
        f != AV_PIX_FMT_BGR24 && f != AV_PIX_FMT_ARGB && f != AV_PIX_FMT_ABGR && f != AV_PIX_FMT_GBRP) {
      pix_format = AV_PIX_FMT_YUYV422;
      v->yuy2 = true;
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
#if SHOWLOG_VIDEO_GET_INFO
  {
    char s[256];
    ov_snprintf(s,
                256,
                NULL,
                "sws_flags: %d pix_format: %d width: %d height: %d pix_fmt: %d",
                sws_flags,
                pix_format,
                v->streams[0].ffmpeg.cctx->width,
                v->streams[0].ffmpeg.cctx->height,
                v->streams[0].ffmpeg.cctx->pix_fmt);
    OutputDebugStringA(s);
  }
#endif
  return sws_getContext(v->streams[0].ffmpeg.cctx->width,
                        v->streams[0].ffmpeg.cctx->height,
                        v->streams[0].ffmpeg.cctx->pix_fmt,
                        v->streams[0].ffmpeg.cctx->width,
                        v->streams[0].ffmpeg.cctx->height,
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
  struct video *v = NULL;
  error err = mem(&v, 1, sizeof(struct video));
  if (efailed(err)) {
    ereport(err);
    return NULL;
  }
  size_t const cap = opt->num_stream;
  *v = (struct video){
      .handle = opt->handle,
  };
  mtx_init(&v->mtx, mtx_plain);

  if (opt->filepath) {
    err = scpy(&v->filepath, opt->filepath);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
  }

  err = mem(&v->streams, cap, sizeof(struct stream));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  v->cap = cap;

#if SHOWLOG_VIDEO_INIT_BENCH
  double const start = now();
#endif
  v->streams[0] = (struct stream){
      .current_gop_intra_pts = AV_NOPTS_VALUE,
  };
  err = ffmpeg_open(&v->streams[0].ffmpeg,
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
  v->len = 1;
#if SHOWLOG_VIDEO_INIT_BENCH
  {
    double const end = now();
    char s[256];
    ov_snprintf(s, 256, NULL, "v init: %0.4fs", end - start);
    OutputDebugStringA(s);
  }
#endif

  calc_current_frame(v->streams);
  v->sws_context = create_sws_context(v, opt->scaling);
  if (!v->sws_context) {
    err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("sws_getContext failed")));
    goto cleanup;
  }

  *vpp = v;
cleanup:
  if (efailed(err)) {
    video_destroy(&v);
  }
  return err;
}

int64_t video_get_start_time(struct video const *const v) {
  if (!v || !v->streams[0].ffmpeg.stream || v->streams[0].ffmpeg.stream->start_time == AV_NOPTS_VALUE) {
    return AV_NOPTS_VALUE;
  }
  return av_rescale_q(v->streams[0].ffmpeg.stream->start_time, v->streams[0].ffmpeg.cctx->pkt_timebase, AV_TIME_BASE_Q);
}
