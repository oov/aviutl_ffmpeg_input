#include "ovbase.h"

#include "ovnum.h"
#include "ovutil/win32.h"

#include "api.h"
#include "bridgecommon.h"
#include "ipcserver.h"

static NODISCARD error create_fmo(DWORD const bytes, HANDLE *const fmo, wchar_t *const fmo_name16) {
  if (!bytes || !fmo || !fmo_name16) {
    return errg(err_invalid_arugment);
  }
  wchar_t fmo_name[16];
  HANDLE h = NULL;
  size_t retry = 0;
  while (h == NULL) {
    wsprintfW(fmo_name, L"ipcfmo_%08x", get_global_hint() & 0xffffffff);
    h = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, bytes, fmo_name);
    HRESULT const hr = HRESULT_FROM_WIN32(GetLastError());
    if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
      CloseHandle(h);
      h = NULL;
      if (++retry < 5) {
        continue;
      }
    }
    if (FAILED(hr)) {
      return errhr(hr);
    }
  }
  *fmo = h;
  wcscpy(fmo_name16, fmo_name);
  return eok();
}

struct handle {
  size_t frame_size;
  size_t sample_size;
  INPUT_HANDLE ih;
};

static INPUT_PLUGIN_TABLE *g_ipt = NULL;
static struct ipcserver *g_serv = NULL;
static wchar_t g_fmo_name[16] = {0};
static HANDLE g_fmo = NULL;
static DWORD g_fmo_bytes = 0;

static inline LONG longabs(LONG const v) { return (v < 0) ? -v : v; }

static void ipc_handler_open(struct ipcserver_context *const ctx) {
  struct str filepath = {0};
  struct handle *h = NULL;
  INPUT_HANDLE ih = NULL;
  INPUT_INFO ii = {0};
  error err = eok();
  if (ctx->buffer_size < sizeof(struct bridge_event_open_request)) {
    err = emsg(
        err_type_generic, err_invalid_arugment, &native_unmanaged_const(NSTR("open request packet size too small")));
    goto cleanup;
  }
  struct bridge_event_open_request *const req = ctx->buffer;
  if ((size_t)ctx->buffer_size < sizeof(struct bridge_event_open_request) + (size_t)req->filepath_size) {
    err = errg(err_invalid_arugment);
    goto cleanup;
  }
  char *str = ctx->buffer;
  err = sncpy(&filepath, str + sizeof(struct bridge_event_open_request), (size_t)req->filepath_size);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  ih = g_ipt->func_open(filepath.ptr);
  if (ih) {
    if (g_ipt->func_info_get(ih, &ii)) {
      err = mem(&h, 1, sizeof(struct handle));
      if (efailed(err)) {
        err = ethru(err);
        goto cleanup;
      }
      *h = (struct handle){
          .ih = ih,
          .frame_size = (size_t)(ii.format->biWidth * ii.format->biBitCount / 8 * longabs(ii.format->biHeight)),
          .sample_size = (size_t)(ii.audio_format->nChannels * ii.audio_format->wBitsPerSample / 8),
      };
    }
  }
  err = ctx->grow_buffer(ctx, sizeof(struct bridge_event_open_response));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  struct bridge_event_open_response *const resp = ctx->buffer;
  *resp = (struct bridge_event_open_response){
      .id = (uint64_t)h,
      .frame_size = (uint32_t)h->frame_size,
      .sample_size = (uint32_t)h->sample_size,
  };
cleanup:
  if (efailed(err)) {
    if (ih) {
      g_ipt->func_close(ih);
    }
  }
  ereport(sfree(&filepath));
  ctx->finish(ctx, err);
}

static void ipc_handler_close(struct ipcserver_context *const ctx) {
  error err = eok();
  if (ctx->buffer_size != sizeof(struct bridge_event_close_request)) {
    err = emsg(err_type_generic,
               err_invalid_arugment,
               &native_unmanaged_const(NSTR("close request packet size is incorrect")));
    goto cleanup;
  }
  struct bridge_event_close_request const *const req = ctx->buffer;
  struct handle *h = (void *)req->id;
  BOOL const r = g_ipt->func_close(h->ih);
  if (r) {
    err = mem_free(&h);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
  }
  err = ctx->grow_buffer(ctx, sizeof(struct bridge_event_close_response));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  struct bridge_event_close_response *resp = ctx->buffer;
  *resp = (struct bridge_event_close_response){
      .success = r != FALSE ? 1 : 0,
  };
cleanup:
  ctx->finish(ctx, err);
}

