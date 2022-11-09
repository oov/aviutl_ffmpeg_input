#pragma once

#include "ovbase.h"

struct ipcclient;

struct ipcclient_options {
  wchar_t const *pipe_name;
  uint32_t signature;
  uint32_t protocol_version;
  uint32_t connect_timeout_msec;
  void *userdata;
  bool (*is_aborted)(void *userdata);
};

NODISCARD error ipcclient_create(struct ipcclient **const c, struct ipcclient_options *opt);
NODISCARD error ipcclient_destroy(struct ipcclient **const c);
NODISCARD error ipcclient_grow_buffer(struct ipcclient *const c, size_t const new_buffer_size, void **pptr);

struct ipcclient_request {
  uint32_t event_id;
  uint32_t size;
  void *ptr;
};

struct ipcclient_response {
  uint32_t size;
  void *ptr;
};

NODISCARD error ipcclient_call(struct ipcclient *const c,
                               struct ipcclient_request const *const req,
                               struct ipcclient_response *const resp);
