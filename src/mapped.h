#pragma once

#include "ovbase.h"

struct mapped;

NODISCARD error mapped_create(struct mapped **const mpp, wchar_t const *const filepath);
void mapped_destroy(struct mapped **const mpp);
int mapped_read(struct mapped *const mp, void *const buf, int const buf_size);
int64_t mapped_seek(struct mapped *const mp, int64_t const offset, int const whence);
int64_t mapped_get_size(struct mapped *const mp);
