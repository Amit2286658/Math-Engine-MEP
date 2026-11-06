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

bool mem_heap_realloc_block(MemArena *mem, uint16_t data_idx, uint32_t new_slots);
void mem_heap_free_block(MemArena *mem, uint16_t data_idx);


#define NANBOX_PREFIX     0xFFF8000000000000ULL
#define NANBOX_MASK       0xFFF8000000000000ULL
#define NANBOX_TAG_SHIFT  48
#define NANBOX_TYPE_SHIFT 32
#define NANBOX_UNIT_SHIFT 40

#define NB_TAG_VOID       0xFFF8ULL
#define NB_TAG_POOL       0xFFF9ULL
#define NB_TAG_STR        0xFFFAULL

#endif /* MEM_INTERNAL_H */
