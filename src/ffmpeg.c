#include "ffmpeg.h"

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

struct w32file {
  HANDLE h;
  AVIOContext *ctx;
};

static int w32read(void *opaque, uint8_t *buf, int buf_size) {
  struct w32file *file = opaque;
  DWORD read;
  if (!ReadFile(file->h, (void *)buf, (DWORD)buf_size, &read, NULL)) {
    errno = EIO;
    return 0;
  }
  if (read == 0) {
    file->ctx->eof_reached = 1;
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
  default:
    return -1; // failed
  }
  LARGE_INTEGER pos;
  if (!SetFilePointerEx(file->h, (LARGE_INTEGER){.QuadPart = offset}, &pos, w)) {
    errno = EIO;
    return -1;
  }
  return pos.QuadPart;
}

static NODISCARD error create_format_context(wchar_t const *const filename,
                                                    size_t const buffer_size,
                                                    AVFormatContext **const format_context) {
  error err = eok();
  HANDLE h = INVALID_HANDLE_VALUE;
  AVFormatContext *ctx = NULL;
  struct w32file *file = NULL;
  unsigned char *buffer = NULL;
  h = CreateFileW(filename, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) {
    err = errhr(HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  ctx = avformat_alloc_context();
  if (!ctx) {
    err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("avformat_alloc_context failed")));
    goto cleanup;
  }
  file = av_malloc(sizeof(struct w32file));
  if (!file) {
    err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("av_malloc failed")));
    goto cleanup;
  }
  *file = (struct w32file){
      .h = h,
  };
  buffer = av_malloc(buffer_size);
  if (!buffer) {
    err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("av_malloc failed")));
    goto cleanup;
  }
  ctx->pb = avio_alloc_context(buffer, (int)buffer_size, 0, file, w32read, NULL, w32seek);
  if (!ctx->pb) {
    err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("avio_alloc_context failed")));
    goto cleanup;
  }
  file->ctx = ctx->pb;
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
    if (h != INVALID_HANDLE_VALUE) {
      CloseHandle(h);
    }
  }
  return err;
}

static void destroy_format_context(AVFormatContext **const format_context) {
  if ((*format_context)->pb->opaque) {
    struct w32file *file = (*format_context)->pb->opaque;
    CloseHandle(file->h);
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
                                  AVCodecContext **const codec_context) {
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
  r = avcodec_open2(ctx, codec, options);
  if (r < 0) {
    err = errffmpeg(r);
    goto cleanup;
  }
  *codec_context = ctx;
cleanup:
  if (efailed(err)) {
    if (ctx) {
      avcodec_free_context(&ctx);
    }
  }
  return err;
}

static NODISCARD error open_preferred_codec(char const *const decoders,
                                                   AVCodec const *const codec,
                                                   AVCodecParameters const *const codec_params,
                                                   AVDictionary **const options,
                                                   AVCodec const **codec_selected,
                                                   AVCodecContext **const codec_context) {
  if (!codec || !codec_params || !codec_context || *codec_context) {
    return errg(err_invalid_arugment);
  }
  error err = eok();
  if (decoders) {
    size_t pos = 0;
    AVCodec const *preferred = NULL;
    while ((preferred = find_preferred(avcodec_find_decoder_by_name, decoders, codec, &pos)) != NULL) {
      err = open_codec(preferred, codec_params, options, codec_context);
      if (efailed(err)) {
        err = ethru(err);
        wchar_t buf[1024];
        wsprintfW(buf, L"failed open codec with \"%hs\".", preferred->name);
        ereportmsg(err, &native_unmanaged_const(buf));
        continue;
      }
      if (codec_selected) {
        *codec_selected = preferred;
      }
      goto cleanup;
    }
  }
  err = open_codec(codec, codec_params, options, codec_context);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  if (codec_selected) {
    *codec_selected = codec;
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

NODISCARD error ffmpeg_open(struct ffmpeg_stream *const fs,
                            wchar_t const *const filepath,
                            enum AVMediaType const media_type,
                            char const *const preferred_decoders) {
  if (!filepath) {
    return errg(err_invalid_arugment);
  }
  AVFormatContext *fctx = NULL;
  AVStream *stream = NULL;
  AVCodec const *codec = NULL;
  AVCodecContext *cctx = NULL;
  AVFrame *frame = NULL;
  AVPacket *packet = NULL;
  error err = create_format_context(filepath, 8126, &fctx);
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

  for (unsigned int i = 0; !stream && i < fctx->nb_streams; ++i) {
    if (fctx->streams[i]->codecpar->codec_type == media_type) {
      stream = fctx->streams[i];
      break;
    }
  }
  if (!stream) {
    err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("stream not found")));
    goto cleanup;
  }
  AVCodec const *const orig_codec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (!orig_codec) {
    err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("decoder not found")));
    goto cleanup;
  }
  err = open_preferred_codec(preferred_decoders, orig_codec, stream->codecpar, NULL, &codec, &cctx);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }

  // workaround for h264_qsv
  if (media_type == AVMEDIA_TYPE_VIDEO && strcmp(codec->name, "h264_qsv") == 0 && cctx->pix_fmt == 0) {
    // It seems that the correct format is not set, so set it manually.
    cctx->pix_fmt = AV_PIX_FMT_NV12;
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
      .stream = stream,
      .codec = codec,
      .cctx = cctx,
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
    if (cctx) {
      avcodec_free_context(&cctx);
    }
    if (codec) {
      codec = NULL;
    }
    if (stream) {
      stream = NULL;
    }
    if (fctx) {
      destroy_format_context(&fctx);
    }
  }
  return err;
}
