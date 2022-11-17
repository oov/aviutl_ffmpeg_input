#pragma once

#include "ovbase.h"

#include "ovutil/win32.h"

NODISCARD error ipccommon_read(HANDLE h, void *const buf, size_t sz);
NODISCARD error ipccommon_write(HANDLE h, void const *const buf, size_t sz);
NODISCARD error ipccommon_flush(HANDLE h);
