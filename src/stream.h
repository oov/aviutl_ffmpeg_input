#pragma once

#include "ovbase.h"

#include "info.h"

struct stream;

NODISCARD error stream_create(struct stream **const spp, wchar_t const *const filepath);
void stream_destroy(struct stream **const spp);
struct info_video const *stream_get_video_info(struct stream const *const sp);
struct info_audio const *stream_get_audio_info(struct stream const *const sp);
NODISCARD error stream_read_video(struct stream *const sp, int64_t const frame, void *const buf, size_t *const written);
NODISCARD error stream_read_audio(
    struct stream *const sp, int64_t const start, size_t const length, void *const buf, int *const written);
