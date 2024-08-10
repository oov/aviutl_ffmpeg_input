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
#define SHOWLOG_AUDIO_SEEK_SPEED 0
#define SHOWLOG_AUDIO_READ 0
#define SHOWLOG_AUDIO_GAP 0

// osr = original sample rate
// asr = active sample rate
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

  int64_t valid_first_sample_pos_asr;
  int out_sample_rate;
  struct audioidx *idx;
  enum audio_index_mode index_mode;
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
  return eok();
}

static NODISCARD error seek(struct audio *const a,
                            struct resampler *const resampler,
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
    int64_t const pos_osr = pts_to_sample_pos_osr(stream->ffmpeg.frame->pts, stream);
    if (pos_osr > sample) {
      if (time_stamp < get_start_time(stream) && prevpts == stream->ffmpeg.frame->pts) {
        // It seems that the pts value is not updated.
        // Depending on the state of the video file, it may not be possible to play back correctly from start_time.
        // Record the sample position obtained at this timing as the valid lower frame.
        a->valid_first_sample_pos_asr = pos_osr * resampler->gcd.factor_b / resampler->gcd.factor_a;
        break;
      } else {
        time_stamp -= duration1s;
      }
      prevpts = stream->ffmpeg.frame->pts;
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
  int64_t pos_osr = pts_to_sample_pos_osr(stream->ffmpeg.frame->pts, stream);
  while (pos_osr + stream->ffmpeg.frame->nb_samples <= sample) {
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
      int64_t const pts_pos = pts_to_sample_pos_osr(stream->ffmpeg.frame->pts, stream);
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

static NODISCARD error read_buffer(
    struct resampler *const resampler, int64_t const readpos_asr, int const length, int *read, uint8_t *const dest) {
#if SHOWLOG_AUDIO_READ
  OutputDebugStringA(__FILE_NAME__ " read_buffer");
#endif
  int const buffer_offset_asr = (int)(readpos_asr - resampler->pos);
  int const samples_asr = imin(length - *read, resampler->written - buffer_offset_asr);
  memcpy(dest + *read * resampler_out_sample_size,
         resampler->buf + buffer_offset_asr * resampler_out_sample_size,
         (size_t)(samples_asr * resampler_out_sample_size));
  *read += samples_asr;
  return eok();
}

static NODISCARD error grab_next_frame(struct resampler *const resampler, struct stream *const stream) {
#if SHOWLOG_AUDIO_READ
  OutputDebugStringA(__FILE_NAME__ " grab_next_frame");
#endif
  error err = grab_next(stream);
  if (efailed(err)) {
    return ethru(err);
  }
  int r = swr_convert(resampler->ctx,
                      (void *)&resampler->buf,
                      resampler->samples,
                      (void *)stream->ffmpeg.frame->data,
                      stream->ffmpeg.frame->nb_samples);
  if (r < 0) {
    return errffmpeg(r);
  }
  resampler->pos += resampler->written;
  resampler->written = r;
#if SHOWLOG_AUDIO_GAP
  int64_t const pts_pos =
      pts_to_sample_pos_osr(stream->ffmpeg.frame->pts, stream) * resampler->gcd.factor_b / resampler->gcd.factor_a;
  if (resampler->pos != pts_pos) {
    char s[256];
    ov_snprintf(s, 256, NULL, "pos gap: %lld %lld", resampler->pos, pts_pos);
    OutputDebugStringA(s);
  }
#endif
  return eok();
}

static NODISCARD error seek_frame(struct audio *const a,
                                  struct resampler *const resampler,
                                  struct stream *const stream,
                                  int64_t const pos_osr) {
#if SHOWLOG_AUDIO_READ
  OutputDebugStringA(__FILE_NAME__ " seek_frame");
#endif
  int64_t real_current_pos_osr;
  error err = seek(a, resampler, stream, pos_osr, &real_current_pos_osr);
  if (efailed(err)) {
    return ethru(err);
  }

  int r = swr_init(resampler->ctx);
  if (r < 0) {
    return errffmpeg(r);
  }
  r = swr_convert(resampler->ctx,
                  (void *)&resampler->buf,
                  resampler->samples,
                  (void *)stream->ffmpeg.frame->data,
                  stream->ffmpeg.frame->nb_samples);
  if (r < 0) {
    return errffmpeg(r);
  }
  resampler->pos = real_current_pos_osr * resampler->gcd.factor_b / resampler->gcd.factor_a;
  resampler->written = r;
  return eok();
}

static NODISCARD error convert_frame(struct resampler *const resampler, struct stream *const stream) {
#if SHOWLOG_AUDIO_READ
  OutputDebugStringA(__FILE_NAME__ " convert_frame");
#endif
  int r = swr_convert(resampler->ctx,
                      (void *)&resampler->buf,
                      resampler->samples,
                      (void *)stream->ffmpeg.frame->data,
                      stream->ffmpeg.frame->nb_samples);
  if (r < 0) {
    return errffmpeg(r);
  }
  resampler->pos =
      pts_to_sample_pos_osr(stream->ffmpeg.frame->pts, stream) * resampler->gcd.factor_b / resampler->gcd.factor_a;
  resampler->written = r;
  return eok();
}

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
                offset_asr,
                length,
                pts_to_sample_pos_osr(stream->ffmpeg.frame->pts, stream),
                resampler->pos);
    OutputDebugStringA(s);
  }
#endif
  error err = eok();
  uint8_t *dest = buf;
  int read = 0;

  while (read < length) {
    int64_t const readpos_asr = offset_asr + read;

    // Is the data we want before the first frame that can be obtained?
    if (a->valid_first_sample_pos_asr != AV_NOPTS_VALUE && readpos_asr < a->valid_first_sample_pos_asr) {
      int const samples = imin((int)(a->valid_first_sample_pos_asr - readpos_asr), length - read);
      memset(dest + (read * resampler_out_sample_size), 0, (size_t)(samples * resampler_out_sample_size));
      read += samples;
      break;
    }

    // Is the data we want in the resampled buffer?
    if (readpos_asr >= resampler->pos && readpos_asr < resampler->pos + resampler->written) {
      err = read_buffer(resampler, readpos_asr, length, &read, dest);
      if (efailed(err)) {
        goto cleanup;
      }
      continue;
    }

    // Is the data we want in the current frame?
    int64_t const frame_pos_osr = pts_to_sample_pos_osr(stream->ffmpeg.frame->pts, stream);
    int64_t const frame_pos_asr = frame_pos_osr * resampler->gcd.factor_b / resampler->gcd.factor_a;
    int64_t const frame_end_pos_asr =
        (frame_pos_osr + stream->ffmpeg.frame->nb_samples) * resampler->gcd.factor_b / resampler->gcd.factor_a;
    if (readpos_asr >= frame_pos_asr && readpos_asr < frame_end_pos_asr) {
      err = convert_frame(resampler, stream);
      if (efailed(err)) {
        goto cleanup;
      }
      continue;
    }
    // Is the data we want in the next frame?
    int64_t const next_frame_end_pos_asr =
        (frame_pos_osr + stream->ffmpeg.frame->nb_samples * 2) * resampler->gcd.factor_b / resampler->gcd.factor_a;
    if (readpos_asr >= frame_end_pos_asr && readpos_asr < next_frame_end_pos_asr) {
      err = grab_next_frame(resampler, stream);
      if (efailed(err)) {
        goto cleanup;
      }
      continue;
    }

    // The data we want cannot be obtained without seeking.
    err = seek_frame(a, resampler, stream, readpos_asr * resampler->gcd.factor_a / resampler->gcd.factor_b);
    if (efailed(err)) {
      goto cleanup;
    }
  }

cleanup:
#if SHOWLOG_AUDIO_READ
{
  char s[256];
  ov_snprintf(s, 256, NULL, "a read done %d", read);
  OutputDebugStringA(s);
}
#endif
  if (efailed(err)) {
    if (eis_errno(err, AVERROR_EOF)) {
      efree(&err);
    } else {
      ereport(err);
    }
  }
  if (read < length) {
    memset(dest + (read * resampler_out_sample_size), 0, (size_t)((length - read) * resampler_out_sample_size));
    read = length;
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
  for (size_t i = 0; i < num_stream; ++i) {
    struct stream *stream = a->streams + i;
    int64_t const pos_osr = pts_to_sample_pos_osr(stream->ffmpeg.frame->pts, stream);
    int64_t const pos = pos_osr * gcd.factor_b / gcd.factor_a;
    int64_t const pos_end = (pos_osr + stream->ffmpeg.frame->nb_samples) * gcd.factor_b / gcd.factor_a;
    if (pos <= offset && offset < pos_end) {
      exact = stream;
      break;
    }
    int64_t const next_pos_end = (pos_osr + stream->ffmpeg.frame->nb_samples * 2) * gcd.factor_b / gcd.factor_a;
    if (pos_end <= offset && offset < next_pos_end) {
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
      .handle = opt->handle,
      .valid_first_sample_pos_asr = AV_NOPTS_VALUE,
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
