#pragma once

#include "info.h"
#include "ovbase.h"

struct audio;

struct audio_options {
  wchar_t const *filepath;
  char const *preferred_decoders;
  int64_t video_start_time;
  bool use_audio_index;
};

NODISCARD error audio_create(struct audio **const app,
                             struct info_audio *const ai,
                             struct audio_options const *const opt);
void audio_destroy(struct audio **const app);
NODISCARD error
audio_read(struct audio *const fp, int64_t const offset, int const length, void *const buf, int *const written);
int64_t audio_get_start_time(struct audio *const a);
