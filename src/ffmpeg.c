#include "ffmpeg.h"

#include "ovutil/win32.h"

static int read(void *opaque, uint8_t *buf, int buf_size) {
  DWORD read;
  if (!ReadFile((HANDLE)opaque, (void *)buf, (DWORD)buf_size, &read, NULL)) {
    errno = EIO;
    return 0;
  }
  return (int)read;
}

static int64_t seek(void *opaque, int64_t offset, int whence) {
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
  if (!SetFilePointerEx((HANDLE)opaque, (LARGE_INTEGER){.QuadPart = offset}, &pos, w)) {
    errno = EIO;
    return -1;
  }
  return pos.QuadPart;
}

NODISCARD error ffmpeg_create_format_context(wchar_t const *const filename,
                                             size_t const buffer_size,
                                             AVFormatContext **const format_context) {
  error err = eok();
  HANDLE h = INVALID_HANDLE_VALUE;
  AVFormatContext *ctx = NULL;
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
  buffer = av_malloc(buffer_size);
  if (!buffer) {
    err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("av_malloc failed")));
    goto cleanup;
  }
  ctx->pb = avio_alloc_context(buffer, (int)buffer_size, 0, h, read, NULL, seek);
  if (!ctx->pb) {
    err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("avio_alloc_context failed")));
    goto cleanup;
  }
  *format_context = ctx;
cleanup:
  if (efailed(err)) {
    if (buffer) {
      av_free(buffer);
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

void ffmpeg_destroy_format_context(AVFormatContext **const format_context) {
  if ((*format_context)->pb->opaque) {
    CloseHandle((*format_context)->pb->opaque);
  }
  avformat_close_input(format_context);
}

NODISCARD error ffmpeg_open_codec(AVCodec const *const codec,
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
    err = emsg(err_type_errno, AVUNERROR(r), &native_unmanaged_const(NSTR("avcodec_parameters_to_context failed")));
    goto cleanup;
  }
  r = avcodec_open2(ctx, codec, options);
  if (r < 0) {
    err = emsg(err_type_errno, AVUNERROR(r), &native_unmanaged_const(NSTR("avcodec_open2 failed")));
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
