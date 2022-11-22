#include "ffmpeg.c"

#ifndef FFMPEGDIR
#  define FFMPEGDIR L"."
#endif
#ifndef TESTDATADIR
#  define TESTDATADIR L"."
#endif

static void initdll(void) { SetDllDirectoryW(FFMPEGDIR); }
#define TEST_MY_INIT initdll()
#include "ovtest.h"

static AVCodec const dummy_codecs[] = {
    {
        .id = AV_CODEC_ID_H264,
        .name = "h264",
    },
    {
        .id = AV_CODEC_ID_H264,
        .name = "h264_qsv",
    },
    {
        .id = AV_CODEC_ID_H265,
        .name = "h265_cuvid",
    },
    {
        .id = AV_CODEC_ID_H264,
        .name = "h264_cuvid",
    },
    {
        .id = AV_CODEC_ID_H264,
        .name = "h264_amf",
    },
};

static AVCodec const *dummy_finder(char const *name) {
  for (size_t i = 0, len = sizeof(dummy_codecs) / sizeof(dummy_codecs[0]); i < len; ++i) {
    if (strcmp(name, dummy_codecs[i].name) == 0) {
      return dummy_codecs + i;
    }
  }
  return NULL;
}

static void verify_find_preferred(char const *const decoders,
                                  AVCodec const *const codec,
                                  size_t *const pos,
                                  char const *const expected) {
  AVCodec const *ret = find_preferred(dummy_finder, decoders, codec, pos);
  if (!TEST_CHECK((expected == NULL && ret == NULL) || (expected != NULL && ret != NULL))) {
    return;
  }
  TEST_CHECK(strcmp(ret->name, expected) == 0);
  TEST_MSG("expected %s got %s", expected, ret->name);
}

static void test_find_preferred(void) {
  static char const decoders[] = "h264_qsv, h265_cuvid, dummy, h264_cuvid, h264_amf";
  AVCodec const *codec = dummy_codecs;
  size_t pos = 0;
  verify_find_preferred(decoders, codec, &pos, "h264_qsv");
  verify_find_preferred(decoders, codec, &pos, "h264_cuvid");
  verify_find_preferred(decoders, codec, &pos, "h264_amf");
}

static NODISCARD error open_stream(struct ffmpeg_stream *fs, wchar_t const *const filename) {
  struct wstr ws = {0};
  error err = scpym(&ws, TESTDATADIR, L"\\", filename);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  err = ffmpeg_open(fs, ws.ptr, AVMEDIA_TYPE_VIDEO, NULL);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
cleanup:
  ereport(sfree(&ws));
  return err;
}

static void test_seek(void) {
  int r;
  struct ffmpeg_stream fs = {0};
  error err = open_stream(&fs, L"15secs.mp4");
  if (!TEST_SUCCEEDED_F(err)) {
    goto cleanup;
  }
  int64_t const time_stamp = av_rescale_q(126, av_inv_q(fs.stream->time_base), fs.stream->avg_frame_rate);
  if (!TEST_SUCCEEDED_F(ffmpeg_seek(&fs, time_stamp))) {
    goto cleanup;
  }
  if (!TEST_CHECK((r = read_packet(&fs)) == 0)) {
    TEST_MSG("want 0 got %d", r);
    err = errffmpeg(r);
    goto cleanup;
  }
  if (!TEST_CHECK(fs.packet->pts <= time_stamp)) {
    goto cleanup;
  }
  if (!TEST_CHECK((r = send_packet(&fs)) == 0)) {
    TEST_MSG("want 0 got %d", r);
    err = errffmpeg(r);
    goto cleanup;
  }
  if (!TEST_CHECK((r = receive_frame(&fs)) == 0)) {
    TEST_MSG("want 0 got %d", r);
    err = errffmpeg(r);
    goto cleanup;
  }
  if (!TEST_CHECK(fs.frame->pts == 30720)) {
    TEST_MSG("want 30720 got %lld", fs.frame->pts);
    goto cleanup;
  }

cleanup:
  ffmpeg_close(&fs);
  ereport(err);
}

TEST_LIST = {
    {"test_find_preferred", test_find_preferred},
    {"test_seek", test_seek},
    {NULL, NULL},
};
