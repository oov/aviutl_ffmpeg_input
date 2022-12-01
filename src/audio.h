#pragma once

#include "info.h"
#include "ovbase.h"

struct audio;

enum audio_index_mode {
  aim_noindex = 0,
  aim_relax = 1,
  aim_strict = 2,
};

struct audio_options {
  wchar_t const *filepath;
  void *handle;
  char const *preferred_decoders;
  int64_t video_start_time;
  enum audio_index_mode index_mode;
};

NODISCARD error audio_create(struct audio **const app, struct audio_options const *const opt);
void audio_destroy(struct audio **const app);
NODISCARD error audio_read(struct audio *const fp,
                           int64_t const offset,
                           int const length,
                           void *const buf,
                           int *const written,
                           bool const accurate);
int64_t audio_get_start_time(struct audio const *const a);
void audio_get_info(struct audio const *const a, struct info_audio *const ai);
