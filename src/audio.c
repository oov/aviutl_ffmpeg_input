#include "audio.h"

#include <ovprintf.h>
#include <ovthreads.h>
#include <ovutil/win32.h>

#include "audioidx.h"
#include "ffmpeg.h"
#include "now.h"
#include "resampler.h"

#define SHOWLOG_AUDIO_GET_INFO 0
#define SHOWLOG_AUDIO_REPORT_INDEX_ENTRIES 0
#define SHOWLOG_AUDIO_SEEK 0
#define SHOWLOG_AUDIO_SEEK_ADJUST 0
#define SHOWLOG_AUDIO_SEEK_SPEED 0
#define SHOWLOG_AUDIO_SEEK_FIND_STREAM 0
#define SHOWLOG_AUDIO_READ 0
#define SHOWLOG_AUDIO_GAP 0

// osr = original sample rate
// asr = active sample rate
// isr = internal sample rate (osr * factor_a OR asr * factor_b)
struct stream {
  struct ffmpeg_stream ffmpeg;
  struct timespec ts;
};

enum status {
  status_nothread,
  status_running,
  status_closing,
};

struct audio {
  struct stream *streams;
  size_t len;
  size_t cap;

  struct wstr filepath;
  void *handle;
  mtx_t mtx;
  thrd_t thread;
  enum status status;

  int out_sample_rate;
  struct audioidx *idx;
  enum audio_index_mode index_mode;
  enum audio_sample_rate sample_rate;
  bool use_sox;
  bool wait_index;
};

static inline int64_t get_start_time(struct stream const *const stream) {
  return stream->ffmpeg.stream->start_time == AV_NOPTS_VALUE ? 0 : stream->ffmpeg.stream->start_time;
}

static inline int64_t pts_to_sample_pos_osr(int64_t const pts, struct stream const *const stream) {
  return av_rescale_q(pts - get_start_time(stream),
                      stream->ffmpeg.cctx->pkt_timebase,
                      av_make_q(1, stream->ffmpeg.stream->codecpar->sample_rate));
}

static inline int64_t sample_pos_osr_to_pts(int64_t const sample, struct stream const *const stream) {
  return av_rescale_q(
             sample, av_make_q(1, stream->ffmpeg.stream->codecpar->sample_rate), stream->ffmpeg.cctx->pkt_timebase) +
         get_start_time(stream);
}

static int get_output_sample_rate(enum audio_sample_rate const sample_rate, int const original_sample_rate) {
  switch (sample_rate) {
  case asr_original:
    return original_sample_rate;
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
    return (int)sample_rate;
  }
  return original_sample_rate;
}

void audio_get_info(struct audio const *const a, struct info_audio *const ai) {
  ai->sample_rate = a->out_sample_rate;
  ai->channels = resampler_out_channels;
  ai->bit_depth = sizeof(sample_t) * 8;
  ai->samples = av_rescale_rnd(a->streams[0].ffmpeg.fctx->duration, a->out_sample_rate, AV_TIME_BASE, AV_ROUND_DOWN);
#if SHOWLOG_AUDIO_GET_INFO
  char s[256];
  ov_snprintf(s,
              256,
              NULL,
              "ainfo duration: %lld / samples: %lld / start_time: %lld",

              a->streams[0].ffmpeg.fctx->duration,
              ai->samples,
              a->streams[0].ffmpeg.stream->start_time);
  OutputDebugStringA(s);
#endif
}

static NODISCARD error grab_next(struct stream *const stream) {
  int const r = ffmpeg_grab(&stream->ffmpeg);
  if (r < 0) {
    return errffmpeg(r);
  }
#if SHOWLOG_AUDIO_READ
  {
    int64_t const p1 = stream->resampled_current_pos_isr + old_samples_isr;
    int64_t const p2 = pts_to_sample_pos_isr(stream->ffmpeg.packet->pts, stream);
    char s[256];
    ov_snprintf(s, 256, NULL, "a grab_next pos add %lld stored: %lld equal: %d", p1, p2, p1 == p2 ? 1 : 0);
    OutputDebugStringA(s);
  }
#endif
  return eok();
}

