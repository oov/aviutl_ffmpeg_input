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


TEST_LIST = {
    {"test_find_preferred", test_find_preferred},
    {NULL, NULL},
};
