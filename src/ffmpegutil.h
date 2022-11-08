#pragma once

#include "ovbase.h"

char const *ffmpegutil_find_preferred_decoder(char const *const decoders,
                                              char const *const codec,
                                              size_t *const pos,
                                              char *const buf32);
