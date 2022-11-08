#include "ovtest.h"

#include "ffmpegutil.h"

static void verify_ffmpegutil_find_preferred_decoder(char const *const decoders,
                                                     char const *const codec,
                                                     size_t *const pos,
                                                     char const *const expected) {
  char buf[32];
  char const *ret = ffmpegutil_find_preferred_decoder(decoders, codec, pos, buf);
  if (!TEST_CHECK((expected == NULL && ret == NULL) || (expected != NULL && ret != NULL))) {
    return;
  }
  TEST_CHECK(strcmp(ret, expected) == 0);
  TEST_MSG("expected %s got %s", expected, ret);
}

static void test_basic(void) {
  static char const decoders[] = "h264_qsv, h265_cuvid, h264_cuvid, h264_amf";
  static char const codec[] = "h264";
  size_t pos = 0;
  verify_ffmpegutil_find_preferred_decoder(decoders, codec, &pos, "h264_qsv");
  verify_ffmpegutil_find_preferred_decoder(decoders, codec, &pos, "h264_cuvid");
  verify_ffmpegutil_find_preferred_decoder(decoders, codec, &pos, "h264_amf");
}

TEST_LIST = {
    {"test_basic", test_basic},
    {NULL, NULL},
};
