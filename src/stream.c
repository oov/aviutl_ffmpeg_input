#include "stream.h"

#include "ovthreads.h"
#include "ovutil/str.h"
#include "ovutil/win32.h"

#include "audio.h"
#include "config.h"
#include "video.h"

struct stream {
  HANDLE file;
  struct config const *config;
  struct video *v;
  struct audio *a;
  int64_t video_start_time;
  struct info_video vi;
  struct info_audio ai;
};

struct fileid {
  uint64_t volume;
  uint64_t id;
};

static bool isold(struct timespec const *const a, struct timespec const *const b) {
  if (a->tv_sec == b->tv_sec) {
    return a->tv_nsec > b->tv_nsec;
  }
  return a->tv_sec > b->tv_sec;
}

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

static NODISCARD error get_fileid(HANDLE const file, struct fileid *const fid) {
  if (!file || file == INVALID_HANDLE_VALUE || !fid) {
    return errg(err_invalid_arugment);
  }
  error err = eok();
  BY_HANDLE_FILE_INFORMATION hfi = {0};
  if (!GetFileInformationByHandle(file, &hfi)) {
    err = errhr(HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  *fid = (struct fileid){
      .volume = (uint64_t)hfi.dwVolumeSerialNumber,
      .id = (ULARGE_INTEGER){.LowPart = hfi.nFileIndexLow, .HighPart = hfi.nFileIndexHigh}.QuadPart,
  };
cleanup:
  return err;
}

static NODISCARD error get_fileid_from_filepath(wchar_t const *const filepath, struct fileid *const fid) {
  error err = eok();
  HANDLE const h =
      CreateFileW(filepath, 0, FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
  if (h == INVALID_HANDLE_VALUE) {
    err = errhr(HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  err = get_fileid(h, fid);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
cleanup:
  if (h != INVALID_HANDLE_VALUE) {
    CloseHandle(h);
  }
  return err;
}

static bool inline is_same_fileid(struct fileid const *const a, struct fileid const *const b) {
  return a->id == b->id && a->volume == b->volume;
}

static NODISCARD error stream_create(struct stream **const spp,
                                     struct config const *const config,
                                     wchar_t const *const filepath) {
  if (!spp || *spp || !filepath) {
    return errg(err_invalid_arugment);
  }
  error err = eok();
  HANDLE file = INVALID_HANDLE_VALUE;
  struct stream *sp = NULL;
  struct video *v = NULL;
  struct audio *a = NULL;
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
  }
  return err;
}

static void stream_destroy(struct stream **const spp) {
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
  ereport(mem_free(&sp));
}

static struct info_video const *stream_get_video_info(struct stream const *const sp) { return &sp->vi; }

static struct info_audio const *stream_get_audio_info(struct stream const *const sp) { return &sp->ai; }

static NODISCARD error stream_read_video(struct stream *const sp,
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

static NODISCARD error stream_read_audio(
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

struct freeitem {
  struct fileid fid;
  struct timespec used_at;
  struct stream *stream;
};

struct streamitem {
  intptr_t key;
  struct stream *stream;
};

struct streammap {
  struct config *config;
  struct hmap map;
  int key_index;
  struct freeitem *freelist;
  size_t freelist_length;
};

NODISCARD error streammap_create(struct streammap **smpp, size_t const keep_length) {
  struct streammap *smp = NULL;
  error err = mem(&smp, 1, sizeof(struct streammap));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  *smp = (struct streammap){
      .freelist_length = keep_length,
  };
  err = hmnews(&smp->map, sizeof(struct streamitem), 4, sizeof(intptr_t));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  err = mem(&smp->freelist, keep_length, sizeof(struct freeitem));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  memset(smp->freelist, 0, sizeof(struct freeitem) * keep_length);
  err = config_create(&smp->config);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  err = config_load(smp->config);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  *smpp = smp;
#ifndef NDEBUG
  OutputDebugStringA("streammap created");
#endif
cleanup:
  if (efailed(err)) {
    streammap_destroy(&smp);
  }
  return err;
}

static bool cleaner(void const *const item, void *const udata) {
  (void)udata;
  struct freeitem *fi = ov_deconster_(item);
  stream_destroy(&fi->stream);
  return true;
}

void streammap_destroy(struct streammap **const smpp) {
  if (!smpp || !*smpp) {
    return;
  }
  struct streammap *const smp = *smpp;
  if (smp->freelist) {
    for (size_t i = 0; i < smp->freelist_length; ++i) {
      stream_destroy(&smp->freelist[i].stream);
    }
  }
  ereport(mem_free(&smp->freelist));
  ereport(hmscan(&smp->map, cleaner, NULL));
  ereport(hmfree(&smp->map));
  config_destroy(&smp->config);
  ereport(mem_free(smpp));
#ifndef NDEBUG
  OutputDebugStringA("streammap destroyed");
#endif
}

static NODISCARD error find_from_freelist(struct streammap *const smp,
                                          wchar_t const *const filepath,
                                          struct stream **sp) {
  struct fileid fid = {0};
  error err = get_fileid_from_filepath(filepath, &fid);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  struct freeitem *found = NULL;
  for (size_t i = 0; i < smp->freelist_length; ++i) {
    if (is_same_fileid(&fid, &smp->freelist[i].fid)) {
      if (!found || isold(&found->used_at, &smp->freelist[i].used_at)) {
        found = smp->freelist + i;
      }
    }
  }
  if (!found) {
    goto cleanup;
  }
  *sp = found->stream;
  *found = (struct freeitem){0};
cleanup:
  return err;
}

NODISCARD error streammap_create_stream(struct streammap *const smp,
                                        wchar_t const *const filepath,
                                        intptr_t *const idx) {
  if (!smp || !filepath || !idx) {
    return errg(err_invalid_arugment);
  }
  struct stream *sp = NULL;
  error err = find_from_freelist(smp, filepath, &sp);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  if (!sp) {
    err = stream_create(&sp, smp->config, filepath);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
  }
  err = hmset(&smp->map,
              &((struct streamitem){
                  .key = ++smp->key_index,
                  .stream = sp,
              }),
              NULL);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  *idx = smp->key_index;
#ifndef NDEBUG
  {
    char s[256];
    wsprintfA(s, "key:%d created", smp->key_index);
    OutputDebugStringA(s);
  }
#endif
cleanup:
  return err;
}

static struct stream *get_stream(struct streammap *const smp, intptr_t const idx) {
  struct streamitem *si = NULL;
  error err = hmget(&smp->map, &((struct streamitem){.key = idx}), &si);
  if (efailed(err)) {
    efree(&err);
    return NULL;
  }
  if (!si) {
    return NULL;
  }
  return si->stream;
}

static NODISCARD error add_to_freelist(struct streammap *const smp, struct stream *sp) {
  struct freeitem fi = {
      .stream = sp,
  };
  if (timespec_get(&fi.used_at, TIME_UTC) != TIME_UTC) {
    return errg(err_invalid_arugment);
  }
  error err = get_fileid(sp->file, &fi.fid);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  struct freeitem *unused = NULL;
  struct freeitem *oldest = NULL;
  for (size_t i = 0; i < smp->freelist_length; ++i) {
    if (!oldest || isold(&oldest->used_at, &smp->freelist[i].used_at)) {
      oldest = smp->freelist + i;
    }
    if (!unused && !smp->freelist[i].stream) {
      unused = smp->freelist + i;
      break;
    }
  }
  if (!unused) {
    stream_destroy(&oldest->stream);
    unused = oldest;
  }
  *unused = fi;
cleanup:
  return err;
}

NODISCARD error streammap_free_stream(struct streammap *const smp, intptr_t const idx) {
  struct streamitem *si = NULL;
  error err = hmdelete(&smp->map, &((struct streamitem){.key = idx}), &si);
  if (efailed(err)) {
    if (eisg(err, err_not_found)) {
      efree(&err);
      return eok();
    }
    err = ethru(err);
    goto cleanup;
  }
  if (!si) {
    goto cleanup;
  }
  err = add_to_freelist(smp, si->stream);
  if (efailed(err)) {
    stream_destroy(&si->stream);
    err = ethru(err);
    goto cleanup;
  }
#ifndef NDEBUG
  {
    char s[256];
    wsprintfA(s, "key:%d moved to freelist", idx);
    OutputDebugStringA(s);
  }
#endif
cleanup:
  return err;
}

struct info_video const *streammap_get_video_info(struct streammap *const smp, intptr_t const idx) {
  struct stream *const sp = get_stream(smp, idx);
  if (!sp) {
    return NULL;
  }
  return stream_get_video_info(sp);
}

struct info_audio const *streammap_get_audio_info(struct streammap *const smp, intptr_t const idx) {
  struct stream *const sp = get_stream(smp, idx);
  if (!sp) {
    return NULL;
  }
  return stream_get_audio_info(sp);
}

NODISCARD error streammap_read_video(
    struct streammap *const smp, intptr_t const idx, int64_t const frame, void *const buf, size_t *const written) {
  struct stream *const sp = get_stream(smp, idx);
  if (!sp) {
    return errg(err_invalid_arugment);
  }
  return stream_read_video(sp, frame, buf, written);
}

NODISCARD error streammap_read_audio(struct streammap *const smp,
                                     intptr_t const idx,
                                     int64_t const start,
                                     size_t const length,
                                     void *const buf,
                                     int *const written) {
  struct stream *const sp = get_stream(smp, idx);
  if (!sp) {
    return errg(err_invalid_arugment);
  }
  return stream_read_audio(sp, start, length, buf, written);
}
