/*
 * hardcode_internal.h — Internal helpers shared across hardcode_parse.c and hardcode_eval.c
 */
#ifndef HARDCODE_INTERNAL_H
#define HARDCODE_INTERNAL_H

#include "hardcode.h"
#include "../delegate/delegate.h"
#include <math.h>
#include <string.h>

int container_kind_from_sym_hc(const MemArena *mem, const SymTable *tab, uint16_t sym_id);

#endif /* HARDCODE_INTERNAL_H */
