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
#include <libavformat/avformat.h>

#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>

#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#ifdef __GNUC__
#  pragma GCC diagnostic pop
#endif // __GNUC__

NODISCARD error ffmpeg_create_error(int const errnum ERR_FILEPOS_PARAMS);
#define errffmpeg(errnum) (ffmpeg_create_error((errnum)ERR_FILEPOS_VALUES))

NODISCARD error ffmpeg_create_format_context(wchar_t const *const filename,
                                             size_t const buffer_size,
                                             AVFormatContext **const format_context);
void ffmpeg_destroy_format_context(AVFormatContext **const format_context);

NODISCARD error ffmpeg_open_preferred_codec(char const *const decoders,
                                            AVCodec const *const codec,
                                            AVCodecParameters const *const codec_params,
                                            AVDictionary **const options,
                                            AVCodec const **codec_selected,
                                            AVCodecContext **const codec_context);
