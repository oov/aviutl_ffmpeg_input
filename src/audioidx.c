#include "audioidx.h"

#include "ffmpeg.h"

#include "ovthreads.h"

#ifndef NDEBUG
#  include <ovprintf.h>
#  include <ovutil/win32.h>
#endif

#include <stdatomic.h>

struct item {
  int64_t key;
  int64_t pos;
};

enum indexer_state {
  is_stop,
  is_running,
};

struct audioidx {
  struct wstr filepath;
  struct hmap ptsmap;
  mtx_t mtx;
  cnd_t cnd;
  int64_t video_start_time;
  int64_t created_pts;
  thrd_t indexer;
  enum indexer_state indexer_state;
};

struct indexer_context {
  struct audioidx *ip;
  error err;
};

static int indexer(void *userdata) {
  struct indexer_context *ictx = userdata;
  struct audioidx *ip = ictx->ip;
  struct ffmpeg_stream fs = {0};
  error err = ffmpeg_open(&fs, ip->filepath.ptr, AVMEDIA_TYPE_AUDIO, NULL);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }

  int64_t const video_start_time = av_rescale_q(ip->video_start_time, AV_TIME_BASE_Q, fs.stream->time_base);

  mtx_lock(&ip->mtx);
  ictx->err = eok();
  ictx->ip = NULL;
  ictx = NULL;
  cnd_signal(&ip->cnd);
  mtx_unlock(&ip->mtx);

  int64_t samples = AV_NOPTS_VALUE;
  int progress = 0;
  enum indexer_state last_indexer_state = is_running;
  while (last_indexer_state == is_running) {
    int r = ffmpeg_read_packet(&fs);
    if (r < 0) {
      if (r == AVERROR_EOF) {
        break;
      }
      err = errffmpeg(r);
      goto cleanup;
    }
    if (samples == AV_NOPTS_VALUE) {
#ifndef NDEBUG
      char s[256];
      int64_t fstart_time = av_rescale_q(fs.fctx->start_time, AV_TIME_BASE_Q, fs.stream->time_base);
      ov_snprintf(s,
                  256,
                  "aidx start_time: pts: %lld / global: %lld / a: %lld / v: %lld",
                  fs.packet->pts,
                  fstart_time,
                  fs.stream->start_time,
                  video_start_time);
      OutputDebugStringA(s);
#endif
      samples =
          av_rescale_q(fs.packet->pts - video_start_time, fs.stream->time_base, av_make_q(1, fs.cctx->sample_rate));
    }
    mtx_lock(&ip->mtx);
    err = hmset(&ip->ptsmap, (&(struct item){.key = fs.packet->pts, .pos = samples}), NULL);
    ip->created_pts = fs.packet->pts;
    last_indexer_state = ip->indexer_state;
    cnd_signal(&ip->cnd);
    mtx_unlock(&ip->mtx);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
    int64_t const packet_samples =
        av_get_audio_frame_duration(fs.cctx, fs.packet->size ? fs.packet->size : fs.cctx->frame_size);
    if (!packet_samples) {
      err = errg(err_fail);
      goto cleanup;
    }
#ifndef NDEBUG
    if (fs.packet->pts < 1000) {
      char s[256];
      ov_snprintf(s, 256, "aidx: pts: %lld / samplepos: %lld", fs.packet->pts, packet_samples);
      OutputDebugStringA(s);
    }
#endif
    int const current_progress = (int)((10 * fs.packet->pts) / fs.stream->duration);
    if (current_progress != progress) {
#ifndef NDEBUG
      char s[256];
      ov_snprintf(s, 256, "aidx: %d%%", current_progress * 10);
      OutputDebugStringA(s);
#endif
      progress = current_progress;
    }
    samples += packet_samples;
  }
cleanup:
  ffmpeg_close(&fs);
  mtx_lock(&ip->mtx);
  ip->created_pts = INT64_MAX;
  if (ictx) {
    ictx->err = err;
    ictx->ip = NULL;
    ictx = NULL;
    err = eok();
  }
  cnd_signal(&ip->cnd);
  mtx_unlock(&ip->mtx);
  ereport(err);
  return 0;
}

NODISCARD error audioidx_create(struct audioidx **const ipp,
                                wchar_t const *const filepath,
                                int64_t const video_start_time) {
  if (!ipp || *ipp || !filepath) {
    return errg(err_invalid_arugment);
  }
  error err = mem(ipp, 1, sizeof(struct audioidx));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  struct audioidx *ip = *ipp;
  *ip = (struct audioidx){
      .video_start_time = video_start_time,
      .indexer_state = is_stop,
  };
  mtx_init(&ip->mtx, mtx_plain | mtx_recursive);
  cnd_init(&ip->cnd);
  ip->created_pts = AV_NOPTS_VALUE;
  err = hmnews(&ip->ptsmap, sizeof(struct item), 128, sizeof(int64_t));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  err = scpy(&ip->filepath, filepath);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
cleanup:
  if (efailed(err)) {
    if (*ipp) {
      audioidx_destroy(ipp);
    }
  }
  return err;
}

void audioidx_destroy(struct audioidx **const ipp) {
  if (!ipp || !*ipp) {
    return;
  }
  struct audioidx *ip = *ipp;
  mtx_lock(&ip->mtx);
  bool const already_running = ip->indexer_state == is_running;
  ip->indexer_state = is_stop;
  mtx_unlock(&ip->mtx);
  if (already_running) {
    thrd_join(ip->indexer, NULL);
  }
  ereport(hmfree(&ip->ptsmap));
  ereport(sfree(&ip->filepath));
  cnd_destroy(&ip->cnd);
  mtx_destroy(&ip->mtx);
  ereport(mem_free(ipp));
}

static NODISCARD error start_thread(struct audioidx *const ip) {
  struct indexer_context ictx = {
      .ip = ip,
      .err = eok(),
  };
  ip->indexer_state = is_running;
  if (thrd_create(&ip->indexer, indexer, &ictx) != thrd_success) {
    ip->indexer_state = is_stop;
    goto cleanup;
  }
  while (ictx.ip) {
    cnd_wait(&ip->cnd, &ip->mtx);
  }
cleanup:
  return ictx.err;
}

int64_t audioidx_get(struct audioidx *const ip, int64_t const pts) {
  error err = eok();
  int64_t pos = -1;
  mtx_lock(&ip->mtx);
  if (ip->indexer_state != is_running) {
    err = start_thread(ip);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
  }
  while (ip->created_pts < pts) {
    cnd_wait(&ip->cnd, &ip->mtx);
  }
  struct item *p = NULL;
  err = hmget(&ip->ptsmap, (&(struct item){.key = pts}), &p);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  if (p) {
    pos = p->pos;
  }
cleanup:
  mtx_unlock(&ip->mtx);
  ereport(err);
  return pos;
}
