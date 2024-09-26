#include "progress.h"

#include <ovbase.h>

#include <ovarray.h>
#include <ovthreads.h>
#include <ovutil/win32.h>

struct progress_context {
  void const *user_context;
  size_t progress;
};

struct progress {
  struct progress_context *ctx;
  thrd_t thread;
  HWND exedit;
  HWND hwnd;
  bool initialized;
};

static mtx_t g_mtx = {0};
static struct progress g_progress = {0};

static wchar_t const prop_name[] = L"progress_instance";

static int const width = 128;
static int const height_per_item = 2;

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
  case WM_CLOSE: {
    return 0;
  }
  case WM_DESTROY: {
    RemovePropW(hwnd, prop_name);
    PostQuitMessage(0);
    return 0;
  }
  case WM_USER: {
    struct progress *const pg = GetPropW(hwnd, prop_name);
    RECT r;
    int h = (int)(OV_ARRAY_LENGTH(pg->ctx))*height_per_item;
    GetWindowRect(g_progress.exedit, &r);
    SetWindowPos(hwnd, HWND_TOPMOST, r.right - width - 32, r.bottom - h - 32, width, h, SWP_NOREDRAW);
    InvalidateRect(hwnd, NULL, TRUE);
    return 0;
  }
  case WM_USER + 1: {
    DestroyWindow(hwnd);
    return 0;
  }
  case WM_PAINT: {
    struct progress *const pg = GetPropW(hwnd, prop_name);
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    mtx_lock(&g_mtx);
    HGDIOBJ brush = GetStockObject(LTGRAY_BRUSH);
    for (size_t i = 0; i < OV_ARRAY_LENGTH(pg->ctx); ++i) {
      struct progress_context *const ctx = &pg->ctx[i];
      FillRect(hdc,
               &(RECT){
                   .left = 0,
                   .top = (int)(i)*height_per_item,
                   .right = (int)(ctx->progress) * width / 10000,
                   .bottom = (int)(i + 1) * height_per_item,
               },
               brush);
    }
    mtx_unlock(&g_mtx);
    EndPaint(hwnd, &ps);
    return 0;
  }
  default:
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
}

static int worker_thread(void *const userdata) {
  struct progress *const pg = userdata;

  static wchar_t const class_name[] = L"ffmpeg_input_audio_indexer_progress";
  HINSTANCE hinst = get_hinstance();

  ATOM atom = 0;
  HWND hwnd = NULL;

  atom = RegisterClassExW(&(WNDCLASSEXW){
      .cbSize = sizeof(WNDCLASSEXW),
      .lpfnWndProc = window_proc,
      .hInstance = hinst,
      .lpszClassName = class_name,
      .hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH),
  });
  if (!atom) {
    goto cleanup;
  }

  hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOPMOST,
                         class_name,
                         class_name,
                         WS_VISIBLE | WS_POPUP,
                         0,
                         0,
                         256,
                         48,
                         NULL,
                         NULL,
                         hinst,
                         pg);
  if (!hwnd) {
    goto cleanup;
  }

  pg->hwnd = hwnd;
  SetPropW(hwnd, prop_name, pg);
  SetLayeredWindowAttributes(hwnd, 0, 168, LWA_ALPHA);
  ShowWindow(hwnd, SW_SHOW);

  for (;;) {
    MSG msg;
    BOOL r = GetMessageW(&msg, hwnd, 0, 0);
    if (r == -1) {
      break;
    }
    if (msg.message == WM_NULL) {
      break;
    }
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

cleanup:
  if (atom) {
    UnregisterClassW(class_name, hinst);
  }
  return 0;
}

static NODISCARD error init(struct progress *const pg) {
  if (pg->initialized) {
    return errg(err_invalid_arugment);
  }
  error err = eok();
  bool thread_created = false;

  if (thrd_create(&pg->thread, worker_thread, pg) != thrd_success) {
    err = emsg_i18n(err_type_generic, err_fail, "thrd_create failed");
    goto cleanup;
  }
  thread_created = true;
  pg->initialized = true;

cleanup:
  if (efailed(err)) {
    if (thread_created) {
      thrd_detach(pg->thread);
    }
  }
  return err;
}

static void destroy(struct progress *const pg) {
  if (pg->initialized) {
    PostMessageW(pg->hwnd, WM_USER + 1, 0, 0);
    thrd_join(pg->thread, NULL);
    pg->hwnd = NULL;
    OV_ARRAY_DESTROY(&pg->ctx);
    pg->initialized = false;
  }
}

void progress_set(void const *const user_context, size_t const progress) {
  error err = eok();
  struct progress_context *ctx = NULL;
  bool need_destroy = false;
  mtx_lock(&g_mtx);

  if (!g_progress.initialized && progress < 10000) {
    err = init(&g_progress);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
  }

  for (size_t i = 0; i < OV_ARRAY_LENGTH(g_progress.ctx); ++i) {
    if (g_progress.ctx[i].user_context == user_context) {
      ctx = &g_progress.ctx[i];
      break;
    }
  }
  if (!ctx) {
    err = OV_ARRAY_PUSH(&g_progress.ctx,
                        ((struct progress_context){
                            .user_context = user_context,
                            .progress = progress,
                        }));
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
    ctx = &g_progress.ctx[OV_ARRAY_LENGTH(g_progress.ctx) - 1];
  }
  if (progress < 10000) {
    ctx->progress = progress;
  } else {
    size_t const ln = OV_ARRAY_LENGTH(g_progress.ctx);
    if (ln > 1) {
      memmove(ctx, ctx + 1, sizeof(struct progress_context) * (ln - (size_t)(ctx - g_progress.ctx) - 1));
    }
    OV_ARRAY_SET_LENGTH(g_progress.ctx, ln - 1);
    need_destroy = ln == 1;
  }
  PostMessageW(g_progress.hwnd, WM_USER, 0, 0);
cleanup:
  mtx_unlock(&g_mtx);
  if (need_destroy) {
    destroy(&g_progress);
  }
}

void progress_set_exedit_window(size_t const hwnd) {
  mtx_lock(&g_mtx);
  g_progress.exedit = (HWND)hwnd;
  mtx_unlock(&g_mtx);
}

void progress_init(void) { mtx_init(&g_mtx, mtx_plain); }

void progress_destroy(void) {
  mtx_lock(&g_mtx);
  bool need_destroy = g_progress.initialized;
  mtx_unlock(&g_mtx);
  if (need_destroy) {
    destroy(&g_progress);
  }
  mtx_destroy(&g_mtx);
}
