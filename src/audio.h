#pragma once

#include "info.h"
#include "ovbase.h"

struct audio;

enum audio_index_mode {
  aim_noindex = 0,
  aim_relax = 1,
  aim_strict = 2,
};

enum audio_sample_rate {
  asr_original = 0,
  asr_8000 = 8000,
  asr_11025 = 11025,
  asr_12000 = 12000,
  asr_16000 = 16000,
  asr_22050 = 22050,
  asr_24000 = 24000,
  asr_32000 = 32000,
  asr_44100 = 44100,
  asr_48000 = 48000,
  asr_64000 = 64000,
  asr_88200 = 88200,
  asr_96000 = 96000,
  asr_128000 = 128000,
  asr_176400 = 176400,
  asr_192000 = 192000,
  asr_256000 = 256000,
};

struct audio_options {
  wchar_t const *filepath;
  void *handle;
  char const *preferred_decoders;
  size_t num_stream;
  enum audio_index_mode index_mode;
  enum audio_sample_rate sample_rate;
  bool use_sox;
};

NODISCARD error audio_create(struct audio **const app, struct audio_options const *const opt);
void audio_destroy(struct audio **const app);
NODISCARD error audio_read(struct audio *const fp,
                           int64_t const offset,
                           int const length,
                           void *const buf,
                           int *const written,
                           bool const accurate);
void audio_get_info(struct audio const *const a, struct info_audio *const ai);
