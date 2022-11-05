#include "error.h"

#include "ovutil/win32.h"

#include "version.h"

NODISCARD static error build_error_message(error e, wchar_t const *const main_message, struct wstr *const dest) {
  struct wstr tmp = {0};
  struct wstr msg = {0};
  error err = eok();
  if (e == NULL) {
    err = scpy(dest, main_message);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
    goto cleanup;
  }
  err = error_to_string(e, &tmp);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  err = scpym(&msg, main_message, L"\r\n\r\n", tmp.ptr);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  err = scpy(dest, msg.ptr);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }

cleanup:
  ereport(sfree(&msg));
  ereport(sfree(&tmp));
  return err;
}

static HWND find_aviutl_window(void) {
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
    return h;
  }
}

void error_message_box(error e, wchar_t const *const msg) {
  struct wstr errmsg = {0};
  error err = build_error_message(e, msg, &errmsg);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  message_box(find_aviutl_window(), errmsg.ptr, L"ffmpeg Video Reader " VERSION_WIDE, MB_ICONERROR);

cleanup:
  ereport(sfree(&errmsg));
  if (efailed(err)) {
    ereportmsg(err, &native_unmanaged(NSTR("エラーダイアログの表示に失敗しました。")));
  }
  efree(&e);
}
