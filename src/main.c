#include "ovbase.h"
#include "ovutil/str.h"
#include "ovutil/win32.h"

#include "api.h"
#include "bridgeclient.h"
#include "error.h"

INPUT_PLUGIN_TABLE __declspec(dllexport) * __stdcall GetInputPluginTable(void);
INPUT_PLUGIN_TABLE __declspec(dllexport) * __stdcall GetInputPluginTable(void) {
  struct wstr module = {0};
  INPUT_PLUGIN_TABLE *table = NULL;
  error err = get_module_file_name(get_hinstance(), &module);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  size_t ext = 0;
  err = extract_file_extension(&module, &ext);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  if (ext > 7 &&
      (wcsnicmp(L"-brdg32", module.ptr + ext - 7, 7) == 0 || wcsnicmp(L"-brdg64", module.ptr + ext - 7, 7) == 0)) {
    table = get_input_plugin_bridge_table();
  } else {
    table = get_input_plugin_table();
  }
cleanup:
  if (efailed(err)) {
    error_message_box(err, L"プラグインの初期化に失敗しました。");
  }
  ereport(sfree(&module));
  return table;
}

struct own_api const __declspec(dllexport) * __stdcall GetOwnAPIEndPoint(void);
struct own_api const __declspec(dllexport) * __stdcall GetOwnAPIEndPoint(void) {
  struct wstr module = {0};
  struct own_api const *own_api = NULL;
  error err = get_module_file_name(get_hinstance(), &module);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  size_t ext = 0;
  err = extract_file_extension(&module, &ext);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  if (ext > 7 &&
      (wcsnicmp(L"-brdg32", module.ptr + ext - 7, 7) == 0 || wcsnicmp(L"-brdg64", module.ptr + ext - 7, 7) == 0)) {
    own_api = get_own_api_bridge_endpoint();
  } else {
    own_api = get_own_api_endpoint();
  }
cleanup:
  if (efailed(err)) {
    error_message_box(err, L"プラグインの初期化に失敗しました。");
  }
  ereport(sfree(&module));
  return own_api;
}

static void
error_reporter(error const e, struct NATIVE_STR const *const message, struct ov_filepos const *const filepos) {
  struct NATIVE_STR tmp = {0};
  struct NATIVE_STR msg = {0};
  error err = error_to_string(e, &tmp);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  NATIVE_CHAR buf[1024] = {0};
  wsprintfW(buf, NSTR("\r\n(reported at %hs:%ld %hs())\r\n"), filepos->file, filepos->line, filepos->func);
  err = scpym(&msg, message->ptr, buf, tmp.ptr);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  OutputDebugStringW(msg.ptr);

cleanup:
  if (efailed(err)) {
    OutputDebugStringW(NSTR("failed to report error"));
    efree(&err);
  }
  eignore(sfree(&msg));
  eignore(sfree(&tmp));
}

static BOOL main_init(HINSTANCE const inst) {
  ov_init();
  error_set_reporter(error_reporter);
  // TODO: ereportmsg(error_ptk_init(), &native_unmanaged(NSTR("エラーメッセージマッパーの登録に失敗しました。")));
  set_hinstance(inst);

  return TRUE;
}

static BOOL main_exit(void) {
  ov_exit();
  return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE const inst, DWORD const reason, LPVOID const reserved);
BOOL WINAPI DllMain(HINSTANCE const inst, DWORD const reason, LPVOID const reserved) {
  (void)reserved;
  (void)inst;
  switch (reason) {
  case DLL_PROCESS_ATTACH:
    return main_init(inst);
  case DLL_PROCESS_DETACH:
    return main_exit();
  }
  return TRUE;
}
