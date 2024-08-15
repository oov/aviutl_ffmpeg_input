#include "ffmpeg.h"

#define USE_FILE_MAPPING 0

#if USE_FILE_MAPPING
#  include "mapped.h"
#endif

#include "ovutil/win32.h"

NODISCARD error ffmpeg_create_error(int errnum ERR_FILEPOS_PARAMS) {
  char buf[AV_ERROR_MAX_STRING_SIZE];
  if (av_strerror(errnum, buf, AV_ERROR_MAX_STRING_SIZE) < 0) {
    return eok();
  }
  struct NATIVE_STR s = {0};
  error err = from_mbcs(&str_unmanaged_const(buf), &s);
  if (efailed(err)) {
    err = ethru(err);
    goto failed;
  }
  return error_add_(NULL, err_type_errno, errnum, &s ERR_FILEPOS_VALUES_PASSTHRU);

failed:
  ereport(sfree(&s));
  ereport(err);
  return err(err_type_errno, errnum);
}

#if USE_FILE_MAPPING
struct mappedfile {
  struct mapped *mp;
};

static int w32read(void *opaque, uint8_t *buf, int buf_size) {
  struct mappedfile *const file = opaque;
  int const r = mapped_read(file->mp, buf, buf_size);
  if (r == 0) {
    return AVERROR_EOF;
  }
  return r;
}

static int64_t w32seek(void *opaque, int64_t offset, int whence) {
  struct mappedfile *const file = opaque;
  return whence == AVSEEK_SIZE ? mapped_get_size(file->mp) : mapped_seek(file->mp, offset, whence);
}
#else
struct w32file {
  HANDLE h;
  bool close_handle;
};

static int w32read(void *opaque, uint8_t *buf, int buf_size) {
  struct w32file *file = opaque;
  DWORD read;
  if (!ReadFile(file->h, (void *)buf, (DWORD)buf_size, &read, NULL)) {
    return -EIO;
  }
  if (read == 0) {
    return AVERROR_EOF;
  }
  return (int)read;
}

static int64_t w32seek(void *opaque, int64_t offset, int whence) {
  struct w32file *file = opaque;
  DWORD w;
  switch (whence) {
  case SEEK_SET:
    w = FILE_BEGIN;
    break;
  case SEEK_CUR:
    w = FILE_CURRENT;
    break;
  case SEEK_END:
    w = FILE_END;
    break;
  case AVSEEK_SIZE: {
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(file->h, &sz)) {
      return -EIO;
    }
    return sz.QuadPart;
  }
  default:
    return -EINVAL;
  }
  LARGE_INTEGER pos;
  if (!SetFilePointerEx(file->h, (LARGE_INTEGER){.QuadPart = offset}, &pos, w)) {
    return -EIO;
  }
  return pos.QuadPart;
}
#endif

struct create_format_context_options {
  wchar_t const *filepath;
  void *handle;
  size_t buffer_size;
};

static NODISCARD error create_format_context(AVFormatContext **const format_context,
                                             struct create_format_context_options *const opt) {
  if (!format_context || *format_context || !opt ||
      (!opt->filepath && (opt->handle == NULL || opt->handle == INVALID_HANDLE_VALUE))) {
    return errg(err_invalid_arugment);
  }
  error err = eok();
  AVFormatContext *ctx = NULL;
  unsigned char *buffer = NULL;

#if USE_FILE_MAPPING
  struct mappedfile *file = NULL;
  struct mapped *mp = NULL;
  err = mapped_create(&mp,
                      &(struct mapped_options){
                          .filepath = opt->filepath,
                          .handle = opt->handle,
                      });
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
#else
  struct w32file *file = NULL;
  HANDLE h = INVALID_HANDLE_VALUE;
  if (opt->filepath) {
    h = CreateFileW(opt->filepath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
      err = errhr(HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }
  } else {
    wchar_t fn[4096];
    GetFinalPathNameByHandleW(opt->handle, fn, 4095, 0);
    h = CreateFileW(fn, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
      err = errhr(HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }
  }
#endif

  ctx = avformat_alloc_context();
  if (!ctx) {
    err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("avformat_alloc_context failed")));
    goto cleanup;
  }

#if USE_FILE_MAPPING
  file = av_malloc(sizeof(struct mappedfile));
  if (!file) {
    err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("av_malloc failed")));
    goto cleanup;
  }
  *file = (struct mappedfile){
      .mp = mp,
  };
  buffer = av_malloc(opt->buffer_size);
  if (!buffer) {
    err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("av_malloc failed")));
    goto cleanup;
  }
  ctx->pb = avio_alloc_context(buffer, (int)opt->buffer_size, 0, file, w32read, NULL, w32seek);
  if (!ctx->pb) {
    err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("avio_alloc_context failed")));
    goto cleanup;
  }
  ctx->pb->direct = 1;
