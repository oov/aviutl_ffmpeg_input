#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <mmsystem.h>

#ifdef __GNUC__
#  ifndef __has_warning
#    define __has_warning(x) 0
#  endif
#  pragma GCC diagnostic push
#  if __has_warning("-Winvalid-utf8")
#    pragma GCC diagnostic ignored "-Winvalid-utf8"
#  endif
#endif // __GNUC__

#include "3rd/aviutl_sdk/input.h"

#ifdef __GNUC__
#  pragma GCC diagnostic pop
#endif // __GNUC__

static inline HWND find_aviutl_window(void) {
  DWORD const pid = GetCurrentProcessId();
  HWND h = NULL;
  for (;;) {
    h = FindWindowExA(NULL, h, "AviUtl", NULL);
    if (h == 0) {
      return 0;
    }
    DWORD p = 0;
    GetWindowThreadProcessId(h, &p);
    if (p != pid) {
      continue;
    }
    if (!IsWindowVisible(h)) {
      continue;
    }
    if (!(GetWindowLongW(h, GWL_STYLE) & WS_MINIMIZEBOX)) {
      continue;
    }
    return h;
  }
}
