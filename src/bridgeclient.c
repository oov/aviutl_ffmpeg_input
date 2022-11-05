#include "bridgeclient.h"

#include "ovbase.h"
#include "ovutil/str.h"
#include "ovutil/win32.h"

#include "bridgecommon.h"
#include "error.h"
#include "ipcclient.h"
#include "process.h"
#include "version.h"

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
  size_t frame_size;
  size_t sample_size;
};

static BOOL ffmpeg_input_info_get(INPUT_HANDLE ih, INPUT_INFO *iip) {
  if (!g_ipcc || !ih) {
    return FALSE;
  }
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
    return FALSE;
  }
  return resp && resp->success ? TRUE : FALSE;
}

static NODISCARD error read(INPUT_HANDLE ih, int start, int length, void *buf, int *written) {
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
  if (!g_ipcc || !ih) {
    return 0;
  }
  int written = 0;
  error err = read(ih, frame, 0, buf, &written);
  if (efailed(err)) {
    err = ethru(err);
    ereport(err);
    return 0;
  }
  return written;
}

static int ffmpeg_input_read_audio(INPUT_HANDLE ih, int start, int length, void *buf) {
  if (!g_ipcc || !ih) {
    return 0;
  }
  int written = 0;
  error err = read(ih, start, length, buf, &written);
  if (efailed(err)) {
    err = ethru(err);
    ereport(err);
    return 0;
  }
  return written;
}

static BOOL ffmpeg_input_close(INPUT_HANDLE ih) {
  if (!g_ipcc || !ih) {
    return FALSE;
  }
  error err = eok();
  struct handle *h = (void *)ih;
  struct bridge_event_close_response *resp = NULL;
  struct ipcclient_response r = {0};
  err = ipcclient_call(g_ipcc,
                       &(struct ipcclient_request){
                           .event_id = bridge_event_close,
                           .size = sizeof(struct bridge_event_close_request),
                           .ptr =
                               &(struct bridge_event_close_request){
                                   .id = h->id,
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
cleanup:
  if (efailed(err)) {
    ereport(err);
    return FALSE;
  } else {
    ereport(mem_free(&h));
  }
  return resp && resp->success ? TRUE : FALSE;
}

static INPUT_HANDLE ffmpeg_input_open(char *filepath) {
  if (!g_ipcc || !filepath) {
    return NULL;
  }
  error err = eok();
  struct handle *h = NULL;
  size_t const l = strlen(filepath);
  size_t req_size = sizeof(struct bridge_event_open_request) + l;
  void *buffer = NULL;
  err = ipcclient_grow_buffer(g_ipcc, req_size, &buffer);
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
  err = ipcclient_call(g_ipcc,
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
  err = mem(&h, 1, sizeof(struct handle));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  *h = (struct handle){
      .id = resp->id,
      .frame_size = (size_t)resp->frame_size,
      .sample_size = (size_t)resp->sample_size,
  };
cleanup:
  if (efailed(err)) {
    ereport(err);
  }
  return (INPUT_HANDLE)h;
}

static BOOL ffmpeg_input_init(void) {
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

  err = process_create(&g_process,
                       &(struct process_options){
                           .module_path = module.ptr,
                       });
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  wchar_t pipe_name[64] = {0};
  build_pipe_name(pipe_name, process_get_unique_id(g_process));
  err = ipcclient_create(&g_ipcc,
                         &(struct ipcclient_options){
                             .pipe_name = pipe_name,
                             .signature = BRIDGE_IPC_SIGNATURE,
                             .protocol_version = BRIDGE_IPC_VERSION,
                             // Remote process may not start immediately due to blocking by security software.
                             // Wait a little longer as it may be unblocked by user interaction.
                             .connect_timeout_msec = 30 * 1000,
                         });
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
cleanup:
  ereport(sfree(&module));
  if (efailed(err)) {
    if (g_ipcc) {
      ereport(ipcclient_destroy(&g_ipcc));
    }
    if (g_process) {
      ereport(process_destroy(&g_process));
    }
    error_message_box(err, L"ffmpeg Video Reader の初期化に失敗しました。");
    return FALSE;
  }
  return TRUE;
}

static BOOL ffmpeg_input_exit(void) {
  if (g_ipcc) {
    ereport(ipcclient_destroy(&g_ipcc));
  }
  if (g_process) {
    ereport(process_destroy(&g_process));
  }
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
      .name = "ffmpeg Video Reader Bridge",
      .filefilter = "ffmpeg Supported Files (" VIDEO_EXTS ")\0" VIDEO_EXTS "\0",
      .information = "ffmpeg Video Reader Bridge " VERSION,
      .func_init = ffmpeg_input_init,
      .func_exit = ffmpeg_input_exit,
      .func_open = ffmpeg_input_open,
      .func_close = ffmpeg_input_close,
      .func_info_get = ffmpeg_input_info_get,
      .func_read_video = ffmpeg_input_read_video,
      .func_read_audio = ffmpeg_input_read_audio,
      // .func_is_keyframe = ffmpeg_input_is_keyframe,
      .func_config = NULL,
  };
  return &table;
}

#undef VIDEO_EXTS
