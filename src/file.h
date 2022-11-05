#pragma once

#include "ovbase.h"

#include "ovutil/win32.h"

NODISCARD error read(HANDLE h, void *const buf, size_t sz);
NODISCARD error write(HANDLE h, void const *const buf, size_t sz);
NODISCARD error flush(HANDLE h);
