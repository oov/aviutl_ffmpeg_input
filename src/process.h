#pragma once

#include "ovbase.h"

typedef void (*process_notify_func)(void *userdata);

struct process;

struct process_options {
  wchar_t *module_path;
  void *userdata;
  process_notify_func on_terminate;
};

NODISCARD error process_create(struct process **const pp, struct process_options const *const opt);
NODISCARD error process_destroy(struct process **const pp);
wchar_t const *process_get_unique_id(struct process *const p);
