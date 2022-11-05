#pragma once

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
