#pragma once

#include "ovbase.h"
#include "video.h"

struct config;

NODISCARD error config_create(struct config **cp);
NODISCARD error config_load(struct config *c);
NODISCARD error config_save(struct config *c);
void config_destroy(struct config **cp);

char const *config_get_preferred_decoders(struct config const *const c);
bool config_get_need_postfix(struct config const *const c);
enum video_format_scaling_algorithm config_get_scaling(struct config const *const c);
bool config_get_invert_phase(struct config const *const c);

NODISCARD error config_set_preferred_decoders(struct config *const c, char const *const preferred_decoders);
NODISCARD error config_set_need_postfix(struct config *const c, bool const need_postfix);
NODISCARD error config_set_scaling(struct config *const c, enum video_format_scaling_algorithm scaling);
NODISCARD error config_set_invert_phase(struct config *const c, bool const invert_phase);
