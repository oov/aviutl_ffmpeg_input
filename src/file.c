#include "file.h"

NODISCARD error read(HANDLE h, void *const buf, size_t sz) {
  char *b = buf;
  for (DWORD read = 0; sz > 0; b += (size_t)read, sz -= (size_t)read) {
    if (!ReadFile(h, b, (DWORD)sz, &read, NULL)) {
      return errhr(HRESULT_FROM_WIN32(GetLastError()));
    }
  }
  return eok();
}

NODISCARD error write(HANDLE h, void const *const buf, size_t sz) {
  char const *b = buf;
  for (DWORD written = 0; sz > 0; b += (size_t)written, sz -= (size_t)written) {
    if (!WriteFile(h, b, (DWORD)sz, &written, NULL)) {
      return errhr(HRESULT_FROM_WIN32(GetLastError()));
    }
  }
  return eok();
}

NODISCARD error flush(HANDLE h) {
  if (!FlushFileBuffers(h)) {
    return errhr(HRESULT_FROM_WIN32(GetLastError()));
  }
  return eok();
}
