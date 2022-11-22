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
  AVPacket *packet;
};

NODISCARD error ffmpeg_open(struct ffmpeg_stream *const fs,
                            wchar_t const *const filepath,
                            enum AVMediaType const media_type,
                            char const *const preferred_decoders);
void ffmpeg_close(struct ffmpeg_stream *const fs);

NODISCARD error ffmpeg_seek(struct ffmpeg_stream *const fs, int64_t const timestamp_in_stream_time_base);

NODISCARD error ffmpeg_grab(struct ffmpeg_stream *const fs);