static void ipc_handler_get_info(struct ipcserver_context *const ctx) {
  error err = eok();
  if (ctx->buffer_size != sizeof(struct bridge_event_get_info_request)) {
    err = emsg(err_type_generic,
               err_invalid_arugment,
               &native_unmanaged_const(NSTR("get_info request packet size is incorrect")));
    goto cleanup;
  }
  struct bridge_event_get_info_request const *const req = ctx->buffer;
  struct handle *h = (void *)req->id;
  INPUT_INFO ii = {0};
  BOOL const r = g_ipt->func_info_get(h->ih, &ii);
  err = ctx->grow_buffer(
      ctx, (uint32_t)(sizeof(struct bridge_event_get_info_response) + (size_t)(ii.format_size + ii.audio_format_size)));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  struct bridge_event_get_info_response *resp = ctx->buffer;
  *resp = (struct bridge_event_get_info_response){
      .success = r != FALSE ? 1 : 0,
      .flag = (int32_t)ii.flag,
      .rate = (int32_t)ii.rate,
      .scale = (int32_t)ii.scale,
      .video_frames = (int32_t)ii.n,
      .video_format_size = (int32_t)ii.format_size,
      .audio_samples = (int32_t)ii.audio_n,
      .audio_format_size = (int32_t)ii.audio_format_size,
      .handler = (uint32_t)ii.handler,
  };
  char *b = ctx->buffer;
  memcpy(b + sizeof(struct bridge_event_get_info_response), ii.format, (size_t)ii.format_size);
  memcpy(b + sizeof(struct bridge_event_get_info_response) + (size_t)ii.format_size,
         ii.audio_format,
         (size_t)ii.audio_format_size);
cleanup:
  ctx->finish(ctx, err);
}

