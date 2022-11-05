#pragma once

#include "info.h"
#include "ovbase.h"

struct video;

NODISCARD error video_create(struct video **const vpp, struct info_video *const vi, char const *const filepath);
void video_destroy(struct video **const vpp);
NODISCARD error video_read(struct video *const v, int64_t frame, void *buf, size_t *written);