static NODISCARD error seek(struct audio *const a,
                            struct stream *stream,
                            int64_t const sample,
                            int64_t *sample_pos_osr) {
  (void)a;
#if SHOWLOG_AUDIO_REPORT_INDEX_ENTRIES
  {
    char s[256];
    ov_snprintf(s, 256, NULL, "a index entries: %d", avformat_index_get_entries_count(stream->ffmpeg.stream));
    OutputDebugStringA(s);
  }
#endif
#if SHOWLOG_AUDIO_SEEK_SPEED
  double const start = now();
#endif
  error err = eok();
  int64_t time_stamp = sample_pos_osr_to_pts(sample, stream);
  int64_t const duration1s = (int64_t)(av_q2d(av_inv_q(stream->ffmpeg.cctx->pkt_timebase)));
#if SHOWLOG_AUDIO_SEEK
  {
    char s[256];
    ov_snprintf(s,
                256,
                NULL,
                "req_pts:%lld sample: %lld tb: %f sr: %d",
                time_stamp,
                sample,
                av_q2d(av_inv_q(stream->ffmpeg.cctx->pkt_timebase)),
                stream->ffmpeg.stream->codecpar->sample_rate);
    OutputDebugStringA(s);
  }
#endif
  int64_t prevpts = AV_NOPTS_VALUE;
  for (;;) {
    err = ffmpeg_seek(&stream->ffmpeg, time_stamp);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
    int r = ffmpeg_grab(&stream->ffmpeg);
    if (r < 0) {
      err = errffmpeg(r);
      goto cleanup;
    }
    int64_t const pos_osr = pts_to_sample_pos_osr(stream->ffmpeg.packet->pts, stream);
    if (pos_osr > sample) {
#if SHOWLOG_AUDIO_SEEK_ADJUST
      {
        char s[256];
        ov_snprintf(s,
                    256,
                    NULL,
                    "a adjust target: %lld current: %lld rewind: %f",
                    sample,
                    stream->resampled_current_pos_isr,
                    av_q2d(av_inv_q(stream->ffmpeg.cctx->pkt_timebase)));
        OutputDebugStringA(s);
      }
#endif
      if (time_stamp < stream->ffmpeg.stream->start_time + duration1s && prevpts == stream->ffmpeg.packet->pts) {
        // It seems that the pts value is not updated.
        // If seeking to the first frame fails, seeking to 0 by byte may succeed.
        err = ffmpeg_seek_head(&stream->ffmpeg);
        if (efailed(err)) {
          err = ethru(err);
          goto cleanup;
        }
        r = ffmpeg_grab(&stream->ffmpeg);
        if (r < 0) {
          err = errffmpeg(r);
          goto cleanup;
        }
        break;
      } else {
        time_stamp -= duration1s;
      }
      prevpts = stream->ffmpeg.packet->pts;
      continue;
    }
    break;
  }
#if 0
  if (stream->resampled_current_pos_isr < sample) {
    // https://ffmpeg.org/doxygen/6.0/group__lavc__packet.html#gga9a80bfcacc586b483a973272800edb97a2093332d8086d25a04942ede61007f6a
    // below code is depend on structure packing, so it's not portable.
    // struct data_skip_samples {
    //   uint32_t start; // number of samples to skip from start of this packet
    //   uint32_t end; // number of samples to skip from end of this packet
    //   uint8_t reason_start; // reason for start skip
    //   uint8_t reason_end; // reason for end skip (0=padding silence, 1=convergence)
    // } __attribute__((packed));
    uint8_t *data = av_packet_get_side_data(stream->ffmpeg.packet, AV_PKT_DATA_SKIP_SAMPLES, NULL);
    if (!data) {
      data = av_packet_new_side_data(stream->ffmpeg.packet, AV_PKT_DATA_SKIP_SAMPLES, 10);
    }
    if (data) {
      *(uint32_t *)((void *)data) = (uint32_t)(sample - stream->resampled_current_pos_isr);
      memset(data + 4, 0, 6);
    }
    int r = avcodec_send_packet(stream->ffmpeg.cctx, stream->ffmpeg.packet);
    if (r < 0) {
      err = errffmpeg(r);
      err = ethru(err);
      goto cleanup;
    }
  }
#endif
  int64_t pos_osr = pts_to_sample_pos_osr(stream->ffmpeg.packet->pts, stream);
  while (pos_osr + stream->ffmpeg.frame->nb_samples <= sample) {
#if SHOWLOG_AUDIO_SEEK
    {
      char s[256];
      ov_snprintf(s,
                  256,
                  NULL,
                  "csp: %lld / smp: %lld",
                  stream->resampled_current_pos_isr + stream->ffmpeg.frame->nb_samples,
                  sample);
      OutputDebugStringA(s);
    }
#endif
    pos_osr += stream->ffmpeg.frame->nb_samples;
    // It seems we should not use ffmpeg_grab_discard here.
    // In some cases, the processing speed may drop significantly.
    int const r = ffmpeg_grab(&stream->ffmpeg);
    if (r < 0) {
      err = errffmpeg(r);
      goto cleanup;
    }
#if SHOWLOG_AUDIO_GAP
    {
      int64_t const pts_pos = pts_to_sample_pos_osr(stream->ffmpeg.packet->pts, stream);
      if (pos_osr != pts_pos) {
        char s[256];
        ov_snprintf(s, 256, NULL, "pos gap: %lld %lld", pos_osr, pts_pos);
        OutputDebugStringA(s);
      }
    }
#endif
  }
  *sample_pos_osr = pos_osr;
cleanup:
#if SHOWLOG_AUDIO_SEEK_SPEED
{
  double const end = now();
  char s[256];
  ov_snprintf(s, 256, NULL, "a seek: %0.4fs", end - start);
  OutputDebugStringA(s);
}
#endif
  return err;
}

