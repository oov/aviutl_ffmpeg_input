#include "audio.h"

#ifndef NDEBUG
#  include <ovprintf.h>
#  include <ovutil/win32.h>
#endif

#include "ffmpeg.h"

typedef int16_t sample_t;
static int const g_channels = 2;
static int const g_sample_format = AV_SAMPLE_FMT_S16;
static int const g_sample_size = sizeof(sample_t) * g_channels;

struct audio {
  AVFormatContext *format_context;

  AVStream *stream;
  const AVCodec *codec;
  AVCodecContext *codec_context;
  AVFrame *frame;
  AVPacket *packet;

  SwrContext *swr_context;
  uint8_t *swr_buf;
  int swr_buf_len;
  int swr_buf_written;

  int current_samples;
  bool jumped;

  int64_t current_sample_pos;
  int64_t swr_buf_sample_pos;
};

static inline void get_info(struct audio const *const a, struct info_audio *const ai) {
  ai->sample_rate = a->codec_context->sample_rate;
  ai->channels = g_channels;
  ai->bit_depth = sizeof(sample_t) * 8;
  ai->samples = av_rescale_q(a->format_context->duration, AV_TIME_BASE_Q, av_make_q(1, a->codec_context->sample_rate));
#ifndef NDEBUG
  char s[256];
  ov_snprintf(s, 256, "a duration: %lld, samples: %lld", a->format_context->duration, ai->samples);
  OutputDebugStringA(s);
#endif
}

static inline void calc_current_frame(struct audio *fp) {
  // It seems pts value may be inaccurate.
  // There would be no way to correct the values except to recalculate from the first frame.
  // This program allows inaccurate values.
  // Instead, it avoids the accumulation of errors by not using the received pts as long as it continues to read frames.
  if (fp->jumped) {
    fp->current_sample_pos = av_rescale_q(
        fp->frame->pts - fp->stream->start_time, fp->stream->time_base, av_make_q(1, fp->codec_context->sample_rate));
    fp->jumped = false;
  } else {
    fp->current_sample_pos += fp->frame->nb_samples;
  }
  fp->current_samples = fp->frame->nb_samples;
#ifndef NDEBUG
  char s[256];
  ov_snprintf(s,
              256,
              "ts: %lld key_frame: %d, pts: %lld start_time: %lld time_base:%f sample_rate:%d",
              fp->current_sample_pos,
              fp->frame->key_frame,
              fp->frame->pts,
              fp->stream->start_time,
              av_q2d(fp->stream->time_base),
              fp->codec_context->sample_rate);
  OutputDebugStringA(s);
#endif
}

