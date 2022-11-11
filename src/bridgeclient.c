#include "bridgeclient.h"

#include "ovbase.h"
#include "ovthreads.h"
#include "ovutil/str.h"
#include "ovutil/win32.h"

#include "bridgecommon.h"
#include "error.h"
#include "ipcclient.h"
#include "process.h"
#include "version.h"

#include <stdatomic.h>

static struct process *g_process = NULL;
static struct ipcclient *g_ipcc = NULL;

static BITMAPINFOHEADER *g_bih = NULL;
static size_t g_bih_size = 0;
static WAVEFORMATEX *g_wfex = NULL;
static size_t g_wfex_size = 0;

static HANDLE g_fmo = NULL;
static wchar_t g_fmo_name[16] = {0};

struct handle {
  uint64_t id;
  struct str filepath;
  size_t frame_size;
  size_t sample_size;
};

struct handle_map_item {
  struct handle *key;
};

static mtx_t g_handles_mtx = {0};
static struct hmap g_handles = {0};

enum runnning_state {
  rs_unknown = 0,
  rs_booting = 1,
  rs_running = 2,
  rs_exiting = 3,
};
static atomic_int g_running_state = rs_unknown;

struct config_thread_context {
  HWND window;
  HANDLE event;
  BOOL ret;
  error err;
};

static int config_thread(void *arg) {
  struct config_thread_context *ctx = arg;
  mtx_lock(&g_handles_mtx);
  error err = eok();
  HWND *disabled_windows = NULL;
  struct bridge_event_config_response *resp = NULL;
  if (!g_ipcc) {
    goto cleanup;
  }

  struct ipcclient_response r = {0};
  // disable all windows to prevent any unexpected troubles.
  err = disable_family_windows(0, &disabled_windows);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  err = ipcclient_call(g_ipcc,
                       &(struct ipcclient_request){
                           .event_id = bridge_event_config,
                           .size = sizeof(struct bridge_event_config_request),
                           .ptr =
                               &(struct bridge_event_config_request){
                                   .window = (uint64_t)ctx->window,
                               },
                       },
                       &r);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  if (!r.ptr) {
    err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("config failed on remote")));
    goto cleanup;
  }
  if (r.size != sizeof(struct bridge_event_config_response)) {
    err = errg(err_unexpected);
    goto cleanup;
  }
  char *ptr = r.ptr;
  resp = (void *)ptr;
  ctx->ret = resp->success ? TRUE : FALSE;
cleanup:
  restore_disabled_family_windows(disabled_windows);
  mtx_unlock(&g_handles_mtx);
  if (efailed(err)) {
    ctx->err = err;
  }
  SetEvent(ctx->event);
  return 0;
}