static inline int imin(int const a, int const b) { return a > b ? b : a; }

static NODISCARD error stream_read(struct audio *const a,
                                   struct resampler *const resampler,
                                   struct stream *const stream,
                                   int64_t const offset_asr,
                                   int const length,
                                   void *const buf,
                                   int *const written) {
#if SHOWLOG_AUDIO_READ
  {
    char s[256];
    ov_snprintf(s,
                256,
                NULL,
                "audio_read ofs: %lld / len: %d / cur: %lld / swr: %lld",
                offset_isr,
                length_isr,
                stream->resampled_current_pos_isr / stream->gcd.factor_b,
                stream->swr_buf_sample_pos_isr / stream->gcd.factor_b);
    OutputDebugStringA(s);
  }
#endif
  error err = eok();
  uint8_t *dest = buf;
  int read = 0;
  int r = 0;

start:
  // Finished?
  if (read == length) {
    goto cleanup;
  }

  // Is there any part that can be used within the resampled buffer?
  int64_t const readpos_asr = offset_asr + read;
  int64_t const resampled_pos_asr = resampler->pos_isr / resampler->gcd.factor_b;
  if (readpos_asr >= resampled_pos_asr && readpos_asr < resampled_pos_asr + resampler->written) {
    goto readbuf;
  }

  // Is there any part that can be used within the current frame?
  int64_t const frame_pos_isr = pts_to_sample_pos_osr(stream->ffmpeg.packet->pts, stream) * resampler->gcd.factor_b;
  int64_t const frame_pos_asr = frame_pos_isr / resampler->gcd.factor_a;
  int64_t const frame_end_pos_asr =
      (frame_pos_isr + stream->ffmpeg.frame->nb_samples * resampler->gcd.factor_b) / resampler->gcd.factor_a;
  if (readpos_asr >= frame_pos_asr && readpos_asr < frame_end_pos_asr) {
    goto convert;
  }

  // There seems to be no data available in the current frame.

  // Assuming that the next frame has the same number of samples, is there data available in the next frame?
  int64_t const next_frame_end_pos_asr =
      (frame_pos_isr + stream->ffmpeg.frame->nb_samples * 2 * resampler->gcd.factor_b) / resampler->gcd.factor_a;
  if (readpos_asr >= frame_end_pos_asr && readpos_asr < next_frame_end_pos_asr) {
    goto grab_next;
  }

  // I've tried everything, so I'll just seek normally.
  goto seek;

readbuf:
#if SHOWLOG_AUDIO_READ
  OutputDebugStringA(__FILE_NAME__ " readbuf");
#endif
  {
    int const buffer_offset_asr = (int)(readpos_asr - resampled_pos_asr);
    int const samples_asr = imin(length - read, resampler->written - buffer_offset_asr);
    memcpy(dest + read * resampler_out_sample_size,
           resampler->buf + buffer_offset_asr * resampler_out_sample_size,
           (size_t)(samples_asr * resampler_out_sample_size));
    read += samples_asr;
  }
  goto start;

grab_next:
#if SHOWLOG_AUDIO_READ
  OutputDebugStringA(__FILE_NAME__ " grab / convert");
#endif
  err = grab_next(stream);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  r = swr_convert(resampler->ctx,
                  (void *)&resampler->buf,
                  resampler->samples,
                  (void *)stream->ffmpeg.frame->data,
                  stream->ffmpeg.frame->nb_samples);
  if (r < 0) {
    err = errffmpeg(r);
    goto cleanup;
  }
  // If you recalculate, the result will be shifted due to the effect of the sample rate conversion,
  resampler->pos_isr += resampler->written * resampler->gcd.factor_b;
  resampler->written = r;
#if SHOWLOG_AUDIO_GAP
  {
    int64_t const pts_pos =
        pts_to_sample_pos_osr(stream->ffmpeg.packet->pts, stream) * resampler->gcd.factor_b / resampler->gcd.factor_a;
    if (resampler->pos_isr / resampler->gcd.factor_b != pts_pos) {
      char s[256];
      ov_snprintf(s, 256, NULL, "pos gap: %lld %lld", resampler->pos_isr / resampler->gcd.factor_b, pts_pos);
      OutputDebugStringA(s);
    }
  }
#endif
  goto start;

seek:
#if SHOWLOG_AUDIO_READ
  OutputDebugStringA(__FILE_NAME__ " seek");
#endif
  {
    int64_t pos_osr;
    err = seek(a, stream, (offset_asr + read) * resampler->gcd.factor_a / resampler->gcd.factor_b, &pos_osr);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
    r = swr_convert(resampler->ctx,
                    (void *)&resampler->buf,
                    resampler->samples,
                    (void *)stream->ffmpeg.frame->data,
                    stream->ffmpeg.frame->nb_samples);
    if (r < 0) {
      err = errffmpeg(r);
      goto cleanup;
    }
    resampler->pos_isr = pos_osr * resampler->gcd.factor_b * resampler->gcd.factor_b / resampler->gcd.factor_a;
    resampler->written = r;
  }
  goto start;

convert:
  r = swr_convert(resampler->ctx,
                  (void *)&resampler->buf,
                  resampler->samples,
                  (void *)stream->ffmpeg.frame->data,
                  stream->ffmpeg.frame->nb_samples);
  if (r < 0) {
    err = errffmpeg(r);
    goto cleanup;
  }
  resampler->pos_isr = pts_to_sample_pos_osr(stream->ffmpeg.packet->pts, stream) * resampler->gcd.factor_b *
                       resampler->gcd.factor_b / resampler->gcd.factor_a;
  resampler->written = r;
  goto start;

cleanup:
#if SHOWLOG_AUDIO_READ
{
  char s[256];
  ov_snprintf(s, 256, NULL, "a read done %d", read_isr);
  OutputDebugStringA(s);
}
#endif
  if (efailed(err)) {
    if (eis_errno(err, AVERROR_EOF)) {
      efree(&err);
      if (read < length) {
        memset(dest + (read * resampler_out_sample_size), 0, (size_t)((length - read) * resampler_out_sample_size));
        read = length;
      }
    }
  }
  *written = read;
  return err;
}

