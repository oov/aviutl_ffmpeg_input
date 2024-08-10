#pragma once

#include "ffmpeg.h"
#include "ovbase.h"

// AviUtl only supports 16-bit.
// The number of channels is fixed to 2 because it is rarely used with more than 1 or 2 channels.
typedef int16_t sample_t;
static int const resampler_out_sample_format = AV_SAMPLE_FMT_S16;
static int const resampler_out_channels = 2;
static int const resampler_out_sample_size = sizeof(sample_t) * resampler_out_channels;

static inline struct gcd {
  int gcd;      // GCD
  int factor_a; // a / GCD
  int factor_b; // b / GCD
} gcd(int const a, int const b) {
  int x = a;
  int y = b;
  while (y != 0) {
    int const z = x % y;
    x = y;
    y = z;
  }
  return (struct gcd){x, a / x, b / x};
}

struct resampler {
  SwrContext *ctx;
  uint8_t *buf;
  int64_t pos; // sample position in output sample rate
  int samples; // size of buf in samples
  int written; // number of samples written to buf
  struct gcd gcd;
};

struct resampler_options {
  int out_rate;
  AVCodecParameters const *codecpar;
  bool use_sox;
};

NODISCARD error resampler_create(struct resampler **const rp, struct resampler_options const *const opt);
void resampler_destroy(struct resampler **const rp);
NODISCARD error resampler_resample(
    struct resampler *const r, void const *const in, int const in_samples, void *const out, int *const out_samples);