static BOOL ffmpeg_input_config(HWND window, HINSTANCE dll_hinst) {
  if (atomic_load(&g_running_state) != rs_running) {
    return FALSE;
  }
  (void)dll_hinst;
  error err = eok();
  struct config_thread_context ctx = {
      .window = window,
      .event = CreateEventW(NULL, FALSE, FALSE, NULL),
      .ret = FALSE,
      .err = eok(),
  };
  if (!ctx.event) {
    err = errhr(HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  thrd_t th;
  if (thrd_create(&th, config_thread, &ctx) != thrd_success) {
    err = emsg(err_type_generic, err_unexpected, &native_unmanaged_const(NSTR("failed to start new thread")));
    goto cleanup;
  }
  thrd_detach(th);
  MSG msg = {0};
  for (;;) {
    DWORD r = MsgWaitForMultipleObjects(1, &ctx.event, FALSE, INFINITE, QS_ALLINPUT);
    switch (r) {
    case WAIT_OBJECT_0:
      goto cleanup;
    case WAIT_OBJECT_0 + 1:
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
cleanup:
  if (esucceeded(err)) {
    err = ctx.err;
    ctx.err = NULL;
  } else {
    efree(&ctx.err);
  }
  if (ctx.event) {
    CloseHandle(ctx.event);
  }
  if (efailed(err)) {
    ereport(err);
  }
  return ctx.ret;
}

static BOOL ffmpeg_input_info_get(INPUT_HANDLE ih, INPUT_INFO *iip) {
  if (atomic_load(&g_running_state) != rs_running || !ih) {
    return FALSE;
  }
  mtx_lock(&g_handles_mtx);
  error err = eok();
  struct bridge_event_get_info_response *resp = NULL;
  struct handle *h = (void *)ih;
  struct ipcclient_response r = {0};
  err = ipcclient_call(g_ipcc,
                       &(struct ipcclient_request){
                           .event_id = bridge_event_get_info,
                           .size = sizeof(struct bridge_event_get_info_request),
                           .ptr =
                               &(struct bridge_event_get_info_request){
                                   .id = h->id,
                               },
                       },
                       &r);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  if (!r.ptr) {
    err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("get_info failed on remote")));
    goto cleanup;
  }
  if (r.size < sizeof(struct bridge_event_get_info_response)) {
    err = errg(err_unexpected);
    goto cleanup;
  }
  char *ptr = r.ptr;
  resp = (void *)ptr;
  if (!g_bih || g_bih_size < (size_t)resp->video_format_size) {
    err = mem(&g_bih, (size_t)resp->video_format_size, 1);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
    g_bih_size = (size_t)resp->video_format_size;
  }
  memcpy(g_bih, ptr + sizeof(struct bridge_event_get_info_response), (size_t)resp->video_format_size);
  if (!g_wfex || g_wfex_size < (size_t)resp->audio_format_size) {
    err = mem(&g_wfex, (size_t)resp->audio_format_size, 1);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
    g_wfex_size = (size_t)resp->audio_format_size;
  }
  memcpy(g_wfex,
         ptr + sizeof(struct bridge_event_get_info_response) + resp->video_format_size,
         (size_t)resp->audio_format_size);
  *iip = (INPUT_INFO){
      .flag = resp->flag,
      .rate = resp->rate,
      .scale = resp->scale,
      .n = resp->video_frames,
      .format = g_bih,
      .format_size = resp->video_format_size,
      .audio_n = resp->audio_samples,
      .audio_format = g_wfex,
      .audio_format_size = resp->audio_format_size,
      .handler = resp->handler,
  };
cleanup:
  if (efailed(err)) {
    ereport(err);
  }
  mtx_unlock(&g_handles_mtx);
  return resp && resp->success ? TRUE : FALSE;
}

static NODISCARD error call_read(INPUT_HANDLE ih, int start, int length, void *buf, int *written) {
  if (!ih || !buf || !written) {
    return errg(err_invalid_arugment);
  }
  error err = eok();
  struct handle *h = (void *)ih;
  struct bridge_event_read_response *resp = NULL;
  void *mapped = NULL;
  struct ipcclient_response r = {0};
  err = ipcclient_call(g_ipcc,
                       &(struct ipcclient_request){
                           .event_id = bridge_event_read,
                           .size = sizeof(struct bridge_event_read_request),
                           .ptr =
                               &(struct bridge_event_read_request){
                                   .id = h->id,
                                   .start = (int32_t)start,
                                   .length = (int32_t)length,
                               },
                       },
                       &r);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  if (!r.ptr) {
    err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("read failed on remote")));
    goto cleanup;
  }
  if (r.size != sizeof(struct bridge_event_read_response)) {
    err = errg(err_unexpected);
    goto cleanup;
  }
  resp = r.ptr;
  if (wcscmp(g_fmo_name, resp->fmo_name) != 0) {
    HANDLE fmo = OpenFileMappingW(FILE_MAP_READ, FALSE, resp->fmo_name);
    if (!h) {
      err = errhr(HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }
    if (g_fmo != NULL) {
      CloseHandle(g_fmo);
    }
    g_fmo = fmo;
    wcscpy(g_fmo_name, resp->fmo_name);
  }
  size_t const bytes = length == 0 ? (size_t)resp->written : h->sample_size * (size_t)resp->written;
  mapped = MapViewOfFile(g_fmo, FILE_MAP_READ, 0, 0, bytes);
  if (!mapped) {
    err = errhr(HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  memcpy(buf, mapped, bytes);
  *written = resp->written;
cleanup:
  if (mapped) {
    if (!UnmapViewOfFile(mapped)) {
      ereport(emsg(err_type_hresult,
                   HRESULT_FROM_WIN32(GetLastError()),
                   &native_unmanaged_const(NSTR("warn: UnmapViewOfFile failed"))));
    }
    mapped = NULL;
  }
  return err;
}

static int ffmpeg_input_read_video(INPUT_HANDLE ih, int frame, void *buf) {
  if (atomic_load(&g_running_state) != rs_running || !ih) {
    return 0;
  }
  mtx_lock(&g_handles_mtx);
  error err = eok();
  int written = 0;
  err = call_read(ih, frame, 0, buf, &written);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
cleanup:
  if (efailed(err)) {
    ereport(err);
  }
  mtx_unlock(&g_handles_mtx);
  return written;
}

static int ffmpeg_input_read_audio(INPUT_HANDLE ih, int start, int length, void *buf) {
  if (atomic_load(&g_running_state) != rs_running || !ih) {
    return 0;
  }
  mtx_lock(&g_handles_mtx);
  error err = eok();
  int written = 0;
  err = call_read(ih, start, length, buf, &written);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
cleanup:
  if (efailed(err)) {
    ereport(err);
  }
  mtx_unlock(&g_handles_mtx);
  return written;
}

static NODISCARD error call_close(struct ipcclient *const ipcc, uint64_t const id, bool *const success) {
  if (!ipcc || !id || !success) {
    return errg(err_invalid_arugment);
  }
  error err = eok();
  struct bridge_event_close_response *resp = NULL;
  struct ipcclient_response r = {0};
  err = ipcclient_call(ipcc,
                       &(struct ipcclient_request){
                           .event_id = bridge_event_close,
                           .size = sizeof(struct bridge_event_close_request),
                           .ptr =
                               &(struct bridge_event_close_request){
                                   .id = id,
                               },
                       },
                       &r);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  if (!r.ptr) {
    err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("close failed on remote")));
    goto cleanup;
  }
  if (r.size != sizeof(struct bridge_event_close_response)) {
    err = errg(err_unexpected);
    goto cleanup;
  }
  resp = r.ptr;
  *success = resp->success;
cleanup:
  return err;
}

static BOOL ffmpeg_input_close(INPUT_HANDLE ih) {
  if (atomic_load(&g_running_state) != rs_running || !ih) {
    return FALSE;
  }
  mtx_lock(&g_handles_mtx);
  error err = eok();
  bool success = false;
  struct handle *h = (void *)ih;
  err = call_close(g_ipcc, h->id, &success);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  ereport(sfree(&h->filepath));
  ereport(hmdelete(&g_handles,
                   (&(struct handle_map_item){
                       .key = h,
                   }),
                   NULL));
  ereport(mem_free(&h));
cleanup:
  if (efailed(err)) {
    ereport(err);
  }
  mtx_unlock(&g_handles_mtx);
  return success ? TRUE : FALSE;
}

static NODISCARD error call_open(struct ipcclient *const ipcc, char const *const filepath, struct handle *h) {
  if (!ipcc || !filepath || !h) {
    return errg(err_invalid_arugment);
  }
  error err = eok();
  size_t const l = strlen(filepath);
  size_t req_size = sizeof(struct bridge_event_open_request) + l;
  void *buffer = NULL;
  err = ipcclient_grow_buffer(ipcc, req_size, &buffer);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  struct bridge_event_open_request *req = buffer;
  *req = (struct bridge_event_open_request){
      .filepath_size = (int32_t)l,
  };
  memcpy(req + 1, filepath, l);
  struct ipcclient_response r = {0};
  err = ipcclient_call(ipcc,
                       &(struct ipcclient_request){
                           .event_id = bridge_event_open,
                           .size = (uint32_t)req_size,
                           .ptr = buffer,
                       },
                       &r);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  if (!r.ptr) {
    err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("open failed on remote")));
    goto cleanup;
  }
  if (r.size != sizeof(struct bridge_event_open_response)) {
    err = errg(err_unexpected);
    goto cleanup;
  }
  struct bridge_event_open_response *resp = r.ptr;
  *h = (struct handle){
      .id = resp->id,
      .frame_size = (size_t)resp->frame_size,
      .sample_size = (size_t)resp->sample_size,
  };
