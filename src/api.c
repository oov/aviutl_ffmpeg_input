#include "api.h"

#include "ovbase.h"
#include "ovutil/str.h"
#include "ovutil/win32.h"

#include "audio.h"
#include "config.h"
#include "error.h"
#include "ffmpeg.h"
#include "version.h"
#include "video.h"

struct file {
  HANDLE file;
  struct video *v;
  struct audio *a;
  bool invert_phase;
  BITMAPINFOHEADER video_format;
  WAVEFORMATEX audio_format;
  INPUT_INFO info;
};

static bool ffmpeg_loaded = false;

static BOOL ffmpeg_input_info_get(INPUT_HANDLE ih, INPUT_INFO *iip) {
  struct file *fp = (void *)ih;
  *iip = fp->info;
  return fp->v || fp->a;
}

static int ffmpeg_input_read_video(INPUT_HANDLE ih, int frame, void *buf) {
  struct file *fp = (void *)ih;
  size_t written = 0;
  error err = video_read(fp->v, frame, buf, &written);
  if (efailed(err)) {
    ereport(err);
    return 0;
  }
  return (int)written;
}

static int ffmpeg_input_read_audio(INPUT_HANDLE ih, int start, int length, void *buf) {
  struct file *fp = (void *)ih;
  int written = 0;
  error err = audio_read(fp->a, (int64_t)start, length, buf, &written);
  if (efailed(err)) {
    ereport(err);
    return 0;
  }
  if (fp->invert_phase) {
    int16_t *w = buf;
    for (int i = 0; i < written; ++i) {
      *w = -*w;
      w++;
      *w = -*w;
      w++;
    }
  }
  return written;
}

static BOOL ffmpeg_input_close(INPUT_HANDLE ih) {
  struct file *fp = (void *)ih;
  if (fp) {
    video_destroy(&fp->v);
    audio_destroy(&fp->a);
    if (fp->file != INVALID_HANDLE_VALUE) {
      CloseHandle(fp->file);
      fp->file = INVALID_HANDLE_VALUE;
    }
    ereport(mem_free(&fp));
  }
  return TRUE;
}

static NODISCARD bool has_postfix(char *filepath) {
  if (!filepath) {
    return false;
  }
  bool r = false;
  struct wstr ws = {0};
  error err = from_mbcs(&str_unmanaged_const(filepath), &ws);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  size_t extpos = 0;
  err = extract_file_extension(&ws, &extpos);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  ws.ptr[extpos] = L'\0';
  ws.len = extpos;
  r = extpos > 7 && wcsicmp(ws.ptr + extpos - 7, L"-ffmpeg") == 0;
cleanup:
  ereport(sfree(&ws));
  ereport(err);
  return r;
}

