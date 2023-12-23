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

HWND aviutl_get_exedit_window(void) {
  if (g_aviutl_version == av_initial) {
    g_aviutl_version = detect_aviutl_version(&g_aviutl);
  }

  typedef BOOL (*get_sys_info_func)(void *editp, SYS_INFO *sip);
  typedef void *(*get_filterp_func)(int filter_id);

  get_sys_info_func get_sys_info = NULL;
  get_filterp_func get_filterp = NULL;

  uint8_t *p = (void *)g_aviutl;
  switch (g_aviutl_version) {
  case av_initial:
  case av_unknown:
    goto fallback;
  case av_110:
    get_sys_info = (get_sys_info_func)(void *)(p + 0x22120);
    get_filterp = (get_filterp_func)(void *)(p + 0x31e40);
    break;
  case av_100:
    get_sys_info = (get_sys_info_func)(void *)(p + 0x1ad20);
    get_filterp = (get_filterp_func)(void *)(p + 0x277b0);
    break;
  }
  SYS_INFO si = {0};
  if (!get_sys_info(NULL, &si)) {
    goto fallback;
  }
  static char const exedit_name_mbcs[] = "\x8a\x67\x92\xa3\x95\xd2\x8f\x57";              // "拡張編集"
  static char const zhcn_patched_exedit_name_mbcs[] = "\xc0\xa9\xd5\xb9\xb1\xe0\xbc\xad"; // "扩展编辑"
  static char const en_patched_exedit_name_mbcs[] = "Advanced Editing";
  FILTER *exedit = NULL;
  for (int i = 0; i < si.filter_n; ++i) {
    FILTER *f = get_filterp(i);
    if (!f || (f->flag & FILTER_FLAG_AUDIO_FILTER) == FILTER_FLAG_AUDIO_FILTER) {
      continue;
    }
    if (strcmp(f->name, exedit_name_mbcs) == 0 || strcmp(f->name, zhcn_patched_exedit_name_mbcs) == 0 ||
        strcmp(f->name, en_patched_exedit_name_mbcs) == 0) {
      exedit = f;
      break;
    }
  }
  if (!exedit || !exedit->hwnd) {
    goto fallback;
  }
  return exedit->hwnd;

fallback:
  return find_aviutl_window();
}