#else
  file = av_malloc(sizeof(struct w32file));
  if (!file) {
    err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("av_malloc failed")));
    goto cleanup;
  }
  *file = (struct w32file){0};
  if (h != INVALID_HANDLE_VALUE) {
    file->h = h;
    file->close_handle = true;
  } else {
    file->h = opt->handle;
  }

  buffer = av_malloc(opt->buffer_size);
  if (!buffer) {
    err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("av_malloc failed")));
    goto cleanup;
  }
  ctx->pb = avio_alloc_context(buffer, (int)opt->buffer_size, 0, file, w32read, NULL, w32seek);
  if (!ctx->pb) {
    err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("avio_alloc_context failed")));
    goto cleanup;
  }
#endif
  *format_context = ctx;
cleanup:
  if (efailed(err)) {
    if (buffer) {
      av_free(buffer);
    }
    if (file) {
      av_free(file);
    }
    if (ctx) {
      if (ctx->pb) {
        avio_context_free(&ctx->pb);
      }
      avformat_free_context(ctx);
    }
#if USE_FILE_MAPPING
    if (mp) {
      mapped_destroy(&mp);
    }
#else
    if (h != INVALID_HANDLE_VALUE) {
      CloseHandle(h);
    }
#endif
  }
  return err;
}

static void destroy_format_context(AVFormatContext **const format_context) {
  if ((*format_context)->pb->opaque) {
#if USE_FILE_MAPPING
    struct mappedfile *file = (*format_context)->pb->opaque;
    mapped_destroy(&file->mp);
#else
    struct w32file *file = (*format_context)->pb->opaque;
    if (file->close_handle) {
      CloseHandle(file->h);
    }
#endif
    av_freep(&(*format_context)->pb->opaque);
  }
  avformat_close_input(format_context);
}

static inline bool is_space(int ch) { return ch == '\t' || ch == '\n' || ch == '\r' || ch == ' '; }

static AVCodec const *find_preferred(AVCodec const *(*finder)(char const *name),
                                     char const *const decoders,
                                     AVCodec const *const codec,
                                     size_t *const pos) {
  char buf[32];
  while (decoders[*pos] != '\0') {
    char const *token = decoders + *pos;
    if (*token == ',') {
      ++*pos;
      continue;
    }
    char const *sep = strchr(token, ',');
    size_t token_len = sep ? (size_t)(sep - token) : strlen(token);
    *pos += token_len + (sep ? 1 : 0);
    while (token_len > 0 && is_space(*token)) {
      ++token;
      --token_len;
    }
    while (token_len > 0 && is_space(token[token_len - 1])) {
      --token_len;
    }
    if (token_len >= 32) {
      continue;
    }
    strncpy(buf, token, token_len);
    buf[token_len] = '\0';
    AVCodec const *const c = finder(buf);
    if (!c || c->id != codec->id) {
      continue;
    }
    return c;
  }
  return NULL;
}

static NODISCARD error open_codec(AVCodec const *const codec,
                                  AVCodecParameters const *const codec_params,
                                  AVDictionary **const options,
                                  struct ffmpeg_stream *const fs,
                                  bool const try_grab) {
  error err = eok();
  AVCodecContext *ctx = avcodec_alloc_context3(codec);
  if (!ctx) {
    err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("avcodec_alloc_context3 failed")));
    goto cleanup;
  }
  int r = avcodec_parameters_to_context(ctx, codec_params);
  if (r < 0) {
    err = errffmpeg(r);
    goto cleanup;
  }
  ctx->pkt_timebase = fs->stream->time_base;
  r = avcodec_open2(ctx, codec, options);
  if (r < 0) {
    err = errffmpeg(r);
    goto cleanup;
  }
  fs->codec = codec;
  fs->cctx = ctx;
  if (try_grab) {
    r = ffmpeg_grab(fs);
    if (r < 0) {
      err = errffmpeg(r);
      goto cleanup;
    }
  }
