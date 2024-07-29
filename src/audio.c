#include "audio.h"

#include <ovprintf.h>
#include <ovthreads.h>
#include <ovutil/win32.h>

#include "audioidx.h"
#include "ffmpeg.h"
#include "now.h"

#define SHOWLOG_AUDIO_GET_INFO 0
#define SHOWLOG_AUDIO_CURRENT_FRAME 0
#define SHOWLOG_AUDIO_REPORT_INDEX_ENTRIES 0
#define SHOWLOG_AUDIO_SEEK 0
#define SHOWLOG_AUDIO_SEEK_ADJUST 0
#define SHOWLOG_AUDIO_SEEK_SPEED 0
#define SHOWLOG_AUDIO_SEEK_FIND_STREAM 0
#define SHOWLOG_AUDIO_READ 0

typedef int16_t sample_t;
static int const g_channels = 2;
static int const g_sample_format = AV_SAMPLE_FMT_S16;
static int const g_sample_size = sizeof(sample_t) * g_channels;

struct stream {
  struct ffmpeg_stream ffmpeg;
  SwrContext *swr_context;
  uint8_t *swr_buf;
  int64_t current_sample_pos_osr;
  int64_t swr_buf_sample_pos_asr;
  int swr_buf_len;
  int swr_buf_written_asr;
  int current_samples_osr;
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

  int active_sample_rate;
  struct audioidx *idx;
  enum audio_index_mode index_mode;
  enum audio_sample_rate sample_rate;
  bool use_sox;
  bool wait_index;

  int64_t first_sample_pos;
};

static inline int64_t get_start_time(struct stream *const stream) {
  return stream->ffmpeg.stream->start_time == AV_NOPTS_VALUE ? 0 : stream->ffmpeg.stream->start_time;
}

static inline int64_t pts_to_sample_pos(int64_t const pts, struct stream *const stream) {
  return av_rescale_q(pts - get_start_time(stream),
                      stream->ffmpeg.cctx->pkt_timebase,
                      av_make_q(1, stream->ffmpeg.stream->codecpar->sample_rate));
}

static inline int64_t sample_pos_to_pts(int64_t const sample, struct stream *const stream) {
  return av_rescale_q(
             sample, av_make_q(1, stream->ffmpeg.stream->codecpar->sample_rate), stream->ffmpeg.cctx->pkt_timebase) +
         get_start_time(stream);
}

static inline int64_t convert_sample_rate(int64_t const sample, int const from, int const to) {
  return av_rescale(sample, to, from);
}

static void stream_destroy(struct stream *const stream) {
  if (stream->swr_context) {
    swr_free(&stream->swr_context);
  }
  if (stream->swr_buf) {
    av_freep(&stream->swr_buf);
  }
  ffmpeg_close(&stream->ffmpeg);
}

static NODISCARD error stream_create(struct stream *const stream,
                                     struct ffmpeg_open_options const *const opt,
                                     enum audio_sample_rate const sample_rate,
                                     bool const use_sox,
                                     int *const active_sample_rate) {
  error err = ffmpeg_open(&stream->ffmpeg, opt);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }

  int out_sample_rate;
  switch (sample_rate) {
  case asr_original:
    out_sample_rate = stream->ffmpeg.stream->codecpar->sample_rate;
    break;
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
    out_sample_rate = (int)sample_rate;
    break;
  }

  stream->current_sample_pos_osr = AV_NOPTS_VALUE;
  stream->swr_buf_sample_pos_asr = AV_NOPTS_VALUE;
  stream->swr_buf_len = out_sample_rate * g_channels;
  int r = av_samples_alloc(&stream->swr_buf, NULL, 2, stream->swr_buf_len, AV_SAMPLE_FMT_S16, 0);
  if (r < 0) {
    err = errffmpeg(r);
    goto cleanup;
  }
  r = swr_alloc_set_opts2(&stream->swr_context,
                          &(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO,
                          g_sample_format,
                          out_sample_rate,
                          &stream->ffmpeg.stream->codecpar->ch_layout,
                          stream->ffmpeg.stream->codecpar->format,
                          stream->ffmpeg.stream->codecpar->sample_rate,
                          0,
                          NULL);
  if (r < 0) {
    err = errffmpeg(r);
    goto cleanup;
  }
  if (use_sox) {
    av_opt_set_int(stream->swr_context, "engine", SWR_ENGINE_SOXR, 0);
  }
  r = swr_init(stream->swr_context);
  if (r < 0) {
    err = errffmpeg(r);
    goto cleanup;
  }
  if (active_sample_rate) {
    *active_sample_rate = out_sample_rate;
  }
cleanup:
  if (efailed(err)) {
    stream_destroy(stream);
  }
  return err;
}

