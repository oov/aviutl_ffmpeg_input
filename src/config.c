#include "config.h"

#include "ovnum.h"
#include "ovutil/str.h"
#include "ovutil/win32.h"

struct config {
  bool modified;
  struct str preferred_decoders;
  enum video_format_scaling_algorithm scaling;
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

char const *config_get_preferred_decoders(struct config const *const c) {
  return c->preferred_decoders.ptr ? c->preferred_decoders.ptr : "";
}

enum video_format_scaling_algorithm config_get_scaling(struct config const *const c) { return c->scaling; }

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
  DWORD len = GetPrivateProfileStringA("global", "preferred_decoders", "", buf, buffer_size, filepath.ptr);
  buf[len] = '\0';
  err = config_set_preferred_decoders(c, buf);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }

  int scaling = (int)GetPrivateProfileIntA("video", "scaling", 0, filepath.ptr);
  err = config_set_scaling(c, (enum video_format_scaling_algorithm)scaling);
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
  c->preferred_decoders = tmp->preferred_decoders;
  tmp->preferred_decoders = (struct str){0};
  c->scaling = tmp->scaling;
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
  error err = get_config_filename(&filepath);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  if (!WritePrivateProfileStringA("global", "preferred_decoders", config_get_preferred_decoders(c), filepath.ptr)) {
    err = errhr(HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  char buf[32];
  if (!WritePrivateProfileStringA("video", "scaling", ov_itoa((int64_t)(config_get_scaling(c)), buf), filepath.ptr)) {
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
