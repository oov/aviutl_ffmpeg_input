#pragma once

#include "ovbase.h"

struct ipcserver;

struct ipcserver_context {
  struct ipcserver *s;
  uint32_t event_id;
  uint32_t buffer_size;
  void *buffer;
  void *userdata;
  bool (*is_waiting)(struct ipcserver_context *const ctx);
  NODISCARD error (*grow_buffer)(struct ipcserver_context *const ctx, uint32_t const new_buffer_size);
  void (*finish)(struct ipcserver_context *const ctx, error err);
};

typedef void (*ipcserver_handler_func)(struct ipcserver_context *const ctx);
typedef void (*ipcserver_error_handler_func)(struct ipcserver *const s, void *const userdata, error err);

struct ipcserver_options {
  wchar_t const *pipe_name;
  uint32_t signature;
  uint32_t protocol_version;

  void *userdata;
  ipcserver_handler_func handler;
  ipcserver_error_handler_func error_handler;
};

NODISCARD error ipcserver_create(struct ipcserver **const s, struct ipcserver_options const *const opt);
NODISCARD error ipcserver_destroy(struct ipcserver **const s);