cleanup:
  if (efailed(err)) {
    if (ctx) {
      avcodec_free_context(&ctx);
    }
    fs->cctx = NULL;
    fs->codec = NULL;
  }
  return err;
}

static NODISCARD error open_preferred_codec(char const *const decoders,
                                            AVCodec const *const codec,
                                            AVCodecParameters const *const codec_params,
                                            AVDictionary **const options,
                                            struct ffmpeg_stream *const fs,
                                            bool const try_grab) {
  if (!codec || !codec_params || !fs) {
    return errg(err_invalid_arugment);
  }
  error err = eok();
  if (decoders) {
    size_t pos = 0;
    AVCodec const *preferred = NULL;
    while ((preferred = find_preferred(avcodec_find_decoder_by_name, decoders, codec, &pos)) != NULL) {
      err = open_codec(preferred, codec_params, options, fs, try_grab);
      if (efailed(err)) {
        err = ethru(err);
        wchar_t buf[1024];
        wsprintfW(buf, L"failed open codec with \"%hs\".", preferred->name);
        ereportmsg(err, &native_unmanaged_const(buf));
        // If the codec is not loaded properly, the next codec may not be loaded properly, so reset stream position.
        // This was encountered when reading Sintel_1080_10s_20MB.mp4.
        // https://test-videos.co.uk/sintel/mp4-av1
        int const r = avformat_seek_file(fs->fctx, fs->stream->index, INT64_MIN, INT64_MIN, INT64_MIN, 0);
        if (r < 0) {
          return errffmpeg(r);
        }
        continue;
      }
      goto cleanup;
    }
  }
  err = open_codec(codec, codec_params, options, fs, try_grab);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
cleanup:
  return err;
}

void ffmpeg_close(struct ffmpeg_stream *const fs) {
  if (fs->packet) {
    av_packet_free(&fs->packet);
  }
  if (fs->frame) {
    av_frame_free(&fs->frame);
  }
  if (fs->cctx) {
    avcodec_free_context(&fs->cctx);
  }
  if (fs->codec) {
    fs->codec = NULL;
  }
  if (fs->stream) {
    fs->stream = NULL;
  }
  if (fs->fctx) {
    destroy_format_context(&fs->fctx);
  }
}

NODISCARD error ffmpeg_open_without_codec(struct ffmpeg_stream *const fs, struct ffmpeg_open_options const *const opt) {
  if (!opt || (!opt->filepath && (opt->handle == NULL || opt->handle == INVALID_HANDLE_VALUE))) {
    return errg(err_invalid_arugment);
  }
  AVFormatContext *fctx = NULL;
  AVFrame *frame = NULL;
  AVPacket *packet = NULL;
  error err = create_format_context(&fctx,
                                    &(struct create_format_context_options){
                                        .filepath = opt->filepath,
                                        .handle = opt->handle,
                                        .buffer_size = opt->buffer_size,
                                    });
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  int r = avformat_open_input(&fctx, "", NULL, NULL);
  if (r < 0) {
    err = errffmpeg(r);
    goto cleanup;
  }
  r = avformat_find_stream_info(fctx, NULL);
  if (r < 0) {
    err = errffmpeg(r);
    goto cleanup;
  }
  frame = av_frame_alloc();
  if (!frame) {
    err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("av_frame_alloc failed")));
    goto cleanup;
  }
  packet = av_packet_alloc();
  if (!packet) {
    err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("av_packet_alloc failed")));
    goto cleanup;
  }
  *fs = (struct ffmpeg_stream){
      .fctx = fctx,
      .frame = frame,
      .packet = packet,
  };
cleanup:
  if (efailed(err)) {
    if (packet) {
      av_packet_free(&packet);
    }
    if (frame) {
      av_frame_free(&frame);
    }
    if (fctx) {
      destroy_format_context(&fctx);
    }
  }
  return err;
}

