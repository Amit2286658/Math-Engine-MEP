/*
 * mem_internal.h — Internal helpers shared across mem_arena.c, mem_containers.c, and mem_gc.c
 */
#ifndef MEM_INTERNAL_H
#define MEM_INTERNAL_H

#include "mem_arena.h"
#include "sym_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

static inline double canonicalize_f64(double d) {
    uint64_t bits = 0;
    memcpy(&bits, &d, sizeof(bits));
    if ((bits & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL &&
        (bits & 0x000FFFFFFFFFFFFFULL) != 0ULL) {
        uint64_t qnan = 0x7FF8000000000000ULL;
        double qd = 0.0;
        memcpy(&qd, &qnan, sizeof(qd));
        return qd;
    }
    return d;
}

/* 4-bit nibble accessors for `MemArena.gc_mark` (2 pool slots per byte) */
static inline uint8_t mem_gc_get_mark(const MemArena *mem, uint16_t idx) {
    uint8_t b = mem->gc_mark[idx >> 1u];
    return (idx & 1u) ? (uint8_t)(b >> 4u) : (uint8_t)(b & 0x0Fu);
}

static inline void mem_gc_set_mark(MemArena *mem, uint16_t idx, uint8_t val) {
    uint16_t byte_idx = (uint16_t)(idx >> 1u);
    uint8_t  b        = mem->gc_mark[byte_idx];
    if (idx & 1u) {
        mem->gc_mark[byte_idx] = (uint8_t)((b & 0x0Fu) | ((val & 0x0Fu) << 4u));
    } else {
        mem->gc_mark[byte_idx] = (uint8_t)((b & 0xF0u) | (val & 0x0Fu));
    }
}

bool mem_heap_realloc_block(MemArena *mem, uint16_t data_idx, uint32_t new_slots);
void mem_heap_free_block(MemArena *mem, uint16_t data_idx);
void mem_str_hash_insert(MemArena *mem, StrSlice s);


#define NANBOX_PREFIX     0xFFF8000000000000ULL
#define NANBOX_MASK       0xFFF8000000000000ULL
#define NANBOX_TAG_SHIFT  48
#define NANBOX_TYPE_SHIFT 32
#define NANBOX_UNIT_SHIFT 40

#define NB_TAG_VOID       0xFFF8ULL
#define NB_TAG_POOL       0xFFF9ULL
#define NB_TAG_STR        0xFFFAULL
#define NB_TAG_I32        0xFFFBULL
#define NB_TAG_F32        0xFFFCULL

#endif /* MEM_INTERNAL_H */
