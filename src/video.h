#pragma once

#include "info.h"
#include "ovbase.h"

struct video;

struct video_options {
  wchar_t const *filepath;
  char const *prefered_decoders;
};

NODISCARD error video_create(struct video **const vpp,
                             struct info_video *const vi,
                             struct video_options const *const opt);
void video_destroy(struct video **const vpp);
NODISCARD error video_read(struct video *const v, int64_t frame, void *buf, size_t *written);