static int create_sub_stream(void *userdata) {
  struct audio *const a = userdata;
  for (;;) {
    mtx_lock(&a->mtx);
    size_t const cap = a->cap;
    size_t const len = a->len;
    enum status st = a->status;
    mtx_unlock(&a->mtx);
    if (st == status_closing || cap == len) {
      break;
    }
    a->streams[len] = (struct stream){0};
    error err = ffmpeg_open(&a->streams[len].ffmpeg,
                            &(struct ffmpeg_open_options){
                                .filepath = a->filepath.ptr,
                                .handle = a->handle,
                                .media_type = AVMEDIA_TYPE_AUDIO,
                                .codec = a->streams[0].ffmpeg.codec,
                            });
    if (efailed(err)) {
      err = ethru(err);
      ereport(err);
      break;
    }
    mtx_lock(&a->mtx);
    ++a->len;
    mtx_unlock(&a->mtx);
  }
  return 0;
}

static struct stream *find_stream(struct audio *const a, struct gcd const gcd, int64_t const offset) {
  struct timespec ts;
  timespec_get(&ts, TIME_UTC);
  mtx_lock(&a->mtx);
  size_t const num_stream = a->len;
  if (a->status == status_nothread && a->cap > 1) {
    if (thrd_create(&a->thread, create_sub_stream, a) == thrd_success) {
      a->status = status_running;
    }
  }
  mtx_unlock(&a->mtx);
  struct stream *exact = NULL;
  struct stream *nearby = NULL;
  struct stream *oldest = NULL;
  int64_t const offset_isr = offset * gcd.factor_b;
  for (size_t i = 0; i < num_stream; ++i) {
    struct stream *stream = a->streams + i;
    int64_t pos =
        pts_to_sample_pos_osr(stream->ffmpeg.packet->pts, stream) * gcd.factor_b * gcd.factor_b / gcd.factor_a;
    int const samples = stream->ffmpeg.frame->nb_samples * gcd.factor_b * gcd.factor_b / gcd.factor_a;
    if (pos <= offset_isr && offset_isr < pos + samples) {
      exact = stream;
      break;
    }
    if (pos + samples <= offset_isr && offset_isr < pos + samples * 2) {
      nearby = stream;
      break;
    }
    if (oldest == NULL || oldest->ts.tv_sec > stream->ts.tv_sec ||
        (oldest->ts.tv_sec == stream->ts.tv_sec && oldest->ts.tv_nsec > stream->ts.tv_nsec)) {
      oldest = stream;
    }
  }
  if (!exact) {
    exact = nearby ? nearby : oldest;
  }
  exact->ts = ts;
#if SHOWLOG_AUDIO_SEEK_FIND_STREAM
  {
    char s[256];
    ov_snprintf(s,
                256,
                NULL,
                "a find stream #%d ofs:%lld cur:%lld",
                exact - a->streams,
                offset,
                exact->resampled_current_pos_isr);
    OutputDebugStringA(s);
  }
#endif
  return exact;
}

