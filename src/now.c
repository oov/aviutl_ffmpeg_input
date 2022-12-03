#include "now.h"

#include "ovutil/win32.h"

static double get_freq(void) {
  static double freq = 0.;
  if (freq > 0.) {
    return freq;
  }
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  freq = (double)(f.QuadPart);
  return freq;
}

double now(void) {
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return (double)(c.QuadPart) / get_freq();
}