NODISCARD error ffmpeg_open(struct ffmpeg_stream *const fs, struct ffmpeg_open_options const *const opt) {
  if (!opt || (!opt->filepath && (opt->handle == NULL || opt->handle == INVALID_HANDLE_VALUE))) {
    return errg(err_invalid_arugment);
  }
  AVDictionary *options = NULL;

  error err = ffmpeg_open_without_codec(fs, opt);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  int const stream_index = av_find_best_stream(fs->fctx, opt->media_type, -1, -1, NULL, 0);
  if (stream_index < 0) {
    err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("stream not found")));
    goto cleanup;
  }
  fs->stream = fs->fctx->streams[stream_index];
  if (opt->codec != NULL) {
    err = open_codec(opt->codec, fs->stream->codecpar, &options, fs, opt->try_grab);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
  } else {
    AVCodec const *const orig_codec = avcodec_find_decoder(fs->stream->codecpar->codec_id);
    if (!orig_codec) {
      err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("decoder not found")));
      goto cleanup;
    }
    err = open_preferred_codec(opt->preferred_decoders, orig_codec, fs->stream->codecpar, &options, fs, opt->try_grab);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
  }
  // workaround for h264_qsv
  if (strcmp(fs->codec->name, "h264_qsv") == 0 && fs->cctx->pix_fmt == 0) {
    // It seems that the correct format is not set, so set it manually.
    fs->cctx->pix_fmt = AV_PIX_FMT_NV12;
  }
cleanup:
  if (options) {
    av_dict_free(&options);
  }
  if (efailed(err)) {
    ffmpeg_close(fs);
  }
  return err;
}

NODISCARD error ffmpeg_seek(struct ffmpeg_stream *const fs, int64_t const timestamp_in_stream_time_base) {
  int r = avformat_seek_file(
      fs->fctx, fs->stream->index, INT64_MIN, timestamp_in_stream_time_base, timestamp_in_stream_time_base, 0);
  if (r < 0) {
    return errffmpeg(r);
  }
  avcodec_flush_buffers(fs->cctx);
  return eok();
}

static int inline receive_frame(struct ffmpeg_stream *const fs) { return avcodec_receive_frame(fs->cctx, fs->frame); }

int ffmpeg_read_packet(struct ffmpeg_stream *const fs) {
  for (;;) {
    av_packet_unref(fs->packet);
    int const r = av_read_frame(fs->fctx, fs->packet);
    if (r < 0) {
      return r;
    }
    if (fs->packet->stream_index != fs->stream->index) {
      continue;
    }
    return r;
  }
}

static int inline send_packet(struct ffmpeg_stream *const fs) { return avcodec_send_packet(fs->cctx, fs->packet); }

static int inline send_null_packet(struct ffmpeg_stream *const fs) { return avcodec_send_packet(fs->cctx, NULL); }

static int grab(struct ffmpeg_stream *const fs, bool const discard) {
  int r;
receive:
  r = receive_frame(fs);
  switch (r) {
  case 0:
    return r;
  case AVERROR_EOF:
  case AVERROR(EAGAIN):
  case AVERROR_INPUT_CHANGED:
    break;
  default:
    return r;
  }
  r = ffmpeg_read_packet(fs);
  if (r < 0) {
    // flush
    r = send_null_packet(fs);
    switch (r) {
    case 0:
    case AVERROR(EAGAIN):
      goto receive;
    case AVERROR_EOF: // decoder has been flushed
      return r;
    default:
      return r;
    }
  }
  if (discard) {
    fs->packet->flags |= AV_PKT_FLAG_DISCARD;
  }
  r = send_packet(fs);
  switch (r) {
  case 0:
    goto receive;
  case AVERROR(EAGAIN):
    // not ready to accept avcodec_send_packet, must call avcodec_receive_frame.
    goto receive;
  default:
    return r;
  }
}

int ffmpeg_grab(struct ffmpeg_stream *const fs) { return grab(fs, false); }

int ffmpeg_grab_discard(struct ffmpeg_stream *const fs) { return grab(fs, true); }
