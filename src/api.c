#include "api.h"

#include "ovbase.h"
#include "ovutil/str.h"
#include "ovutil/win32.h"

#include "config.h"
#include "error.h"
#include "ffmpeg.h"
#include "stream.h"
#include "version.h"

static bool g_ready = false;
static struct streammap *g_smp = NULL;

static BOOL ffmpeg_input_info_get(INPUT_HANDLE ih, INPUT_INFO *iip) {
  if (!g_ready) {
    return FALSE;
  }
  static BITMAPINFOHEADER bih = {0};
  struct info_video const *const vi = streammap_get_video_info(g_smp, (intptr_t)ih);
  if (vi->width) {
    bih = (BITMAPINFOHEADER){
        .biSize = sizeof(BITMAPINFOHEADER),
        .biWidth = vi->width,
        .biHeight = vi->height,
        .biCompression = vi->is_rgb ? MAKEFOURCC('B', 'G', 'R', 0) : MAKEFOURCC('Y', 'U', 'Y', '2'),
        .biBitCount = (WORD)vi->bit_depth,
    };
    iip->flag |= INPUT_INFO_FLAG_VIDEO | INPUT_INFO_FLAG_VIDEO_RANDOM_ACCESS;
    iip->rate = vi->frame_rate;
    iip->scale = vi->frame_scale;
    iip->n = (int)vi->frames;
    iip->format = &bih;
    iip->format_size = (int)bih.biSize;
  }

  static WAVEFORMATEX wfex = {0};
  struct info_audio const *const ai = streammap_get_audio_info(g_smp, (intptr_t)ih);
  if (ai->sample_rate) {
    wfex = (WAVEFORMATEX){
        .wFormatTag = WAVE_FORMAT_PCM,
        .nChannels = (WORD)ai->channels,
        .nSamplesPerSec = (DWORD)ai->sample_rate,
        .wBitsPerSample = (WORD)ai->bit_depth,
        .nBlockAlign = (WORD)(ai->channels * ai->bit_depth / 8),
        .nAvgBytesPerSec = (DWORD)(ai->sample_rate * ai->channels * ai->bit_depth / 8),
        .cbSize = 0,
    };
    iip->flag |= INPUT_INFO_FLAG_AUDIO;
    iip->audio_n = (int)ai->samples;
    iip->audio_format = &wfex;
    iip->audio_format_size = (int)(sizeof(WAVEFORMATEX) + wfex.cbSize);
  }
  return bih.biWidth || wfex.nSamplesPerSec;
}

static int ffmpeg_input_read_video_ex(INPUT_HANDLE ih, int frame, void *buf, bool const saving) {
  (void)saving;
  if (!g_ready) {
    return 0;
  }
  size_t wr = 0;
  error err = streammap_read_video(g_smp, (intptr_t)ih, (int64_t)frame, buf, &wr);
  if (efailed(err)) {
    ereport(err);
    return 0;
  }
  return (int)wr;
}

static int ffmpeg_input_read_video(INPUT_HANDLE ih, int frame, void *buf) {
  return ffmpeg_input_read_video_ex(ih, frame, buf, aviutl_is_saving());
}

static int ffmpeg_input_read_audio_ex(INPUT_HANDLE ih, int start, int length, void *buf, bool const saving) {
  if (!g_ready) {
    return 0;
  }
  int wr = 0;
  error err = streammap_read_audio(g_smp, (intptr_t)ih, (int64_t)start, (size_t)length, buf, &wr, saving);
  if (efailed(err)) {
    ereport(err);
    return 0;
  }
  return (int)wr;
}

static int ffmpeg_input_read_audio(INPUT_HANDLE ih, int start, int length, void *buf) {
  return ffmpeg_input_read_audio_ex(ih, start, length, buf, aviutl_is_saving());
}

static BOOL ffmpeg_input_close(INPUT_HANDLE ih) {
  if (!g_ready) {
    return FALSE;
  }
  error err = streammap_free_stream(g_smp, (intptr_t)ih);
  if (efailed(err)) {
    ereport(err);
    return FALSE;
  }
  return TRUE;
}

