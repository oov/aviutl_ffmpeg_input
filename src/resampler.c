#include "resampler.h"

NODISCARD error resampler_create(struct resampler **const rp, struct resampler_options const *const opt) {
  if (!rp || *rp || !opt || !opt->codecpar || opt->out_rate <= 0) {
    return errg(err_invalid_arugment);
  }
  struct resampler *resampler = NULL;
  error err = mem(&resampler, 1, sizeof(struct resampler));
  if (efailed(err)) {
    ereport(err);
    return NULL;
  }
  *resampler = (struct resampler){
      .gcd = gcd(opt->codecpar->sample_rate, opt->out_rate),
      .pos = AV_NOPTS_VALUE,
      .samples = opt->out_rate * resampler_out_channels,
  };
  int r = av_samples_alloc(&resampler->buf, NULL, 2, resampler->samples, AV_SAMPLE_FMT_S16, 0);
  if (r < 0) {
    err = errffmpeg(r);
    goto cleanup;
  }
  r = swr_alloc_set_opts2(&resampler->ctx,
                          &(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO,
                          resampler_out_sample_format,
                          opt->out_rate,
#if LIBSWRESAMPLE_VERSION_INT < AV_VERSION_INT(5, 0, 0)
                          ov_deconster_(&opt->codecpar->ch_layout),
#else
                          &opt->codecpar->ch_layout,
#endif
                          opt->codecpar->format,
                          opt->codecpar->sample_rate,
                          0,
                          NULL);
  if (r < 0) {
    err = errffmpeg(r);
    goto cleanup;
  }
  if (opt->use_sox) {
    av_opt_set_int(resampler->ctx, "engine", SWR_ENGINE_SOXR, 0);
  }
  r = swr_init(resampler->ctx);
  if (r < 0) {
    err = errffmpeg(r);
    goto cleanup;
  }
  *rp = resampler;
cleanup:
  if (efailed(err)) {
    resampler_destroy(&resampler);
  }
  return err;
}

void resampler_destroy(struct resampler **const rp) {
  if (!rp || !*rp) {
    return;
  }
  struct resampler *const r = *rp;
  if (r->ctx) {
    swr_free(&r->ctx);
  }
  if (r->buf) {
    av_freep(&r->buf);
  }
  ereport(mem_free(rp));
}

NODISCARD error resampler_resample(
    struct resampler *const r, void const *const in, int const in_samples, void *const out, int *const out_samples);
