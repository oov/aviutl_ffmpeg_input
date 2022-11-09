#include "video.h"

#ifndef NDEBUG
#  include <ovprintf.h>
#  include <ovutil/win32.h>
#endif

#include "ffmpeg.h"

static bool is_output_yuy2 = true;

struct video {
  AVFormatContext *format_context;

  AVStream *stream;
  AVCodec const *codec;
  AVCodecContext *codec_context;
  AVFrame *frame;
  AVPacket *packet;

  struct SwsContext *sws_context;

  int64_t current_frame;
  bool yuy2;
};

static inline void get_info(struct video const *const v, struct info_video *const vi) {
  vi->width = v->codec_context->width;
  vi->height = v->codec_context->height;
  vi->bit_depth = v->yuy2 ? 16 : 24;
  vi->is_rgb = !v->yuy2;
  vi->frame_rate = v->stream->avg_frame_rate.num;
  vi->frame_scale = v->stream->avg_frame_rate.den;
  vi->frames = av_rescale_q(v->format_context->duration, v->stream->avg_frame_rate, av_inv_q(AV_TIME_BASE_Q));
#ifndef NDEBUG
  char s[256];
  ov_snprintf(s, 256, "v duration: %lld, samples: %lld", v->format_context->duration, vi->frames);
  OutputDebugStringA(s);
#endif
}

static inline void calc_current_frame(struct video *fp) {
  fp->current_frame = av_rescale_q(
      fp->frame->pts - fp->stream->start_time, fp->stream->avg_frame_rate, av_inv_q(fp->stream->time_base));
#ifndef NDEBUG
  char s[256];
  ov_snprintf(s,
              256,
              "frame: %d key_frame: %d, pts: %d start_time: %d time_base:%f avg_frame_rate:%f",
              (int)fp->current_frame,
              fp->frame->key_frame,
              (int)fp->frame->pts,
              (int)fp->stream->start_time,
              av_q2d(fp->stream->time_base),
              av_q2d(fp->stream->avg_frame_rate));
  OutputDebugStringA(s);
#endif
}

static NODISCARD error grab(struct video *fp) {
  error err = eok();
  int r = 0;
receive_frame:
  r = avcodec_receive_frame(fp->codec_context, fp->frame);
  switch (r) {
  case 0:
    goto cleanup;
  case AVERROR(EAGAIN):
  case AVERROR_EOF:
  case AVERROR_INPUT_CHANGED:
    break;
  default:
    err = errffmpeg(r);
    goto cleanup;
  }
read_frame:
  av_packet_unref(fp->packet);
  r = av_read_frame(fp->format_context, fp->packet);
  if (r < 0) {
    // flush
    r = avcodec_send_packet(fp->codec_context, NULL);
    switch (r) {
    case 0:
    case AVERROR(EAGAIN):
      goto receive_frame;
    case AVERROR_EOF:
      err = errffmpeg(r); // decoder has been flushed
      goto cleanup;
    default:
      err = errffmpeg(r);
      goto cleanup;
    }
  }
  if (fp->packet->stream_index != fp->stream->index) {
    goto read_frame;
  }
  r = avcodec_send_packet(fp->codec_context, fp->packet);
  switch (r) {
  case 0:
    goto receive_frame;
  case AVERROR(EAGAIN):
    // not ready to accept avcodec_send_packet, must call avcodec_receive_frame.
    goto receive_frame;
  default:
    err = errffmpeg(r);
    goto cleanup;
  }
cleanup:
  if (efailed(err)) {
    av_packet_unref(fp->packet);
  } else {
    calc_current_frame(fp);
  }
  return err;
}

static NODISCARD error jump(struct video *fp, int64_t frame) {
  error err = eok();
  int64_t time_stamp = av_rescale_q(frame, av_inv_q(AV_TIME_BASE_Q), fp->stream->avg_frame_rate);
#ifndef NDEBUG
  int64_t pts = av_rescale_q(time_stamp, AV_TIME_BASE_Q, fp->stream->time_base);
  char s[256];
  ov_snprintf(s,
              256,
              "req_ts:%lld pts:%lld, frame: %lld tb: %f tb2: %f fr: %f",
              time_stamp,
              pts,
              frame,
              av_q2d(av_inv_q(AV_TIME_BASE_Q)),
              av_q2d(av_inv_q(fp->stream->time_base)),
              av_q2d(fp->stream->avg_frame_rate));
  OutputDebugStringA(s);
#endif

  int r = avformat_seek_file(fp->format_context, -1, INT64_MIN, time_stamp, INT64_MAX, 0);
  if (r < 0) {
    err = errffmpeg(r);
    goto cleanup;
  }
  avcodec_flush_buffers(fp->codec_context);
  for (;;) {
    err = grab(fp);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
    if (!fp->frame->key_frame) {
#ifndef NDEBUG
      OutputDebugStringA("not keyframe so skipped");
#endif
      continue;
    }
    break;
  }
cleanup:
  return err;
}