cleanup:
  return err;
}

static INPUT_HANDLE ffmpeg_input_open(char *filepath) {
  if (atomic_load(&g_running_state) != rs_running || !filepath) {
    return NULL;
  }
  mtx_lock(&g_handles_mtx);
  error err = eok();
  struct handle *h = NULL;
  struct handle tmp = {0};
  err = call_open(g_ipcc, filepath, &tmp);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  err = mem(&h, 1, sizeof(struct handle));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  err = scpy(&tmp.filepath, filepath);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  *h = tmp;
  err = hmset(&g_handles,
              (&(struct handle_map_item){
                  .key = h,
              }),
              NULL);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
cleanup:
  if (efailed(err)) {
    if (h) {
      ereport(mem_free(&h));
    }
    if (tmp.id) {
      bool success = false;
      ereport(call_close(g_ipcc, tmp.id, &success));
    }
    if (tmp.filepath.ptr) {
      ereport(sfree(&tmp.filepath));
    }
    ereport(err);
  }
  mtx_unlock(&g_handles_mtx);
  return (INPUT_HANDLE)h;
}

static void process_finished(void *userdata);

static bool ipcc_is_aborted(void *userdata) {
  (void)userdata;
  return atomic_load(&g_running_state) != rs_booting;
}

static NODISCARD error start_process(struct process **const pp, struct ipcclient **const cp) {
  if (!pp || *pp || !cp || *cp) {
    return errg(err_invalid_arugment);
  }
  error err = eok();
  struct wstr module = {0};
  err = get_module_file_name(get_hinstance(), &module);
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
  if (ext < 7) {
    err = errg(err_fail);
    goto cleanup;
  }
  wchar_t *bits = NULL;
  if (wcsnicmp(L"-brdg32", module.ptr + ext - 7, 7) == 0) {
    bits = L"32";
  } else if (wcsnicmp(L"-brdg64", module.ptr + ext - 7, 7) == 0) {
    bits = L"64";
  } else {
    err = emsg(err_type_generic, err_fail, &native_unmanaged_const(NSTR("failed to detect bridge target module")));
    goto cleanup;
  }

  module.ptr[ext - 7] = '\0';
  module.len = ext - 7;
  err = scatm(&module, L".", bits, L"aui");
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }

  err = process_create(pp,
                       &(struct process_options){
                           .module_path = module.ptr,
                           .on_terminate = process_finished,
                       });
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  wchar_t pipe_name[64] = {0};
  build_pipe_name(pipe_name, process_get_unique_id(*pp));
  err = ipcclient_create(cp,
                         &(struct ipcclient_options){
                             .pipe_name = pipe_name,
                             .signature = BRIDGE_IPC_SIGNATURE,
                             .protocol_version = BRIDGE_IPC_VERSION,
                             // Remote process may not start immediately due to blocking by security software.
                             // Wait a little longer as it may be unblocked by user interaction.
                             .connect_timeout_msec = 30 * 1000,
                             .is_aborted = ipcc_is_aborted,
                         });
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
cleanup:
  ereport(sfree(&module));
  if (efailed(err)) {
    if (*cp) {
      ereport(ipcclient_destroy(cp));
    }
    if (*pp) {
      ereport(process_destroy(pp));
    }
  }
  return err;
}

