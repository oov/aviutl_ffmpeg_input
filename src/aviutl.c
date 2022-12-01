#include "aviutl.h"

#include "ovutil/win32.h"

#include <memory.h>
#include <psapi.h>

enum aviutl_version {
  av_initial,
  av_unknown,
  av_100,
  av_110,
};

static HMODULE g_aviutl = NULL;
static enum aviutl_version g_aviutl_version = av_initial;

static enum aviutl_version detect_aviutl_version(HMODULE *const mod) {
  HMODULE m = GetModuleHandleW(NULL);
  if (!m) {
    *mod = NULL;
    return av_unknown;
  }
  MODULEINFO mi = {0};
  if (!GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof(MODULEINFO))) {
    *mod = NULL;
    return av_unknown;
  }
  if (mi.SizeOfImage < 0x24000) {
    *mod = NULL;
    return av_unknown;
  }
  uint8_t const *p = (void *)m;
  if (memcmp(&(int32_t){11003}, p + 0x221f7, sizeof(int32_t)) == 0) {
    *mod = m;
    return av_110;
  } else if (memcmp(&(int32_t){10000}, p + 0x1ae02, sizeof(int32_t)) == 0) {
    *mod = m;
    return av_100;
  }
  *mod = NULL;
  return av_unknown;
}

bool aviutl_is_saving(void) {
  if (g_aviutl_version == av_initial) {
    g_aviutl_version = detect_aviutl_version(&g_aviutl);
  }
  uint8_t const *p = (void *)g_aviutl;
  switch (g_aviutl_version) {
  case av_initial:
  case av_unknown:
    return true;
  case av_110:
    return !(memcmp(&(uint32_t){0}, p + 0x87954, sizeof(uint32_t)) == 0) ||
           !(memcmp(&(uint32_t){0}, p + 0x24bac4, sizeof(uint32_t)) == 0);
  case av_100:
    return !(memcmp(&(uint32_t){0}, p + 0x71684, sizeof(uint32_t)) == 0) ||
           !(memcmp(&(uint32_t){0}, p + 0x23b1a0, sizeof(uint32_t)) == 0);
  }
}
