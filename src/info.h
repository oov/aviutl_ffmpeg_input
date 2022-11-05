#pragma once

#include <stdint.h>

#ifndef PACKED
#  if __has_c_attribute(PACKED)
#    define PACKED [[packed]]
#  elif __has_attribute(packed)
#    define PACKED __attribute__((packed))
#  else
#    define PACKED
#  endif
#endif // #endif

#ifdef __GNUC__
#  pragma GCC diagnostic push
#  if __has_warning("-Wpacked")
#    pragma GCC diagnostic ignored "-Wpacked"
#  endif
#endif // __GNUC__

struct info_video {
  int64_t frames;
  int32_t width;
  int32_t height;
  int32_t bit_depth;
  int32_t is_rgb;
  int32_t frame_rate;
  int32_t frame_scale;
};

struct info_audio {
  int64_t samples;
  int32_t sample_rate;
  int16_t channels;
  int16_t bit_depth;
};

#ifdef __GNUC__
#  pragma GCC diagnostic pop
#endif // __GNUC__
