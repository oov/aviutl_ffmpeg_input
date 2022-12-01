#pragma once

#include "ovbase.h"

#include "info.h"

struct streammap;

NODISCARD error streammap_create(struct streammap **const smpp);
void streammap_destroy(struct streammap **const smpp);

NODISCARD error streammap_create_stream(struct streammap *const smp,
                                        wchar_t const *const filepath,
                                        intptr_t *const idx);
NODISCARD error streammap_free_stream(struct streammap *const smp, intptr_t const idx);

struct info_video const *streammap_get_video_info(struct streammap *const smp, intptr_t const idx);
struct info_audio const *streammap_get_audio_info(struct streammap *const smp, intptr_t const idx);

NODISCARD error streammap_read_video(
    struct streammap *const smp, intptr_t const idx, int64_t const frame, void *const buf, size_t *const written);
NODISCARD error streammap_read_audio(struct streammap *const smp,
                                     intptr_t const idx,
                                     int64_t const start,
                                     size_t const length,
                                     void *const buf,
                                     int *const written,
                                     bool const accurate);
