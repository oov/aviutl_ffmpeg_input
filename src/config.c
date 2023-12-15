#include "config.h"

#include "ovnum.h"
#include "ovutil/str.h"
#include "ovutil/win32.h"

struct config {
  struct str preferred_decoders;
  enum video_format_scaling_algorithm scaling;
  enum config_handle_manage_mode handle_manage_mode;
  enum audio_index_mode audio_index_mode;
  enum audio_sample_rate audio_sample_rate;
  int number_of_stream;
  bool need_postfix;
  bool audio_use_sox;
  bool audio_invert_phase;
  bool modified;
};

NODISCARD error config_create(struct config **cp) {
  if (!cp || *cp) {
    return errg(err_invalid_arugment);
  }
  error err = mem(cp, 1, sizeof(struct config));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  struct config *c = *cp;
  *c = (struct config){0};

cleanup:
  return err;
}

enum config_handle_manage_mode config_get_handle_manage_mode(struct config const *const c) {
  return c->handle_manage_mode;
}

int config_get_number_of_stream(struct config const *const c) { return c->number_of_stream; }

char const *config_get_preferred_decoders(struct config const *const c) {
  return c->preferred_decoders.ptr ? c->preferred_decoders.ptr : "";
}

enum video_format_scaling_algorithm config_get_scaling(struct config const *const c) { return c->scaling; }

bool config_get_need_postfix(struct config const *const c) { return c->need_postfix; }

enum audio_index_mode config_get_audio_index_mode(struct config const *const c) { return c->audio_index_mode; }

enum audio_sample_rate config_get_audio_sample_rate(struct config const *const c) { return c->audio_sample_rate; }

bool config_get_audio_use_sox(struct config const *const c) { return c->audio_use_sox; }

bool config_get_audio_invert_phase(struct config const *const c) { return c->audio_invert_phase; }

NODISCARD error config_set_handle_manage_mode(struct config *const c,
                                              enum config_handle_manage_mode handle_manage_mode) {
  if (!c) {
    return errg(err_invalid_arugment);
  }
  if (c->handle_manage_mode == handle_manage_mode) {
    return eok();
  }
  switch ((int)handle_manage_mode) {
  case chmm_normal:
  case chmm_cache:
  case chmm_pool:
    break;
  default:
    handle_manage_mode = chmm_normal;
    break;
  }
  c->handle_manage_mode = handle_manage_mode;
  c->modified = true;
  return eok();
}

NODISCARD error config_set_number_of_stream(struct config *const c, int number_of_stream) {
  if (!c) {
    return errg(err_invalid_arugment);
  }
  if (number_of_stream < 1) {
    number_of_stream = 1;
  } else if (number_of_stream > 16) {
    number_of_stream = 16;
  }
  if (c->number_of_stream == number_of_stream) {
    return eok();
  }
  c->number_of_stream = number_of_stream;
  c->modified = true;
  return eok();
}

NODISCARD error config_set_preferred_decoders(struct config *const c, char const *const preferred_decoders) {
  if (!c || !preferred_decoders) {
    return errg(err_invalid_arugment);
  }
  if (strcmp(config_get_preferred_decoders(c), preferred_decoders) == 0) {
    return eok();
  }
  error err = scpy(&c->preferred_decoders, preferred_decoders);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  c->modified = true;
cleanup:
  return err;
}

NODISCARD error config_set_need_postfix(struct config *const c, bool const need_postfix) {
  if (!c) {
    return errg(err_invalid_arugment);
  }
  if (c->need_postfix == !!need_postfix) {
    return eok();
  }
  c->need_postfix = !!need_postfix;
  c->modified = true;
  return eok();
}

NODISCARD error config_set_scaling(struct config *const c, enum video_format_scaling_algorithm scaling) {
  if (!c) {
    return errg(err_invalid_arugment);
  }
  if (c->scaling == scaling) {
    return eok();
  }
  switch ((int)scaling) {
  case video_format_scaling_algorithm_fast_bilinear:
  case video_format_scaling_algorithm_bilinear:
  case video_format_scaling_algorithm_bicubic:
  case video_format_scaling_algorithm_x:
  case video_format_scaling_algorithm_point:
  case video_format_scaling_algorithm_area:
  case video_format_scaling_algorithm_bicublin:
  case video_format_scaling_algorithm_gauss:
  case video_format_scaling_algorithm_sinc:
  case video_format_scaling_algorithm_lanczos:
  case video_format_scaling_algorithm_spline:
    break;
  default:
    scaling = video_format_scaling_algorithm_fast_bilinear;
    break;
  }
  c->scaling = scaling;
  c->modified = true;
  return eok();
}

