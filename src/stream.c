#include "stream.h"

#include "ovutil/str.h"
#include "ovutil/win32.h"

#include "audio.h"
#include "config.h"
#include "video.h"

struct stream {
  HANDLE file;
  struct config *config;
  struct video *v;
  struct audio *a;
  int64_t video_start_time;
  struct info_video vi;
  struct info_audio ai;
};

static NODISCARD bool has_postfix(wchar_t const *const filepath) {
  if (!filepath) {
    return false;
  }
  bool r = false;
  struct wstr ws = {0};
  error err = scpy(&ws, filepath);
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

static NODISCARD error create_video(struct stream *sp, struct video **v) {
  error err = video_create(v,
                           &(struct video_options){
                               .handle = sp->file,
                               .preferred_decoders = config_get_preferred_decoders(sp->config),
                               .scaling = config_get_scaling(sp->config),
                           });
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
cleanup:
  return err;
}

static NODISCARD error create_audio(struct stream *sp, struct audio **a) {
  error err = audio_create(a,
                           &(struct audio_options){
                               .handle = sp->file,
                               .preferred_decoders = config_get_preferred_decoders(sp->config),
                               .video_start_time = sp->video_start_time,
                               .use_audio_index = config_get_use_audio_index(sp->config),
                           });
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
cleanup:
  return err;
}

NODISCARD error stream_create(struct stream **const spp, wchar_t const *const filepath) {
  if (!spp || *spp || !filepath) {
    return errg(err_invalid_arugment);
  }
  error err = eok();
  HANDLE file = INVALID_HANDLE_VALUE;
  struct config *config = NULL;
  struct stream *sp = NULL;
  struct video *v = NULL;
  struct audio *a = NULL;
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
  file = CreateFileW(filepath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE) {
    err = errhr(HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  err = mem(&sp, 1, sizeof(struct stream));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  *sp = (struct stream){
      .file = file,
      .config = config,
  };
  err = create_video(sp, &v);
  if (efailed(err)) {
    ereport(err);
  } else {
    video_get_info(v, &sp->vi);
    sp->video_start_time = video_get_start_time(v);
  }
  err = create_audio(sp, &a);
  if (efailed(err)) {
    ereport(err);
  } else {
    audio_get_info(a, &sp->ai);
  }
  if (!v && !a) {
    err = errg(err_fail);
    goto cleanup;
  }
  *spp = sp;
cleanup:
  // In AviUtl's extended editing, video and audio are read as separate objects.
  // As a result, two video and two audio streams are retained.
  // To avoid this, I release both streams at this time and reopen them when a read request comes in.
  audio_destroy(&a);
  video_destroy(&v);
  if (efailed(err)) {
    if (sp) {
      ereport(mem_free(&sp));
    }
    if (file != INVALID_HANDLE_VALUE) {
      CloseHandle(file);
      file = INVALID_HANDLE_VALUE;
    }
    if (config) {
      config_destroy(&config);
    }
  }
  return err;
}

void stream_destroy(struct stream **const spp) {
  if (!spp || !*spp) {
    return;
  }
  struct stream *sp = *spp;
  video_destroy(&sp->v);
  audio_destroy(&sp->a);
  if (sp->file != INVALID_HANDLE_VALUE) {
    CloseHandle(sp->file);
    sp->file = INVALID_HANDLE_VALUE;
  }
  config_destroy(&sp->config);
  ereport(mem_free(&sp));
}

struct info_video const *stream_get_video_info(struct stream const *const sp) { return &sp->vi; }

struct info_audio const *stream_get_audio_info(struct stream const *const sp) { return &sp->ai; }

NODISCARD error stream_read_video(struct stream *const sp,
                                  int64_t const frame,
                                  void *const buf,
                                  size_t *const written) {
  if (!sp || !buf) {
    return errg(err_invalid_arugment);
  }
  error err = eok();
  if (!sp->v) {
    err = create_video(sp, &sp->v);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
  }
  size_t wr = 0;
  err = video_read(sp->v, frame, buf, &wr);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  if (written) {
    *written = wr;
  }
cleanup:
  return err;
}

NODISCARD error stream_read_audio(
    struct stream *const sp, int64_t const start, size_t const length, void *const buf, int *const written) {
  if (!sp || !buf || !length) {
    return errg(err_invalid_arugment);
  }
  error err = eok();
  if (!sp->a) {
    err = create_audio(sp, &sp->a);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
  }
  int wr = 0;
  err = audio_read(sp->a, start, (int)length, buf, &wr);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  if (config_get_invert_phase(sp->config)) {
    int16_t *w = buf;
    for (int i = 0; i < wr; ++i) {
      *w = -*w;
      w++;
      *w = -*w;
      w++;
    }
  }
  if (written) {
    *written = wr;
  }
cleanup:
  return err;
}