void audio_get_info(struct audio const *const a, struct info_audio *const ai) {
  ai->sample_rate = a->active_sample_rate;
  ai->channels = g_channels;
  ai->bit_depth = sizeof(sample_t) * 8;
  ai->samples = av_rescale_q(a->streams[0].ffmpeg.fctx->duration, AV_TIME_BASE_Q, av_make_q(1, a->active_sample_rate));
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

static void calc_current_position(struct audio *const a, struct stream *const stream) {
  int64_t pos = a->idx ? audioidx_get(a->idx, stream->ffmpeg.packet->pts, a->wait_index) : -1;
  if (pos != -1) {
    // found corrent sample position
    stream->current_sample_pos_osr = pos;
  } else {
    // It seems pts value may be inaccurate.
    // There would be no way to correct the values except to recalculate from the first frame.
    // This program allows inaccurate values.
    // Instead, it avoids the accumulation of errors by not using
    // the received pts as long as it continues to read frames.
    stream->current_sample_pos_osr = pts_to_sample_pos(stream->ffmpeg.packet->pts, stream);
  }
  stream->current_samples_osr = stream->ffmpeg.frame->nb_samples;
#if SHOWLOG_AUDIO_CURRENT_FRAME
  char s[256];
  ov_snprintf(s,
              256,
              NULL,
              "a samplepos: %lld samples: %ld key_frame: %d, pts: %lld start_time: %lld time_base:%f sample_rate:%d",
              stream->current_sample_pos_osr,
              stream->current_samples_osr,
              (stream->ffmpeg.frame->flags & AV_FRAME_FLAG_KEY) != 0,
              stream->ffmpeg.frame->pts,
              stream->ffmpeg.stream->start_time,
              av_q2d(stream->ffmpeg.cctx->pkt_timebase),
              stream->ffmpeg.stream->codecpar->sample_rate);
  OutputDebugStringA(s);
#endif
}

static NODISCARD error grab(struct stream *const stream) {
  int const old_samples_osr = stream->current_samples_osr;
  int const r = ffmpeg_grab(&stream->ffmpeg);
  if (r < 0) {
    return errffmpeg(r);
  }
  stream->current_sample_pos_osr += old_samples_osr;
  stream->current_samples_osr = stream->ffmpeg.frame->nb_samples;
  return eok();
}

static NODISCARD error seek(struct audio *const a, struct stream *stream, int64_t sample) {
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
  int64_t time_stamp = sample_pos_to_pts(sample, stream);
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
    calc_current_position(a, stream);
    if (stream->current_sample_pos_osr > sample) {
#if SHOWLOG_AUDIO_SEEK_ADJUST
      {
        char s[256];
        ov_snprintf(s,
                    256,
                    NULL,
                    "a adjust target: %lld current: %lld rewind: %f",
                    sample,
                    stream->current_sample_pos_osr,
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
        calc_current_position(a, stream);
        break;
      } else {
        // rewind 1s
        time_stamp -= duration1s;
      }
      prevpts = stream->ffmpeg.packet->pts;
      continue;
    }
    break;
  }
#if 0
  if (stream->current_sample_pos_osr < sample) {
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
      *(uint32_t *)((void *)data) = (uint32_t)(sample - stream->current_sample_pos_osr);
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
  while (stream->current_sample_pos_osr + stream->ffmpeg.frame->nb_samples <= sample) {
#if SHOWLOG_AUDIO_SEEK
    char s[256];
    ov_snprintf(s,
                256,
                NULL,
                "csp: %lld / smp: %lld",
                stream->current_sample_pos_osr + stream->ffmpeg.frame->nb_samples,
                sample);
    OutputDebugStringA(s);
#endif
    err = grab(stream);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
  }
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
                                   struct stream *const stream,
                                   int64_t const offset,
                                   int const length,
                                   void *const buf,
                                   int *const written) {
#if SHOWLOG_AUDIO_READ
  char s[256];
  ov_snprintf(s, 256, NULL, "audio_read ofs: %lld / len: %d", offset, length);
  OutputDebugStringA(s);
#endif
  error err = eok();
  uint8_t *dest = buf;
  int read = 0;
  int r = 0;
  int64_t readpos_asr = 0;

start:
  if (read == length) {
    goto cleanup;
  }

readbuf:
  readpos_asr = offset + read;
  if (readpos_asr < stream->swr_buf_sample_pos_asr) {
    goto seek;
  }
  if (readpos_asr < (stream->swr_buf_sample_pos_asr + stream->swr_buf_written_asr)) {
#if SHOWLOG_AUDIO_READ
    OutputDebugStringA(__FILE_NAME__ " readbuf");
#endif
    int const bufpos = (int)(readpos_asr - stream->swr_buf_sample_pos_asr);
    int const samples = imin(stream->swr_buf_written_asr - bufpos, (int)(length - read));
    memcpy(
        dest + (read * g_sample_size), stream->swr_buf + (bufpos * g_sample_size), (size_t)(samples * g_sample_size));
    read += samples;
    goto start;
  }

  // flushswr:
#if SHOWLOG_AUDIO_READ
  OutputDebugStringA(__FILE_NAME__ " flushswr");
#endif
  stream->swr_buf_sample_pos_asr += stream->swr_buf_written_asr;
  r = swr_convert(stream->swr_context, &stream->swr_buf, stream->swr_buf_len, NULL, 0);
  if (r < 0) {
    err = errffmpeg(r);
    goto cleanup;
  }
  stream->swr_buf_written_asr = r;
  if (r) {
    goto readbuf;
  }

seek:
  if (readpos_asr < stream->swr_buf_sample_pos_asr ||
      readpos_asr >= stream->swr_buf_sample_pos_asr + a->active_sample_rate) {
    if (readpos_asr < a->first_sample_pos) {
      goto inject_silence;
    }
#if SHOWLOG_AUDIO_READ
    OutputDebugStringA(__FILE_NAME__ " seek");
#endif
    int64_t const readpos =
        convert_sample_rate(readpos_asr, a->active_sample_rate, stream->ffmpeg.stream->codecpar->sample_rate);
    err = seek(a, stream, readpos);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
    // There is no guarantee that the destination can be moved to the destination by seek,
    // and it may move slightly ahead of the destination.
    // In this case, insert a margin to the destination to adjust the balance.
    if (stream->current_sample_pos_osr > readpos) {
      int const silent = (int)(convert_sample_rate(stream->current_sample_pos_osr,
                                                   stream->ffmpeg.stream->codecpar->sample_rate,
                                                   a->active_sample_rate) -
                               readpos_asr);
      r = swr_inject_silence(stream->swr_context, silent);
      if (r < 0) {
        err = errffmpeg(r);
        goto cleanup;
      }
      stream->swr_buf_sample_pos_asr = readpos_asr;
      stream->swr_buf_written_asr = silent;
      goto readbuf;
    }
    goto convert;
  }

#if SHOWLOG_AUDIO_READ
  OutputDebugStringA(__FILE_NAME__ " grab");
#endif
  err = grab(stream);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }

convert:
#if SHOWLOG_AUDIO_READ
  OutputDebugStringA(__FILE_NAME__ " convert");
#endif
  r = swr_convert(stream->swr_context,
                  (void *)&stream->swr_buf,
                  stream->swr_buf_len,
                  (void *)stream->ffmpeg.frame->data,
                  stream->ffmpeg.frame->nb_samples);
  if (r < 0) {
    err = errffmpeg(r);
    goto cleanup;
  }
  stream->swr_buf_sample_pos_asr = convert_sample_rate(
      stream->current_sample_pos_osr, stream->ffmpeg.stream->codecpar->sample_rate, a->active_sample_rate);
  stream->swr_buf_written_asr = r;
  goto readbuf;

inject_silence:
#if SHOWLOG_AUDIO_READ
  OutputDebugStringA(__FILE_NAME__ " inject_silence");
#endif
  r = swr_inject_silence(stream->swr_context, imin(stream->swr_buf_len, (int)(a->first_sample_pos - readpos_asr)));
  if (r < 0) {
    err = errffmpeg(r);
    goto cleanup;
  }
  r = swr_convert(stream->swr_context, (void *)&stream->swr_buf, stream->swr_buf_len, NULL, 0);
  if (r < 0) {
    err = errffmpeg(r);
    goto cleanup;
  }
  stream->swr_buf_sample_pos_asr = readpos_asr;
  stream->swr_buf_written_asr = r;
  goto readbuf;

cleanup:
  if (efailed(err)) {
    if (eis_errno(err, AVERROR_EOF)) {
      efree(&err);
      if (read < length) {
        memset(dest + read, 0, (size_t)((length - read) * g_sample_size));
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
    error err = stream_create(a->streams + len,
                              &(struct ffmpeg_open_options){
                                  .filepath = a->filepath.ptr,
                                  .handle = a->handle,
                                  .media_type = AVMEDIA_TYPE_AUDIO,
                                  .codec = a->streams[0].ffmpeg.codec,
                              },
                              a->sample_rate,
                              a->use_sox,
                              NULL);
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

static struct stream *find_stream(struct audio *const a, int64_t const offset) {
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
  struct stream *oldest = NULL;
  for (size_t i = 0; i < num_stream; ++i) {
    struct stream *stream = a->streams + i;
    if (stream->swr_buf_sample_pos_asr != AV_NOPTS_VALUE && stream->swr_buf_sample_pos_asr <= offset &&
        offset < stream->swr_buf_sample_pos_asr + stream->swr_buf_written_asr) {
      exact = stream;
      break;
    }
    if (oldest == NULL || oldest->ts.tv_sec > stream->ts.tv_sec ||
        (oldest->ts.tv_sec == stream->ts.tv_sec && oldest->ts.tv_nsec > stream->ts.tv_nsec)) {
      oldest = stream;
    }
  }
  if (!exact) {
    exact = oldest;
  }
  exact->ts = ts;
#if SHOWLOG_AUDIO_SEEK_FIND_STREAM
  {
    char s[256];
    ov_snprintf(
        s, 256, NULL, "a find stream #%d ofs:%lld cur:%lld", exact - a->streams, offset, exact->current_sample_pos_osr);
    OutputDebugStringA(s);
  }
#endif
  return exact;
}

NODISCARD error audio_read(struct audio *const a,
                           int64_t const offset,
                           int const length,
                           void *const buf,
                           int *const written,
                           bool const accurate) {
  a->wait_index = (a->index_mode == aim_strict) || accurate;
  return stream_read(a, find_stream(a, offset), offset, length, buf, written);
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
      stream_destroy(a->streams + i);
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
  err = stream_create(a->streams,
                      &(struct ffmpeg_open_options){
                          .filepath = opt->filepath,
                          .handle = opt->handle,
                          .media_type = AVMEDIA_TYPE_AUDIO,
                          .preferred_decoders = opt->preferred_decoders,
                      },
                      opt->sample_rate,
                      opt->use_sox,
                      &a->active_sample_rate);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  a->len = 1;
  a->first_sample_pos = pts_to_sample_pos(a->streams[0].ffmpeg.stream->start_time, a->streams);

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