struct restore_handle_context {
  struct ipcclient *ipcc;
  error err;
};

static bool restore_handle(void const *const item, void *const udata) {
  struct handle_map_item *hmi = ov_deconster_(item);
  struct restore_handle_context *ctx = udata;
  struct handle tmp = {0};
  ctx->err = call_open(ctx->ipcc, hmi->key->filepath.ptr, &tmp);
  if (efailed(ctx->err)) {
    ctx->err = ethru(ctx->err);
    return false;
  }
  hmi->key->id = tmp.id;
  return true;
}

static void process_finished(void *userdata) {
  switch (atomic_load(&g_running_state)) {
  case rs_booting:
    // It seems remote process crashed in very early stage.
    // There is no point in continuing the connection attempt in this situation.
    atomic_store(&g_running_state, rs_exiting);
    return;
  case rs_running:
    break;
  default:
    return;
  }
  (void)userdata;
  error err = eok();
  HWND window = find_aviutl_window();
  HWND *disabled_windows = NULL;
  struct process *new_process = NULL;
  struct ipcclient *new_ipcc = NULL;
  atomic_store(&g_running_state, rs_exiting);
  mtx_lock(&g_handles_mtx);
  if (g_ipcc) {
    ereport(ipcclient_destroy(&g_ipcc));
  }
  if (g_process) {
    ereport(process_destroy(&g_process));
  }
  ereport(disable_family_windows(window, &disabled_windows));
  int const r = MessageBoxW(window,
                            L"動画読み込み用プロセスの異常終了を検知しました。\r\n"
                            L"このままだとすべての動画読み込み処理に失敗します。\r\n\r\n"
                            L"プロセスの再起動を試みますか？",
                            L"ffmpeg Video Reader Bridge " VERSION_WIDE,
                            MB_ICONWARNING | MB_OKCANCEL);
  if (r != IDOK) {
    goto cleanup;
  }
  atomic_store(&g_running_state, rs_booting);
  err = start_process(&new_process, &new_ipcc);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  struct restore_handle_context ctx = {
      .ipcc = new_ipcc,
      .err = eok(),
  };
  err = hmscan(&g_handles, restore_handle, &ctx);
  if (eisg(err, err_abort)) {
    efree(&err);
    err = ctx.err;
    ctx.err = NULL;
  }
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  atomic_store(&g_running_state, rs_running);
cleanup:
  if (efailed(err)) {
    if (new_ipcc) {
      ereport(ipcclient_destroy(&new_ipcc));
    }
    if (new_process) {
      ereport(process_destroy(&new_process));
    }
    atomic_store(&g_running_state, rs_unknown);
  } else {
    g_process = new_process;
    g_ipcc = new_ipcc;
  }
  restore_disabled_family_windows(disabled_windows);
  mtx_unlock(&g_handles_mtx);
  ereport(err);
}

