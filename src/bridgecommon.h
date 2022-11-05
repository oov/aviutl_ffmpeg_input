#pragma once

#include "ovbase.h"
#include "ovutil/win32.h"

#include "info.h"
#include "ovbase.h"

enum bridge_event_id {
  bridge_event_open = 1,
  bridge_event_close = 2,
  bridge_event_get_info = 3,
  bridge_event_read = 4,
};

#define BRIDGE_IPC_SIGNATURE (0x96419697)
#define BRIDGE_IPC_VERSION (1)

#ifndef PACKED
#  if __has_c_attribute(PACKED)
#    define PACKED [[packed]]
#  elif __has_attribute(packed)
#    define PACKED __attribute__((packed))
#  else
#    define PACKED
#  endif
#endif // #endif

#ifdef __GNUC__
#  pragma GCC diagnostic push
#  if __has_warning("-Wpacked")
#    pragma GCC diagnostic ignored "-Wpacked"
#  endif
#endif // __GNUC__

struct PACKED bridge_event_open_request {
  int32_t filepath_size;
};

struct PACKED bridge_event_open_response {
  uint64_t id;
  uint32_t frame_size;
  uint32_t sample_size;
};

struct PACKED bridge_event_close_request {
  uint64_t id;
};

struct PACKED bridge_event_close_response {
  int32_t success;
};

struct PACKED bridge_event_get_info_request {
  uint64_t id;
};

struct PACKED bridge_event_get_info_response {
  int32_t success;
  int32_t flag;
  int32_t rate;
  int32_t scale;
  int32_t video_frames;
  int32_t video_format_size;
  int32_t audio_samples;
  int32_t audio_format_size;
  uint32_t handler;
};

struct PACKED bridge_event_read_request {
  uint64_t id;
  int32_t start;
  int32_t length; // 0 = video, other = audio
};

struct PACKED bridge_event_read_response {
  int32_t written;
  wchar_t fmo_name[16];
};

#ifdef __GNUC__
#  pragma GCC diagnostic pop
#endif // __GNUC__

static inline void build_pipe_name(wchar_t *const buf64, wchar_t const *const unique_id) {
  wcscpy(buf64, L"\\\\.\\pipe\\aui_bridge_");
  wcscat(buf64, unique_id);
}
