#pragma once

#include "info.h"
#include "ovbase.h"

struct audio;

NODISCARD error audio_create(struct audio **const app, struct info_audio *const ai, wchar_t const *const filepath);
void audio_destroy(struct audio **const app);
NODISCARD error
audio_read(struct audio *const fp, int64_t const offset, int const length, void *const buf, int *const written);
