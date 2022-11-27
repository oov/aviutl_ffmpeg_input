#pragma once

#include "info.h"
#include "ovbase.h"

struct video;

enum video_format_scaling_algorithm {
  video_format_scaling_algorithm_fast_bilinear = 0x1,
  video_format_scaling_algorithm_bilinear = 0x2,
  video_format_scaling_algorithm_bicubic = 0x4,
  video_format_scaling_algorithm_x = 0x8,
  video_format_scaling_algorithm_point = 0x10,
  video_format_scaling_algorithm_area = 0x20,
  video_format_scaling_algorithm_bicublin = 0x40,
  video_format_scaling_algorithm_gauss = 0x80,
  video_format_scaling_algorithm_sinc = 0x100,
  video_format_scaling_algorithm_lanczos = 0x200,
  video_format_scaling_algorithm_spline = 0x400,
};

struct video_options {
  wchar_t const *filepath;
  void *handle;
  char const *preferred_decoders;
  enum video_format_scaling_algorithm scaling;
};

NODISCARD error video_create(struct video **const vpp, struct video_options const *const opt);
void video_destroy(struct video **const vpp);
NODISCARD error video_read(struct video *const v, int64_t frame, void *buf, size_t *written);
int64_t video_get_start_time(struct video const *const v);
void video_get_info(struct video const *const v, struct info_video *const vi);
