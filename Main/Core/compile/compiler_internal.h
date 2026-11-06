/*
 * compiler_internal.h — Internal helpers shared across compiler_decl.c, compiler_slice.c, and compiler_unit.c
 */
#ifndef COMPILER_INTERNAL_H
#define COMPILER_INTERNAL_H

#include "compiler.h"
#include "parser.h"
#include "hardcode.h"
#include "evaluator.h"
#include "../lib/delegate/delegate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    MemArena   *mem;
    SymTable   *tab;
    const char *src;
    uint16_t    len;
    uint16_t    pos;
    uint16_t    cur_mod;
} ExtLoader;

uint16_t ext_skip_ws_at(const char *src, uint16_t p, uint16_t len);
void ext_skip_ws(ExtLoader *L);
bool ext_match_char(ExtLoader *L, char expected);
StrSlice ext_read_ident(ExtLoader *L);
StrSlice ext_read_quoted(ExtLoader *L);
int ext_find_duplicate_overload(const SymTable *tab, uint16_t sym_id, uint16_t mod_id,
                                uint8_t fixity, uint8_t pcount,
                                const uint16_t *ptypes, const uint8_t *punits);
bool ext_peek_is_kw_type_decl(const ExtLoader *L, uint16_t pos);
void ext_parse_type_decl(ExtLoader *L, uint8_t decl_flags);
bool ext_parse_one_param(ExtLoader *L, StrSlice *out_name,
                         uint16_t *out_type, uint8_t *out_unit);
bool ext_parse_return_spec(ExtLoader *L, uint16_t *out_rtype, uint8_t *out_runit);
void ext_compile_body(ExtLoader *L, OpOverload *ov, StrSlice *pnames);
void ext_parse_sym_decl(ExtLoader *L, uint16_t sym_id, uint8_t decl_flags, bool allow_redef);
void ext_parse_sym_decl_ex(ExtLoader *L, uint16_t sym_id, uint8_t decl_flags,
                           bool allow_redef, uint16_t assoc_type_id);
bool ext_peek_is_mod_header(const char *src, uint16_t pos, uint16_t len,
                            uint16_t *out_id_start, uint16_t *out_id_len,
                            uint16_t *out_after_rbrack, bool *out_malformed);
uint16_t ext_peek_modifiers(const ExtLoader *L, uint16_t start_pos,
                            uint8_t *out_flags, bool *out_has_mods,
                            bool report_conflicts);
bool ext_peek_is_quoted_decl(const ExtLoader *L, uint16_t pos);
bool ext_peek_is_op_decl(const ExtLoader *L, uint16_t pos);
uint16_t ext_skip_opt_fn_kw(const ExtLoader *L, uint16_t pos);
bool ext_peek_is_fn_decl(const ExtLoader *L, uint16_t pos);
bool ext_peek_is_var_decl(const ExtLoader *L, uint16_t pos);
bool ext_peek_is_import_decl(const ExtLoader *L, uint16_t pos);
uint16_t ext_find_stmt_chunk_end(const ExtLoader *L);
bool ext_chunk_is_var_decl_only(const ExtLoader *L, uint16_t start, uint16_t end);

#endif /* COMPILER_INTERNAL_H */
