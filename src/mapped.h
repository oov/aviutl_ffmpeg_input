#pragma once

#include "ovbase.h"

struct mapped;

struct mapped_options {
  wchar_t const *filepath;
  void *handle;
};

NODISCARD error mapped_create(struct mapped **const mpp, struct mapped_options const *const opt);
void mapped_destroy(struct mapped **const mpp);
int mapped_read(struct mapped *const mp, void *const buf, int const buf_size);
int64_t mapped_seek(struct mapped *const mp, int64_t const offset, int const whence);
int64_t mapped_get_size(struct mapped *const mp);
