#pragma once

#include <stdint.h>

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