static INPUT_HANDLE ffmpeg_input_open(char *filepath) {
  if (!ffmpeg_loaded) {
    return NULL;
  }
  error err = eok();
  HANDLE file = INVALID_HANDLE_VALUE;
  struct config *config = NULL;
  struct file *fp = NULL;
  struct video *v = NULL;
  struct audio *a = NULL;
  struct wstr ws = {0};
  struct info_video vi = {0};
  struct info_audio ai = {0};
  err = config_create(&config);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  err = config_load(config);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  if (config_get_need_postfix(config) && !has_postfix(filepath)) {
    err = emsg(err_type_generic, err_abort, &native_unmanaged_const(NSTR("filename does not contain \"-ffmpeg\".")));
    goto cleanup;
  }
  err = from_mbcs(&str_unmanaged_const(filepath), &ws);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  file = CreateFileW(ws.ptr, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE) {
    err = errhr(HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  err = video_create(&v,
                     &vi,
                     &(struct video_options){
                         .handle = file,
                         .preferred_decoders = config_get_preferred_decoders(config),
                         .scaling = config_get_scaling(config),
                     });
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  err = audio_create(&a,
                     &ai,
                     &(struct audio_options){
                         .handle = file,
                         .preferred_decoders = config_get_preferred_decoders(config),
                         .video_start_time = video_get_start_time(v),
                     });
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  if (!v && !a) {
    err = errg(err_fail);
    goto cleanup;
  }

  err = mem(&fp, 1, sizeof(struct file));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  *fp = (struct file){
      .file = file,
      .v = v,
      .a = a,
      .invert_phase = config_get_invert_phase(config),
  };
  INPUT_INFO *ii = &fp->info;
  if (v) {
    fp->video_format = (BITMAPINFOHEADER){
        .biSize = sizeof(BITMAPINFOHEADER),
        .biWidth = vi.width,
        .biHeight = vi.height,
        .biCompression = vi.is_rgb ? MAKEFOURCC('B', 'G', 'R', 0) : MAKEFOURCC('Y', 'U', 'Y', '2'),
        .biBitCount = (WORD)vi.bit_depth,
    };
    ii->flag |= INPUT_INFO_FLAG_VIDEO | INPUT_INFO_FLAG_VIDEO_RANDOM_ACCESS;
    ii->rate = vi.frame_rate;
    ii->scale = vi.frame_scale;
    ii->n = (int)vi.frames;
    ii->format = &fp->video_format;
    ii->format_size = (int)fp->video_format.biSize;
  }
  if (a) {
    fp->audio_format = (WAVEFORMATEX){
        .wFormatTag = WAVE_FORMAT_PCM,
        .nChannels = (WORD)ai.channels,
        .nSamplesPerSec = (DWORD)ai.sample_rate,
        .wBitsPerSample = (WORD)ai.bit_depth,
        .nBlockAlign = (WORD)(ai.channels * ai.bit_depth / 8),
        .nAvgBytesPerSec = (DWORD)(ai.sample_rate * ai.channels * ai.bit_depth / 8),
        .cbSize = 0,
    };
    ii->flag |= INPUT_INFO_FLAG_AUDIO;
    ii->audio_n = (int)ai.samples;
    ii->audio_format = &fp->audio_format;
    ii->audio_format_size = (int)(sizeof(WAVEFORMATEX) + fp->audio_format.cbSize);
  }

cleanup:
  ereport(sfree(&ws));
  if (config) {
    config_destroy(&config);
  }
  if (efailed(err)) {
    if (fp) {
      ereport(mem_free(&fp));
    }
    if (a) {
      audio_destroy(&a);
    }
    if (v) {
      video_destroy(&v);
    }
    if (file != INVALID_HANDLE_VALUE) {
      CloseHandle(file);
      file = INVALID_HANDLE_VALUE;
    }
    ereport(err);
    return NULL;
  }
  return (INPUT_HANDLE)fp;
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

  ffmpeg_loaded = true;
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

static struct {
  enum video_format_scaling_algorithm const id;
  wchar_t const *name;
} scaling_algorithms[] = {
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
};

enum config_control {
  ID_BTN_ABOUT = 100,
  ID_EDT_DECODERS = 1001,
  ID_CMB_SCALING = 1003,
  ID_CHK_USE_AUDIO_INDEX = 2000,
  ID_CHK_INVERT_PHASE = 2001,
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

static INT_PTR CALLBACK config_wndproc(HWND const dlg, UINT const message, WPARAM const wparam, LPARAM const lparam) {
  switch (message) {
  case WM_INITDIALOG: {
    struct config_dialog_props *const pr = (void *)lparam;
    SetPropW(dlg, config_prop, (HANDLE)pr);
    SetWindowTextW(dlg, "FFmpeg Video Reader " VERSION_WIDE);
    SetWindowTextA(GetDlgItem(dlg, ID_EDT_DECODERS), config_get_preferred_decoders(pr->config));
    HWND h = GetDlgItem(dlg, ID_CMB_SCALING);
    enum video_format_scaling_algorithm const id = config_get_scaling(pr->config);
    size_t selected_index = 0;
    for (size_t i = 0, len = sizeof(scaling_algorithms) / sizeof(scaling_algorithms[0]); i < len; ++i) {
      SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)scaling_algorithms[i].name);
      if (scaling_algorithms[i].id == id) {
        selected_index = i;
      }
    }
    SendMessageW(h, CB_SETCURSEL, (WPARAM)selected_index, 0);
    set_check(dlg, ID_CHK_USE_AUDIO_INDEX, config_get_use_audio_index(pr->config));
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
      size_t scaling_index = (size_t)(SendMessageW(GetDlgItem(dlg, ID_CMB_SCALING), CB_GETCURSEL, 0, 0));
      if (scaling_index < sizeof(scaling_algorithms) / sizeof(scaling_algorithms[0])) {
        err = config_set_scaling(pr->config, scaling_algorithms[scaling_index].id);
        if (efailed(err)) {
          err = ethru(err);
          goto cleanup;
        }
      }
      err = config_set_use_audio_index(pr->config, get_check(dlg, ID_CHK_USE_AUDIO_INDEX));
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

INPUT_PLUGIN_TABLE *get_input_plugin_table(void) {
  static INPUT_PLUGIN_TABLE table = {
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
  return &table;
}

#undef VIDEO_EXTS
