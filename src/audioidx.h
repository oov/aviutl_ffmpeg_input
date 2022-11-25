#pragma once

#include "ovbase.h"

struct audioidx;

NODISCARD error audioidx_create(struct audioidx **const ipp, wchar_t const *const filepath);
void audioidx_destroy(struct audioidx **const ipp);
int64_t audioidx_get(struct audioidx *const ip, int64_t const pts);
