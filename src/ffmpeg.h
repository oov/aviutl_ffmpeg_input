#pragma once

#include "ovbase.h"

#ifdef __GNUC__
#  ifndef __has_warning
#    define __has_warning(x) 0
#  endif
#  pragma GCC diagnostic push
#  if __has_warning("-Wsign-conversion")
#    pragma GCC diagnostic ignored "-Wsign-conversion"
#  endif
#  if __has_warning("-Wimplicit-int-conversion")
#    pragma GCC diagnostic ignored "-Wimplicit-int-conversion"
#  endif
#  if __has_warning("-Wdocumentation")
#    pragma GCC diagnostic ignored "-Wdocumentation"
#  endif
#endif // __GNUC__

#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavcodec/version.h>

#include <libavformat/avformat.h>
#include <libavformat/version.h>

#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/version.h>

#include <libswresample/swresample.h>
#include <libswresample/version.h>

#include <libswscale/swscale.h>
#include <libswscale/version.h>

#ifdef __GNUC__
#  pragma GCC diagnostic pop
#endif // __GNUC__

NODISCARD error ffmpeg_create_error(int const errnum ERR_FILEPOS_PARAMS);
#define errffmpeg(errnum) (ffmpeg_create_error((errnum)ERR_FILEPOS_VALUES))

struct ffmpeg_stream {
  AVFormatContext *fctx;
  AVStream *stream;
  AVCodec const *codec;
  AVCodecContext *cctx;
  AVFrame *frame;
  AVFrame *frame2;
  AVPacket *packet;
};

struct ffmpeg_open_options {
  wchar_t const *filepath;
  void *handle;
  size_t buffer_size;

  // these are used in ffmpeg_open.
  enum AVMediaType media_type;
  AVCodec const *codec;
  char const *preferred_decoders;
};

AVStream *ffmpeg_find_stream(struct ffmpeg_stream *const fs, enum AVMediaType media_type);

NODISCARD error ffmpeg_open_without_codec(struct ffmpeg_stream *const fs, struct ffmpeg_open_options const *const opt);
NODISCARD error ffmpeg_open(struct ffmpeg_stream *const fs, struct ffmpeg_open_options const *const opt);
void ffmpeg_close(struct ffmpeg_stream *const fs);

NODISCARD error ffmpeg_seek(struct ffmpeg_stream *const fs, int64_t const timestamp_in_stream_time_base);
NODISCARD error ffmpeg_seek_bytes(struct ffmpeg_stream *const fs, int64_t const pos);

int ffmpeg_read_packet(struct ffmpeg_stream *const fs);
int ffmpeg_grab(struct ffmpeg_stream *const fs);