static NODISCARD error seek(struct video *fp, int frame) {
  int f = frame;
  error err = jump(fp, f);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  while (fp->current_frame > frame && f != 0) {
    f -= 30;
    if (f < 0) {
      f = 0;
    }
#ifndef NDEBUG
    char s[256];
    wsprintfA(s, "re-jump: %d", f);
    OutputDebugStringA(s);
#endif
    err = jump(fp, f);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
  }
  while (fp->current_frame < frame) {
    err = grab(fp);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
  }
cleanup:
  return err;
}

NODISCARD error video_read(struct video *const fp, int64_t frame, void *buf, size_t *written) {
  if (!fp || !fp->stream || !buf || !written) {
    return errg(err_invalid_arugment);
  }

  error err = eok();
#ifndef NDEBUG
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
  int const width = fp->frame->width;
  int const height = fp->frame->height;
  int const output_linesize = width * 3;
  if (fp->yuy2) {
    sws_scale(fp->sws_context,
              (const uint8_t *const *)fp->frame->data,
              fp->frame->linesize,
              0,
              fp->frame->height,
              (uint8_t *[4]){(uint8_t *)buf, NULL, NULL, NULL},
              (int[4]){width * 2, 0, 0, 0});
    *written = (size_t)(width * height * 2);
  } else {
    sws_scale(fp->sws_context,
              (const uint8_t *const *)fp->frame->data,
              fp->frame->linesize,
              0,
              fp->frame->height,
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
    int const f = fp->codec_context->pix_fmt;
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
  return sws_getContext(fp->codec_context->width,
                        fp->codec_context->height,
                        fp->codec_context->pix_fmt,
                        fp->codec_context->width,
                        fp->codec_context->height,
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
  if (v->packet) {
    av_packet_free(&v->packet);
  }
  if (v->frame) {
    av_frame_free(&v->frame);
  }
  if (v->sws_context) {
    sws_freeContext(v->sws_context);
  }
  if (v->codec_context) {
    avcodec_free_context(&v->codec_context);
  }
  if (v->format_context) {
    ffmpeg_destroy_format_context(&v->format_context);
  }
  ereport(mem_free(vpp));
}

NODISCARD error video_create(struct video **const vpp,
                             struct info_video *const vi,
                             struct video_options const *const opt) {
  if (!vpp || *vpp || !vi || !opt || !opt->filepath) {
    return errg(err_invalid_arugment);
  }
  struct video *fp = NULL;
  error err = mem(&fp, 1, sizeof(struct video));
  if (efailed(err)) {
    ereport(err);
    return NULL;
  }
  *fp = (struct video){0};
  err = ffmpeg_create_format_context(opt->filepath, 8126, &fp->format_context);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  int r = avformat_open_input(&fp->format_context, "", NULL, NULL);
  if (r < 0) {
    err = errffmpeg(r);
    goto cleanup;
  }
  r = avformat_find_stream_info(fp->format_context, NULL);
  if (r < 0) {
    err = errffmpeg(r);
    goto cleanup;
  }

  for (unsigned int i = 0; !fp->stream && i < fp->format_context->nb_streams; ++i) {
    if (fp->format_context->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      fp->stream = fp->format_context->streams[i];
    }
  }
  if (!fp->stream) {
    err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("stream not found")));
    goto cleanup;
  }
  AVCodec const *const codec = avcodec_find_decoder(fp->stream->codecpar->codec_id);
  if (!codec) {
    err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("video decoder not found")));
    goto cleanup;
  }
  err = ffmpeg_open_preferred_codec(
      opt->preferred_decoders, codec, fp->stream->codecpar, NULL, &fp->codec, &fp->codec_context);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }

  // workaround for h264_qsv
  if (strcmp(fp->codec->name, "h264_qsv") == 0 && fp->codec_context->pix_fmt == 0) {
    // It seems that the correct format is not set, so set it manually.
    fp->codec_context->pix_fmt = AV_PIX_FMT_NV12;
  }

  fp->frame = av_frame_alloc();
  if (!fp->frame) {
    err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("av_frame_alloc failed")));
    goto cleanup;
  }
  fp->packet = av_packet_alloc();
  if (!fp->packet) {
    err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("av_packet_alloc failed")));
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

cleanup:
  if (efailed(err)) {
    video_destroy(&fp);
  } else {
    *vpp = fp;
    get_info(fp, vi);
  }
  return err;
}