NODISCARD error audio_read(struct audio *const a,
                           struct resampler *const r,
                           int64_t const offset,
                           int const length,
                           void *const buf,
                           int *const written,
                           bool const accurate) {
  a->wait_index = (a->index_mode == aim_strict) || accurate;
  return stream_read(a, r, find_stream(a, r->gcd, offset), offset, length, buf, written);
}

void audio_destroy(struct audio **const app) {
  if (!app || !*app) {
    return;
  }
  struct audio *a = *app;
  if (a->streams) {
    if (a->status == status_running) {
      mtx_lock(&a->mtx);
      a->status = status_closing;
      mtx_unlock(&a->mtx);
      thrd_join(a->thread, NULL);
    }
    for (size_t i = 0; i < a->len; ++i) {
      ffmpeg_close(&a->streams[i].ffmpeg);
    }
    ereport(mem_free(&a->streams));
  }
  audioidx_destroy(&a->idx);
  ereport(sfree(&a->filepath));
  mtx_destroy(&a->mtx);
  ereport(mem_free(app));
}

NODISCARD error audio_create(struct audio **const app, struct audio_options const *const opt) {
  if (!app || *app || !opt || (!opt->filepath && (opt->handle == NULL || opt->handle == INVALID_HANDLE_VALUE)) ||
      !opt->num_stream) {
    return errg(err_invalid_arugment);
  }
  struct audio *a = NULL;
  error err = mem(&a, 1, sizeof(struct audio));
  if (efailed(err)) {
    ereport(err);
    return NULL;
  }
  *a = (struct audio){
      .index_mode = opt->index_mode,
      .sample_rate = opt->sample_rate,
      .use_sox = opt->use_sox,
      .handle = opt->handle,
  };
  mtx_init(&a->mtx, mtx_plain);

  if (opt->filepath) {
    err = scpy(&a->filepath, opt->filepath);
    if (efailed(err)) {
      ereport(err);
      goto cleanup;
    }
  }

  size_t const cap = opt->num_stream;
  err = mem(&a->streams, cap, sizeof(struct stream));
  if (efailed(err)) {
    ereport(err);
    goto cleanup;
  }
  a->cap = cap;

  a->streams[0] = (struct stream){0};
  err = ffmpeg_open(&a->streams[0].ffmpeg,
                    &(struct ffmpeg_open_options){
                        .filepath = opt->filepath,
                        .handle = opt->handle,
                        .media_type = AVMEDIA_TYPE_AUDIO,
                        .preferred_decoders = opt->preferred_decoders,
                    });
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  a->len = 1;
  a->out_sample_rate = get_output_sample_rate(opt->sample_rate, a->streams[0].ffmpeg.stream->codecpar->sample_rate);

  if (a->index_mode != aim_noindex) {
    err = audioidx_create(&a->idx,
                          &(struct audioidx_create_options){
                              .filepath = opt->filepath,
                              .handle = opt->handle,
                          });
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
  }

  *app = a;
cleanup:
  if (efailed(err)) {
    audio_destroy(&a);
  }
  return err;
}

void *audio_get_codec_parameter(struct audio const *const a) { return a->streams[0].ffmpeg.stream->codecpar; }