NODISCARD error config_set_audio_index_mode(struct config *const c, enum audio_index_mode audio_index_mode) {
  if (!c) {
    return errg(err_invalid_arugment);
  }
  if (c->audio_index_mode == audio_index_mode) {
    return eok();
  }
  switch ((int)audio_index_mode) {
  case aim_noindex:
  case aim_relax:
  case aim_strict:
    break;
  default:
    audio_index_mode = aim_noindex;
    break;
  }
  c->audio_index_mode = audio_index_mode;
  c->modified = true;
  return eok();
}

NODISCARD error config_set_audio_sample_rate(struct config *const c, enum audio_sample_rate audio_sample_rate) {
  if (!c) {
    return errg(err_invalid_arugment);
  }
  if (c->audio_sample_rate == audio_sample_rate) {
    return eok();
  }
  switch ((int)audio_sample_rate) {
  case asr_original:
  case asr_8000:
  case asr_11025:
  case asr_12000:
  case asr_16000:
  case asr_22050:
  case asr_24000:
  case asr_32000:
  case asr_44100:
  case asr_48000:
  case asr_64000:
  case asr_88200:
  case asr_96000:
  case asr_128000:
  case asr_176400:
  case asr_192000:
  case asr_256000:
    break;
  default:
    audio_sample_rate = asr_original;
    break;
  }
  c->audio_sample_rate = audio_sample_rate;
  c->modified = true;
  return eok();
}

NODISCARD error config_set_audio_use_sox(struct config *const c, bool const use_sox) {
  if (!c) {
    return errg(err_invalid_arugment);
  }
  if (c->audio_use_sox == !!use_sox) {
    return eok();
  }
  c->audio_use_sox = !!use_sox;
  c->modified = true;
  return eok();
}

NODISCARD error config_set_audio_invert_phase(struct config *const c, bool const invert_phase) {
  if (!c) {
    return errg(err_invalid_arugment);
  }
  if (c->audio_invert_phase == !!invert_phase) {
    return eok();
  }
  c->audio_invert_phase = !!invert_phase;
  c->modified = true;
  return eok();
}

static NODISCARD error get_config_filename(struct str *const dest) {
  struct wstr ws = {0};
  error err = get_module_file_name(get_hinstance(), &ws);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  size_t fnpos = 0;
  err = extract_file_name(&ws, &fnpos);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  ws.ptr[fnpos] = '\0';
  ws.len = fnpos;
  err = scat(&ws, L"ffmpeg_input.ini");
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  err = to_mbcs(&ws, dest);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
cleanup:
  ereport(sfree(&ws));
  return err;
}

static NODISCARD error load(struct config *c) {
  if (!c) {
    return errg(err_invalid_arugment);
  }
  struct str filepath = {0};
  error err = get_config_filename(&filepath);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  enum {
    buffer_size = 4096,
  };
  char buf[buffer_size];
  DWORD len;
  err = config_set_handle_manage_mode(c,
                                      (enum config_handle_manage_mode)(GetPrivateProfileIntA(
                                          "global", "handle_manage_mode", chmm_cache, filepath.ptr)));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  err = config_set_number_of_stream(c, (int)(GetPrivateProfileIntA("global", "number_of_stream", 2, filepath.ptr)));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  len = GetPrivateProfileStringA(
      "global",
      "preferred_decoders",
      "av1_cuvid,h264_cuvid,hevc_cuvid,mjpeg_cuvid,mpeg1_cuvid,mpeg2_cuvid,mpeg4_cuvid,vc1_cuvid,vp8_cuvid,vp9_cuvid,"
      "av1_qsv,h264_qsv,hevc_qsv,mjpeg_qsv,mpeg2_qsv,vc1_qsv,vp8_qsv,vp9_qsv,"
      "libopenh264",
      buf,
      buffer_size,
      filepath.ptr);
  buf[len] = '\0';
  err = config_set_preferred_decoders(c, buf);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  err = config_set_need_postfix(c, GetPrivateProfileIntA("global", "need_postfix", 0, filepath.ptr) != 0);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  err = config_set_scaling(c,
                           (enum video_format_scaling_algorithm)(GetPrivateProfileIntA(
                               "video", "scaling", video_format_scaling_algorithm_fast_bilinear, filepath.ptr)));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  err = config_set_audio_index_mode(
      c, (enum audio_index_mode)(GetPrivateProfileIntA("audio", "audio_index_mode", 0, filepath.ptr)));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  err = config_set_audio_sample_rate(
      c, (enum audio_sample_rate)(GetPrivateProfileIntA("audio", "audio_sample_rate", 0, filepath.ptr)));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  err = config_set_audio_use_sox(c, GetPrivateProfileIntA("audio", "audio_use_sox", 0, filepath.ptr) != 0);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  err = config_set_audio_invert_phase(c, GetPrivateProfileIntA("audio", "invert_phase", 0, filepath.ptr) != 0);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
cleanup:
  ereport(sfree(&filepath));
  return err;
}