static int ffmpeg_input_open_ex(char const *filepath, INPUT_HANDLE *ih) {
  if (!filepath || !ih) {
    return EINVAL;
  }
  if (!g_ready) {
    return EACCES;
  }
  struct wstr ws = {0};
  intptr_t idx = 0;
  int eno = 0;
  error err = from_mbcs(&str_unmanaged_const(filepath), &ws);
  if (efailed(err)) {
    eno = EINVAL;
    err = ethru(err);
    goto cleanup;
  }
  err = streammap_create_stream(g_smp, ws.ptr, &idx);
  if (efailed(err)) {
    if (eisg(err, err_abort)) {
      eno = ECANCELED;
      efree(&err);
      goto cleanup;
    }
    eno = EIO;
    err = ethru(err);
    goto cleanup;
  }
cleanup:
  ereport(sfree(&ws));
  if (efailed(err)) {
    ereport(err);
  }
  *ih = (INPUT_HANDLE)idx;
  return eno;
}

static INPUT_HANDLE ffmpeg_input_open(char *filepath) {
  INPUT_HANDLE ih = NULL;
  ffmpeg_input_open_ex(filepath, &ih);
  return ih;
}

static HANDLE ffmpeg_dll_handles[5] = {0};

static BOOL ffmpeg_input_init(void) {
  error err = eok();
  struct wstr module_dir = {0};
  struct wstr old_search_path = {0};
  err = get_module_file_name(get_hinstance(), &module_dir);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  size_t fnpos = 0;
  err = extract_file_name(&module_dir, &fnpos);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  module_dir.ptr[fnpos] = L'\0';
  module_dir.len = fnpos;
  err = scatm(&module_dir, L"ffmpeg", sizeof(size_t) == 4 ? L"32" : L"64", L"\\bin\\");
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }

  DWORD const searchlen = GetDllDirectoryW(0, NULL);
  if (!searchlen) {
    err = errhr(HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  err = sgrow(&old_search_path, searchlen + 1);
  GetDllDirectoryW(searchlen, old_search_path.ptr);
  old_search_path.ptr[searchlen] = L'\0';
  old_search_path.len = searchlen;

  SetDllDirectoryW(module_dir.ptr);
#define TOSTR3(name, ver) L##name L##ver
#define TOSTR2(name, ver) TOSTR3(name, #ver)
#define TOSTR(name, ver) TOSTR2(name, ver)
  static struct ffmpeg_dll {
    wchar_t const *name;
    unsigned compiled_version;
    unsigned (*dynamic_version)(void);
    const char *(*dynamic_license)(void);
  } const ffmpeg_dll[5] = {
      {
          .name = TOSTR("avcodec-", LIBAVCODEC_VERSION_MAJOR),
          .compiled_version = LIBAVCODEC_VERSION_INT,
          .dynamic_version = avcodec_version,
          .dynamic_license = avcodec_license,
      },
      {
          .name = TOSTR("avformat-", LIBAVFORMAT_VERSION_MAJOR),
          .compiled_version = LIBAVFORMAT_VERSION_INT,
          .dynamic_version = avformat_version,
          .dynamic_license = avformat_license,
      },
      {
          .name = TOSTR("avutil-", LIBAVUTIL_VERSION_MAJOR),
          .compiled_version = LIBAVUTIL_VERSION_INT,
          .dynamic_version = avutil_version,
          .dynamic_license = avutil_license,
      },
      {
          .name = TOSTR("swscale-", LIBSWSCALE_VERSION_MAJOR),
          .compiled_version = LIBSWSCALE_VERSION_INT,
          .dynamic_version = swscale_version,
          .dynamic_license = swscale_license,
      },
      {
          .name = TOSTR("swresample-", LIBSWRESAMPLE_VERSION_MAJOR),
          .compiled_version = LIBSWRESAMPLE_VERSION_INT,
          .dynamic_version = swresample_version,
          .dynamic_license = swresample_license,
      },
  };
#undef TOSTR
#undef TOSTR2
#undef TOSTR3
  for (size_t i = 0; i < sizeof(ffmpeg_dll) / sizeof(ffmpeg_dll[0]); ++i) {
    ffmpeg_dll_handles[i] = LoadLibraryW(ffmpeg_dll[i].name);
    if (!ffmpeg_dll_handles[i]) {
      wchar_t buf[1024];
      wsprintfW(buf, L"%s を開けませんでした。", ffmpeg_dll[i].name);
      err = emsg(err_type_hresult, HRESULT_FROM_WIN32(GetLastError()), &native_unmanaged_const(buf));
      goto cleanup;
    }
    if (!strstr(ffmpeg_dll[i].dynamic_license(), "LGPL")) {
      wchar_t buf[1024];
      wsprintfW(buf,
                L"%s が LGPL 版ではないため処理を続行できません。\r\n検出されたライセンス: %hs",
                ffmpeg_dll[i].name,
                ffmpeg_dll[i].dynamic_license());
      err = emsg(err_type_generic, err_abort, &native_unmanaged_const(buf));
      goto cleanup;
    }
    if (AV_VERSION_MAJOR(ffmpeg_dll[i].compiled_version) != AV_VERSION_MAJOR(ffmpeg_dll[i].dynamic_version())) {
      wchar_t buf[1024];
      wsprintfW(buf,
                L"%s のバージョンが一致しません。\r\n必要なバージョン: %d\r\nDLLのバージョン: %d",
                ffmpeg_dll[i].name,
                AV_VERSION_MAJOR(ffmpeg_dll[i].compiled_version),
                AV_VERSION_MAJOR(ffmpeg_dll[i].dynamic_version()));
      err = emsg(err_type_generic, err_abort, &native_unmanaged_const(buf));
      goto cleanup;
    }
  }

  g_ready = true;

  err = streammap_create(&g_smp);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
cleanup:
  if (old_search_path.ptr) {
    SetDllDirectoryW(old_search_path.ptr);
  }
  ereport(sfree(&old_search_path));
  ereport(sfree(&module_dir));
  if (efailed(err)) {
    error_message_box(err, L"初期化中にエラーが発生しました。");
    return FALSE;
  }
  return TRUE;
}

static BOOL ffmpeg_input_exit(void) {
  streammap_destroy(&g_smp);
  for (size_t i = 0; i < sizeof(ffmpeg_dll_handles) / sizeof(ffmpeg_dll_handles[0]); ++i) {
    if (ffmpeg_dll_handles[i]) {
      FreeLibrary(ffmpeg_dll_handles[i]);
      ffmpeg_dll_handles[i] = NULL;
    }
  }
  return TRUE;
}

struct config_dialog_props {
  struct config *config;
  error err;
};

static wchar_t const config_prop[] = L"config_prop";

struct combo_items {
  int const id;
  wchar_t const *name;
};

static struct combo_items const scaling_algorithms[] = {
    {video_format_scaling_algorithm_fast_bilinear, L"fast bilinear"},
    {video_format_scaling_algorithm_bilinear, L"bilinear"},
    {video_format_scaling_algorithm_bicubic, L"bicubic"},
    {video_format_scaling_algorithm_x, L"experimental"},
    {video_format_scaling_algorithm_point, L"nearest neighbor"},
    {video_format_scaling_algorithm_area, L"averaging area"},
    {video_format_scaling_algorithm_bicublin, L"luma bicubic, chroma bilinear"},
    {video_format_scaling_algorithm_gauss, L"Gaussian"},
    {video_format_scaling_algorithm_sinc, L"sinc"},
    {video_format_scaling_algorithm_lanczos, L"Lanczos"},
    {video_format_scaling_algorithm_spline, L"natural bicubic spline"},
    {0},
};

static struct combo_items const handle_manage_modes[] = {
    {chmm_normal, L"通常"},
    {chmm_cache, L"ハンドルキャッシュ"},
    {chmm_pool, L"ハンドルプール"},
    {0},
};

static struct combo_items const audio_index_modes[] = {
    {aim_noindex, L"なし"},
    {aim_relax, L"リラックス"},
    {aim_strict, L"正確"},
    {0},
};

enum config_control {
  ID_BTN_ABOUT = 100,
  ID_CHK_NEED_POSTFIX = 1000,
  ID_EDT_DECODERS = 1001,
  ID_CMB_HANDLE_MANAGE_MODE = 1002,
  ID_CMB_SCALING = 2000,
  ID_CMB_AUDIO_INDEX_MODE = 3000,
  ID_CHK_INVERT_PHASE = 3001,
};

static wchar_t *ver_to_str(wchar_t *const buf, char const *const ident, unsigned int ver) {
  wsprintfW(
      buf, L"  %hs linked to %d.%d.%d\r\n", ident, AV_VERSION_MAJOR(ver), AV_VERSION_MINOR(ver), AV_VERSION_MICRO(ver));
  return buf;
}

static bool get_check(HWND const window, int const control_id) {
  return SendMessageW(GetDlgItem(window, control_id), BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static void set_check(HWND const window, int const control_id, bool const checked) {
  SendMessageW(GetDlgItem(window, control_id), BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
}

static int get_combo(HWND const window, int const control_id, struct combo_items const *items) {
  size_t const selected_index = (size_t)(SendMessageW(GetDlgItem(window, control_id), CB_GETCURSEL, 0, 0));
  for (size_t i = 0; items[i].name; ++i) {
    if (i == selected_index) {
      return items[i].id;
    }
  }
  return -1;
}

static void set_combo(HWND const window, int const control_id, struct combo_items const *items, int const selected_id) {
  HWND h = GetDlgItem(window, control_id);
  size_t selected_index = 0;
  for (size_t i = 0; items[i].name; ++i) {
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)items[i].name);
    if (items[i].id == selected_id) {
      selected_index = i;
    }
  }
  SendMessageW(h, CB_SETCURSEL, (WPARAM)selected_index, 0);
}

static INT_PTR CALLBACK config_wndproc(HWND const dlg, UINT const message, WPARAM const wparam, LPARAM const lparam) {
  switch (message) {
  case WM_INITDIALOG: {
    struct config_dialog_props *const pr = (void *)lparam;
    SetPropW(dlg, config_prop, (HANDLE)pr);
    SetWindowTextW(dlg, "FFmpeg Video Reader " VERSION_WIDE);
    set_check(dlg, ID_CHK_NEED_POSTFIX, config_get_need_postfix(pr->config));
    SetWindowTextA(GetDlgItem(dlg, ID_EDT_DECODERS), config_get_preferred_decoders(pr->config));
    set_combo(dlg, ID_CMB_HANDLE_MANAGE_MODE, handle_manage_modes, (int)(config_get_handle_manage_mode(pr->config)));
    set_combo(dlg, ID_CMB_SCALING, scaling_algorithms, (int)(config_get_scaling(pr->config)));
    set_combo(dlg, ID_CMB_AUDIO_INDEX_MODE, audio_index_modes, (int)(config_get_audio_index_mode(pr->config)));
    set_check(dlg, ID_CHK_INVERT_PHASE, config_get_invert_phase(pr->config));
    return TRUE;
  }
  case WM_DESTROY:
    RemovePropW(dlg, config_prop);
    return 0;
  case WM_COMMAND:
    switch (LOWORD(wparam)) {
    case IDOK: {
      struct str s = {0};
      error err = eok();
      struct config_dialog_props *const pr = (void *)GetPropW(dlg, config_prop);
      if (!pr) {
        err = errg(err_unexpected);
        goto cleanup;
      }
      err = config_set_handle_manage_mode(
          pr->config, (enum config_handle_manage_mode)(get_combo(dlg, ID_CMB_HANDLE_MANAGE_MODE, handle_manage_modes)));
      if (efailed(err)) {
        err = ethru(err);
        goto cleanup;
      }
      err = config_set_need_postfix(pr->config, get_check(dlg, ID_CHK_NEED_POSTFIX));
      if (efailed(err)) {
        err = ethru(err);
        goto cleanup;
      }
      HWND h = GetDlgItem(dlg, ID_EDT_DECODERS);
      int len = GetWindowTextLengthA(h);
      err = sgrow(&s, (size_t)len + 1);
      if (efailed(err)) {
        err = ethru(err);
        goto cleanup;
      }
      GetWindowTextA(h, s.ptr, len + 1);
      s.ptr[len] = '\0';
      s.len = (size_t)len;
      err = config_set_preferred_decoders(pr->config, s.ptr);
      if (efailed(err)) {
        err = ethru(err);
        goto cleanup;
      }
      err = config_set_scaling(
          pr->config, (enum video_format_scaling_algorithm)(get_combo(dlg, ID_CMB_SCALING, scaling_algorithms)));
      if (efailed(err)) {
        err = ethru(err);
        goto cleanup;
      }
      err = config_set_audio_index_mode(
          pr->config, (enum audio_index_mode)(get_combo(dlg, ID_CMB_AUDIO_INDEX_MODE, audio_index_modes)));
      if (efailed(err)) {
        err = ethru(err);
        goto cleanup;
      }
      err = config_set_invert_phase(pr->config, get_check(dlg, ID_CHK_INVERT_PHASE));
      if (efailed(err)) {
        err = ethru(err);
        goto cleanup;
      }
    cleanup:
      ereport(sfree(&s));
      if (efailed(err)) {
        pr->err = err;
        err = NULL;
        EndDialog(dlg, 0);
        return TRUE;
      }
      EndDialog(dlg, IDOK);
      return TRUE;
    }
    case IDCANCEL:
      EndDialog(dlg, IDCANCEL);
      return TRUE;
    case ID_BTN_ABOUT: {
      wchar_t buf[2048];
      wchar_t tmp[256];
      wcscpy(buf,
             L"This software uses libraries from the FFmpeg project under the LGPLv2.1.\r\n"
             L"Copyright (c) 2003-2022 the FFmpeg developers.\r\n");
      ver_to_str(tmp, LIBAVCODEC_IDENT, avcodec_version());
      wcscat(buf, tmp);
      ver_to_str(tmp, LIBAVFORMAT_IDENT, avformat_version());
      wcscat(buf, tmp);
      ver_to_str(tmp, LIBAVUTIL_IDENT, avutil_version());
      wcscat(buf, tmp);
      ver_to_str(tmp, LIBSWSCALE_IDENT, swscale_version());
      wcscat(buf, tmp);
      ver_to_str(tmp, LIBSWRESAMPLE_IDENT, swresample_version());
      wcscat(buf, tmp);
      wcscat(buf, L"\r\n");
      wcscat(buf,
             L"This software uses OpenH264 binary that released from Cisco Systems, Inc.\r\n"
             L"OpenH264 Video Codec provided by Cisco Systems, Inc.\r\n"
             L"Copyright (c) 2014 Cisco Systems, Inc. All rights reserved.\r\n");
      message_box(dlg, buf, L"About", MB_OK);
      return TRUE;
    }
    }
    break;
  }
  return FALSE;
}

static BOOL ffmpeg_input_config(HWND window, HINSTANCE dll_hinst) {
  (void)dll_hinst;
  struct config_dialog_props pr = {
      .config = NULL,
      .err = eok(),
  };
  error err = config_create(&pr.config);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  err = config_load(pr.config);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  INT_PTR r = DialogBoxParamW(get_hinstance(), L"CONFIG", window, config_wndproc, (LPARAM)&pr);
  if (r == 0 || r == -1) {
    if (efailed(pr.err)) {
      err = ethru(pr.err);
      pr.err = NULL;
    } else {
      err = errhr(HRESULT_FROM_WIN32(GetLastError()));
    }
    goto cleanup;
  }
  if (r == IDCANCEL) {
    goto cleanup;
  }
  err = config_save(pr.config);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
cleanup:
  config_destroy(&pr.config);
  ereport(err);
  return TRUE;
}

#define VIDEO_EXTS "*.mkv;*.avi;*.mov;*.wmv;*.mp4;*.webm;*.mpeg;*.ts;*.mts;*.m2ts"
// #define AUDIO_EXTS "*.mp3;*.ogg;*.wav;*.aac;*.wma;*.m4a;*.webm;*.opus"
static INPUT_PLUGIN_TABLE g_input_plugin_table = {
    .flag = INPUT_PLUGIN_FLAG_VIDEO | INPUT_PLUGIN_FLAG_AUDIO,
    .name = "FFmpeg Video Reader",
    .filefilter = "FFmpeg Supported Files (" VIDEO_EXTS ")\0" VIDEO_EXTS "\0",
    .information = "FFmpeg Video Reader " VERSION,
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
#undef VIDEO_EXTS

INPUT_PLUGIN_TABLE *get_input_plugin_table(void) { return &g_input_plugin_table; }

static struct own_api const g_own_api = {
    .original_api = &g_input_plugin_table,
    .func_open_ex = ffmpeg_input_open_ex,
    .func_read_video_ex = ffmpeg_input_read_video_ex,
    .func_read_audio_ex = ffmpeg_input_read_audio_ex,
};

struct own_api const *get_own_api_endpoint(void) { return &g_own_api; }
