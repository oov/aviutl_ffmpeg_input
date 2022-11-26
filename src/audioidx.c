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

struct audioidx {
  struct hmap ptsmap;
  mtx_t mtx;
  thrd_t indexer;
  atomic_bool exiting;
};

struct indexer_context {
  struct audioidx *ip;
  struct cndvar cv;
  error err;
  wchar_t const *filepath;
  int64_t video_start_time;
};

static int indexer(void *userdata) {
  struct indexer_context *ictx = userdata;
  struct audioidx *ip = ictx->ip;
  struct ffmpeg_stream fs = {0};
  error err = ffmpeg_open(&fs, ictx->filepath, AVMEDIA_TYPE_AUDIO, NULL);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }

  int64_t const video_start_time = av_rescale_q(ictx->video_start_time, AV_TIME_BASE_Q, fs.stream->time_base);

  ictx->err = eok();
  cndvar_lock(&ictx->cv);
  cndvar_signal(&ictx->cv, 1);
  cndvar_unlock(&ictx->cv);
  ictx = NULL;

  int64_t samples = AV_NOPTS_VALUE;
  while (!atomic_load(&ip->exiting)) {
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
    if (fs.packet->pts < 10000) {
      char s[256];
      ov_snprintf(s, 256, "aidx: pts: %lld / samplepos: %lld", fs.packet->pts, packet_samples);
      OutputDebugStringA(s);
    }
#endif
    samples += packet_samples;
  }
cleanup:
  ffmpeg_close(&fs);
  if (ictx) {
    ictx->err = err;
    cndvar_lock(&ictx->cv);
    cndvar_signal(&ictx->cv, 1);
    cndvar_unlock(&ictx->cv);
  } else {
    ereport(err);
  }
  return 0;
}

NODISCARD error audioidx_create(struct audioidx **const ipp,
                                wchar_t const *const filepath,
                                int64_t const video_start_time) {
  if (!ipp || *ipp || !filepath) {
    return errg(err_invalid_arugment);
  }
  struct indexer_context ictx = {
      .filepath = filepath,
      .video_start_time = video_start_time,
  };
  cndvar_init(&ictx.cv);
  cndvar_lock(&ictx.cv);
  ictx.cv.var = 0;
  error err = mem(ipp, 1, sizeof(struct audioidx));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  struct audioidx *ip = *ipp;
  *ip = (struct audioidx){0};
  mtx_init(&ip->mtx, mtx_plain | mtx_recursive);
  err = hmnews(&ip->ptsmap, sizeof(struct item), 128, sizeof(int64_t));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  ictx.ip = ip;
  if (thrd_create(&ip->indexer, indexer, &ictx) != thrd_success) {
    err = errg(err_fail);
    goto cleanup;
  }
  cndvar_wait_while(&ictx.cv, 0);
cleanup:
  cndvar_unlock(&ictx.cv);
  cndvar_exit(&ictx.cv);
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
  atomic_store(&ip->exiting, true);
  thrd_join(ip->indexer, NULL);
  ereport(hmfree(&ip->ptsmap));
  mtx_destroy(&ip->mtx);
  ereport(mem_free(ipp));
}

int64_t audioidx_get(struct audioidx *const ip, int64_t const pts) {
  int64_t pos = -1;
  {
    mtx_lock(&ip->mtx);
    struct item *p = NULL;
    error err = hmget(&ip->ptsmap, (&(struct item){.key = pts}), &p);
    if (efailed(err)) {
      efree(&err);
    } else if (p) {
      pos = p->pos;
    }
    mtx_unlock(&ip->mtx);
  }
  return pos;
}
