#pragma once

#include <stddef.h>

void progress_init(void);
void progress_destroy(void);

void progress_set(void const *const user_context, size_t const progress);
void progress_set_exedit_window(size_t const hwnd);