static void ipc_handler_read(struct ipcserver_context *const ctx) {
  error err = eok();
  void *mapped = NULL;
  if (ctx->buffer_size != sizeof(struct bridge_event_read_request)) {
    err = emsg(
        err_type_generic, err_invalid_arugment, &native_unmanaged_const(NSTR("read request packet size is incorrect")));
    goto cleanup;
  }
  struct bridge_event_read_request const *const req = ctx->buffer;
  struct handle *h = (void *)req->id;
  size_t const bytes = req->length == 0 ? h->frame_size : (size_t)(req->length) * h->sample_size;
  wchar_t fmo_name[16] = {0};
  if (g_fmo_bytes < bytes) {
    HANDLE fmo = NULL;
    err = create_fmo((DWORD)bytes, &fmo, fmo_name);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
    if (g_fmo) {
      CloseHandle(g_fmo);
    }
    g_fmo = fmo;
    g_fmo_bytes = (DWORD)bytes;
    wcscpy(g_fmo_name, fmo_name);
  }
  mapped = MapViewOfFile(g_fmo, FILE_MAP_WRITE, 0, 0, bytes);
  if (!mapped) {
    err = errhr(HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  int const written = req->length == 0 ? g_ipt->func_read_video(h->ih, req->start, mapped)
                                       : g_ipt->func_read_audio(h->ih, req->start, req->length, mapped);
  err = ctx->grow_buffer(ctx, sizeof(struct bridge_event_read_response));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  struct bridge_event_read_response *resp = ctx->buffer;
  *resp = (struct bridge_event_read_response){
      .written = written,
  };
  wcscpy(resp->fmo_name, g_fmo_name);
cleanup:
  if (mapped) {
    if (!UnmapViewOfFile(mapped)) {
      ereport(emsg(err_type_hresult,
                   HRESULT_FROM_WIN32(GetLastError()),
                   &native_unmanaged_const(NSTR("warn: UnmapViewOfFile failed"))));
    }
    mapped = NULL;
  }
  ctx->finish(ctx, err);
}

static void ipc_handler_config(struct ipcserver_context *const ctx) {
  error err = eok();
  if (ctx->buffer_size != sizeof(struct bridge_event_config_request)) {
    err = emsg(err_type_generic,
               err_invalid_arugment,
               &native_unmanaged_const(NSTR("config request packet size is incorrect")));
    goto cleanup;
  }
  struct bridge_event_config_request const *const req = ctx->buffer;
  BOOL const r = g_ipt->func_config((HWND)req->window, get_hinstance());
  err = ctx->grow_buffer(ctx, (uint32_t)(sizeof(struct bridge_event_config_response)));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  struct bridge_event_config_response *resp = ctx->buffer;
  *resp = (struct bridge_event_config_response){
      .success = r != FALSE ? 1 : 0,
  };
cleanup:
  ctx->finish(ctx, err);
}

static void ipc_handler(struct ipcserver_context *const ctx) {
  switch ((enum bridge_event_id)ctx->event_id) {
  case bridge_event_open:
    ipc_handler_open(ctx);
    return;
  case bridge_event_close:
    ipc_handler_close(ctx);
    return;
  case bridge_event_get_info:
    ipc_handler_get_info(ctx);
    return;
  case bridge_event_read:
    ipc_handler_read(ctx);
    return;
  case bridge_event_config:
    ipc_handler_config(ctx);
    return;
  }
  ctx->finish(ctx, errg(err_invalid_arugment));
}

void __declspec(dllexport) CALLBACK BridgeMainW(HWND window, HINSTANCE hinstance, LPWSTR cmdline, int cmdshow);
void __declspec(dllexport) CALLBACK BridgeMainW(HWND window, HINSTANCE hinstance, LPWSTR cmdline, int cmdshow) {
  (void)window;
  (void)hinstance;
  (void)cmdshow;
  bool initialized = false;
  HANDLE event = NULL;
  HANDLE parent_process = NULL;
  error err = eok();

  wchar_t event_name[16] = {0};
  DWORD parent_pid = 0;
  {
    wchar_t *sep = wcschr(cmdline, L' ');
    if (!sep) {
      err = errg(err_unexpected);
      goto cleanup;
    }
    size_t len = (size_t)(sep - cmdline);
    if (len > 16) {
      err = errg(err_unexpected);
      goto cleanup;
    }
    wcsncpy(event_name, cmdline, len);
    event_name[len] = L'\0';

    len = wcslen(sep + 1);
    if (len > 32) {
      err = errg(err_unexpected);
      goto cleanup;
    }
    uint64_t n = 0;
    if (!ov_atou(sep + 1, &n, true)) {
      err = errg(err_unexpected);
      goto cleanup;
    }
    parent_pid = (DWORD)n;
  }
  event = OpenEventW(SYNCHRONIZE, FALSE, event_name);
  if (!event) {
    err = errhr(HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  parent_process = OpenProcess(SYNCHRONIZE, FALSE, parent_pid);
  if (!parent_process) {
    err = errhr(HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }

  g_ipt = get_input_plugin_table();
  if (g_ipt->func_init) {
    if (!g_ipt->func_init()) {
      err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("func_init failed")));
      goto cleanup;
    }
    initialized = true;
  }

  {
    wchar_t pipe_name[64] = {0};
    build_pipe_name(pipe_name, event_name);
    err = ipcserver_create(&g_serv,
                           &(struct ipcserver_options){
                               .pipe_name = pipe_name,
                               .signature = BRIDGE_IPC_SIGNATURE,
                               .protocol_version = BRIDGE_IPC_VERSION,
                               .userdata = NULL,
                               .handler = ipc_handler,
                               .error_handler = NULL,
                           });
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
  }

  {
    MSG msg = {0};
    HANDLE handles[2] = {event, parent_process};
    for (;;) {
      DWORD r = MsgWaitForMultipleObjects(2, handles, FALSE, INFINITE, QS_ALLINPUT);
      switch (r) {
      case WAIT_OBJECT_0:
        goto cleanup;
      case WAIT_OBJECT_0 + 1:
        goto cleanup;
      case WAIT_OBJECT_0 + 2:
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
          TranslateMessage(&msg);
          DispatchMessageW(&msg);
        }
        break;
      case WAIT_FAILED:
        err = errhr(HRESULT_FROM_WIN32(GetLastError()));
        goto cleanup;
      default:
        goto cleanup;
      }
    }
  }
cleanup:
  if (g_serv) {
    ereport(ipcserver_destroy(&g_serv));
  }
  if (g_fmo) {
    CloseHandle(g_fmo);
    g_fmo = NULL;
  }
  if (initialized) {
    if (g_ipt->func_exit) {
      if (!g_ipt->func_exit()) {
        ereport(emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("func_exit failed"))));
      }
    }
  }
  if (parent_process) {
    CloseHandle(parent_process);
  }
  if (event) {
    CloseHandle(event);
  }
  ereport(err);
}