static NODISCARD error grab(struct audio *fp) {
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
    err = emsg(err_type_errno, AVUNERROR(r), &native_unmanaged_const(NSTR("avcodec_receive_frame failed")));
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
      err = emsg(err_type_errno, AVUNERROR(r), &native_unmanaged_const(NSTR("decoder has been flushed")));
      goto cleanup;
    default:
      err = emsg(err_type_errno, AVUNERROR(r), &native_unmanaged_const(NSTR("avcodec_receive_frame failed")));
      goto cleanup;
    }
  }
  if (fp->packet->stream_index != fp->stream->index) {
    goto read_frame;
  }
  r = avcodec_send_packet(fp->codec_context, fp->packet);
  switch (r) {
  case 0:
  case AVERROR(EAGAIN): // not ready to accept avcodec_send_packet, must call avcodec_receive_frame.
    goto receive_frame;
  default:
    err = emsg(err_type_errno, AVUNERROR(r), &native_unmanaged_const(NSTR("avcodec_send_packet failed")));
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

static NODISCARD error jump(struct audio *fp, int64_t sample) {
  error err = eok();
  int64_t time_stamp = av_rescale_q(sample, av_inv_q(AV_TIME_BASE_Q), av_make_q(fp->codec_context->sample_rate, 1));
#ifndef NDEBUG
  int64_t pts = av_rescale_q(time_stamp, AV_TIME_BASE_Q, fp->stream->time_base);
  char s[256];
  ov_snprintf(s,
              256,
              "req_ts:%lld pts:%lld sample: %lld tb: %f tb2: %f sr: %d",
              time_stamp,
              pts,
              sample,
              av_q2d(av_inv_q(AV_TIME_BASE_Q)),
              av_q2d(av_inv_q(fp->stream->time_base)),
              fp->codec_context->sample_rate);
  OutputDebugStringA(s);
#endif

  int r = avformat_seek_file(fp->format_context, -1, INT64_MIN, time_stamp, INT64_MAX, 0);
  if (r < 0) {
    err = emsg(err_type_errno, AVUNERROR(r), &native_unmanaged_const(NSTR("avformat_seek_file failed")));
    goto cleanup;
  }
  avcodec_flush_buffers(fp->codec_context);
  fp->jumped = true;
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

static NODISCARD error seek(struct audio *fp, int64_t sample) {
  int64_t f = sample;
  error err = jump(fp, f);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  while (fp->current_sample_pos > sample && f != 0) {
    f -= fp->codec_context->sample_rate;
    if (f < 0) {
      f = 0;
    }
#ifndef NDEBUG
    char s[256];
    ov_snprintf(s, 256, "re-jump: %lld", f);
    OutputDebugStringA(s);
#endif
    err = jump(fp, f);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
  }
  while (fp->current_sample_pos + fp->frame->nb_samples < sample) {
#ifndef NDEBUG
    char s[256];
    ov_snprintf(s, 256, "csp: %lld / smp: %lld", fp->current_sample_pos + fp->frame->nb_samples, sample);
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
    err = emsg(err_type_errno, AVUNERROR(r), &native_unmanaged_const(NSTR("swr_convert failed")));
    goto cleanup;
  }
  if (r) {
    fp->swr_buf_sample_pos += fp->swr_buf_written;
    fp->swr_buf_written = r;
    goto readbuf;
  }

  // seek:
  if (readpos < fp->current_sample_pos || readpos >= fp->current_sample_pos + fp->codec_context->sample_rate) {
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
  r = swr_convert(
      fp->swr_context, (void *)&fp->swr_buf, fp->swr_buf_len, (void *)fp->frame->data, fp->frame->nb_samples);
  if (r < 0) {
    err = emsg(err_type_errno, AVUNERROR(r), &native_unmanaged_const(NSTR("swr_convert failed")));
    goto cleanup;
  }
  if (r) {
    fp->swr_buf_sample_pos = fp->current_sample_pos;
    fp->swr_buf_written = r;
    goto readbuf;
  }

cleanup:
  if (efailed(err)) {
    if (eis_errno(err, AVUNERROR(AVERROR_EOF))) {
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
  if (a->packet) {
    av_packet_free(&a->packet);
  }
  if (a->frame) {
    av_frame_free(&a->frame);
  }
  if (a->swr_context) {
    swr_free(&a->swr_context);
  }
  if (a->swr_buf) {
    av_freep(&a->swr_buf);
  }
  if (a->codec_context) {
    avcodec_free_context(&a->codec_context);
  }
  if (a->format_context) {
    if (a->format_context->pb->opaque) {
      CloseHandle(a->format_context->pb->opaque);
    }
    avformat_close_input(&a->format_context);
  }
  ereport(mem_free(app));
}

NODISCARD error audio_create(struct audio **const app, struct info_audio *const ai, wchar_t const *const filepath) {
  if (!app || *app || !ai || !filepath) {
    return errg(err_invalid_arugment);
  }
  struct audio *fp = NULL;
  error err = mem(&fp, 1, sizeof(struct audio));
  if (efailed(err)) {
    ereport(err);
    return NULL;
  }
  *fp = (struct audio){0};

  err = ffmpeg_create_format_context(filepath, 8126, &fp->format_context);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  int r = avformat_open_input(&fp->format_context, "", NULL, NULL);
  if (r < 0) {
    err = emsg(err_type_errno, AVUNERROR(r), &native_unmanaged_const(NSTR("avformat_open_input failed")));
    goto cleanup;
  }
  r = avformat_find_stream_info(fp->format_context, NULL);
  if (r < 0) {
    err = emsg(err_type_errno, AVUNERROR(r), &native_unmanaged_const(NSTR("avformat_find_stream_info failed")));
    goto cleanup;
  }

  for (unsigned int i = 0; !fp->stream && i < fp->format_context->nb_streams; ++i) {
    if (fp->format_context->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      fp->stream = fp->format_context->streams[i];
    }
  }
  if (!fp->stream) {
    err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("stream not found")));
    goto cleanup;
  }
  fp->codec = avcodec_find_decoder(fp->stream->codecpar->codec_id);
  if (!fp->codec) {
    err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("video decoder not found")));
    goto cleanup;
  }

  // TODO: ユーザー側でデコーダーの優先度指定があるならそれに従って検索
  // char const *codec_str = fp->video_codec->name;
  // if (true /*decoder_redirect.count(codec_str) != 0*/) {
  //   AVCodec const *const dec = avcodec_find_decoder_by_name("h264_qsv");
  //   if (dec) {
  //     fp->codec = dec;
  //   }
  // }

  fp->codec_context = avcodec_alloc_context3(fp->codec);
  if (!fp->codec_context) {
    err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("avcodec_alloc_context3 failed")));
    goto cleanup;
  }
  r = avcodec_parameters_to_context(fp->codec_context, fp->stream->codecpar);
  if (r < 0) {
    err = emsg(err_type_errno, AVUNERROR(r), &native_unmanaged_const(NSTR("avcodec_parameters_to_context failed")));
    goto cleanup;
  }
  r = avcodec_open2(fp->codec_context, fp->codec, NULL);
  if (r < 0) {
    err = emsg(err_type_errno, AVUNERROR(r), &native_unmanaged_const(NSTR("avcodec_open2 failed")));
    goto cleanup;
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

  fp->jumped = true;
  err = grab(fp);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }

  fp->swr_buf_len = fp->codec_context->sample_rate * g_channels;
  r = av_samples_alloc(&fp->swr_buf, NULL, 2, fp->swr_buf_len, AV_SAMPLE_FMT_S16, 0);
  if (r < 0) {
    err = emsg(err_type_errno, AVUNERROR(r), &native_unmanaged_const(NSTR("av_samples_alloc failed")));
    goto cleanup;
  }

  r = swr_alloc_set_opts2(&fp->swr_context,
                          &(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO,
                          g_sample_format,
                          fp->codec_context->sample_rate,
                          &fp->frame->ch_layout,
                          fp->frame->format,
                          fp->frame->sample_rate,
                          0,
                          NULL);
  if (r < 0) {
    err = emsg(err_type_errno, AVUNERROR(r), &native_unmanaged_const(NSTR("swr_alloc_set_opts2 failed")));
    goto cleanup;
  }
  av_opt_set_int(fp->swr_context, "engine", SWR_ENGINE_SOXR, 0);
  r = swr_init(fp->swr_context);
  if (r < 0) {
    err = emsg(err_type_errno, AVUNERROR(r), &native_unmanaged_const(NSTR("swr_init failed")));
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
