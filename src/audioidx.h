#pragma once

#include "ovbase.h"

struct audioidx;

struct audioidx_create_options {
  wchar_t const *const filepath;
  void *handle;
  int64_t const video_start_time;
};

NODISCARD error audioidx_create(struct audioidx **const ipp, struct audioidx_create_options const *const opt);
void audioidx_destroy(struct audioidx **const ipp);
int64_t audioidx_get(struct audioidx *const ip, int64_t const pts);