NODISCARD error config_load(struct config *c) {
  if (!c) {
    return errg(err_invalid_arugment);
  }
  struct config *tmp = NULL;
  error err = config_create(&tmp);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  err = load(tmp);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  ereport(sfree(&c->preferred_decoders));
  c->handle_manage_mode = tmp->handle_manage_mode;
  c->number_of_stream = tmp->number_of_stream;
  c->preferred_decoders = tmp->preferred_decoders;
  tmp->preferred_decoders = (struct str){0};
  c->need_postfix = tmp->need_postfix;
  c->scaling = tmp->scaling;
  c->audio_index_mode = tmp->audio_index_mode;
  c->audio_sample_rate = tmp->audio_sample_rate;
  c->audio_use_sox = tmp->audio_use_sox;
  c->audio_invert_phase = tmp->audio_invert_phase;
  c->modified = false;
cleanup:
  config_destroy(&tmp);
  return err;
}

NODISCARD error config_save(struct config *c) {
  if (!c) {
    return errg(err_invalid_arugment);
  }
  if (!c->modified) {
    return eok();
  }
  struct str filepath = {0};
  char buf[32];
  error err = get_config_filename(&filepath);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  if (!WritePrivateProfileStringA(
          "global", "handle_manage_mode", ov_itoa((int64_t)(config_get_handle_manage_mode(c)), buf), filepath.ptr)) {
    err = errhr(HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (!WritePrivateProfileStringA(
          "global", "number_of_stream", ov_itoa((int64_t)(config_get_number_of_stream(c)), buf), filepath.ptr)) {
    err = errhr(HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (!WritePrivateProfileStringA("global", "preferred_decoders", config_get_preferred_decoders(c), filepath.ptr)) {
    err = errhr(HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (!WritePrivateProfileStringA("global", "need_postfix", config_get_need_postfix(c) ? "1" : "0", filepath.ptr)) {
    err = errhr(HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (!WritePrivateProfileStringA("video", "scaling", ov_itoa((int64_t)(config_get_scaling(c)), buf), filepath.ptr)) {
    err = errhr(HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (!WritePrivateProfileStringA(
          "audio", "audio_index_mode", ov_itoa((int64_t)(config_get_audio_index_mode(c)), buf), filepath.ptr)) {
    err = errhr(HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (!WritePrivateProfileStringA(
          "audio", "audio_sample_rate", ov_itoa((int64_t)(config_get_audio_sample_rate(c)), buf), filepath.ptr)) {
    err = errhr(HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (!WritePrivateProfileStringA("audio", "audio_use_sox", config_get_audio_use_sox(c) ? "1" : "0", filepath.ptr)) {
    err = errhr(HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (!WritePrivateProfileStringA(
          "audio", "invert_phase", config_get_audio_invert_phase(c) ? "1" : "0", filepath.ptr)) {
    err = errhr(HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
cleanup:
  ereport(sfree(&filepath));
  return err;
}

void config_destroy(struct config **cp) {
  if (!cp || !*cp) {
    return;
  }
  struct config *c = *cp;
  ereport(sfree(&c->preferred_decoders));
  ereport(mem_free(cp));
}