static BOOL ffmpeg_input_init(void) {
  error err = eok();
  bool mtx_initialized = false;
  if (mtx_init(&g_handles_mtx, mtx_plain | mtx_recursive) != thrd_success) {
    err = errg(err_unexpected);
    goto cleanup;
  }
  mtx_initialized = true;

  mtx_lock(&g_handles_mtx);

  err = hmnews(&g_handles, sizeof(struct handle_map_item), 4, sizeof(struct handle *));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  atomic_store(&g_running_state, rs_booting);
  err = start_process(&g_process, &g_ipcc);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
cleanup:
  if (efailed(err)) {
    if (g_handles.ptr) {
      ereport(hmfree(&g_handles));
    }
    if (mtx_initialized) {
      mtx_destroy(&g_handles_mtx);
    }
    error_message_box(err, L"ffmpeg Video Reader の初期化に失敗しました。");
    atomic_store(&g_running_state, rs_unknown);
    return FALSE;
  }
  mtx_unlock(&g_handles_mtx);
  atomic_store(&g_running_state, rs_running);
  return TRUE;
}

static BOOL ffmpeg_input_exit(void) {
  atomic_store(&g_running_state, rs_exiting);
  if (g_ipcc) {
    ereport(ipcclient_destroy(&g_ipcc));
  }
  if (g_process) {
    ereport(process_destroy(&g_process));
  }
  if (g_handles.ptr) {
    ereport(hmfree(&g_handles));
  }
  mtx_destroy(&g_handles_mtx);
  if (g_bih) {
    ereport(mem_free(&g_bih));
  }
  if (g_wfex) {
    ereport(mem_free(&g_wfex));
  }
  return TRUE;
}

#define VIDEO_EXTS "*.mp4;*.mkv;*.avi;*.mov;*.wmv;*.webm;*.mpeg;*.ts;*.mts;*.m2ts"
// #define AUDIO_EXTS "*.mp3;*.ogg;*.wav;*.aac;*.wma;*.m4a;*.webm;*.opus"

INPUT_PLUGIN_TABLE *get_input_plugin_bridge_table(void) {
  static INPUT_PLUGIN_TABLE table = {
      .flag = INPUT_PLUGIN_FLAG_VIDEO | INPUT_PLUGIN_FLAG_AUDIO,
      .name = "FFmpeg Video Reader Bridge",
      .filefilter = "FFmpeg Supported Files (" VIDEO_EXTS ")\0" VIDEO_EXTS "\0",
      .information = "FFmpeg Video Reader Bridge " VERSION,
      .func_init = ffmpeg_input_init,
      .func_exit = ffmpeg_input_exit,
      .func_open = ffmpeg_input_open,
      .func_close = ffmpeg_input_close,
      .func_info_get = ffmpeg_input_info_get,
      .func_read_video = ffmpeg_input_read_video,
      .func_read_audio = ffmpeg_input_read_audio,
      // .func_is_keyframe = ffmpeg_input_is_keyframe,
      .func_config = ffmpeg_input_config,
  };
  return &table;
}

#undef VIDEO_EXTS
