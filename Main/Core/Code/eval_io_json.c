/*
 * eval_io_json.c — Sandboxed File I/O & Recursive JSON Parser/Serializer
 */
#include "eval_internal.h"

static bool eval_resolve_io_path(MemArena *mem, Value path_val, char *out_buf, size_t cap) {
    char raw[220];
    if (!val_as_cstr(mem, path_val, raw, sizeof(raw)) || raw[0] == '\0') {
        mem_err(mem, "File path must be a non-empty string");
        return false;
    }
    if (raw[0] != '/' && mem->import_dir[0] != '\0') {
        snprintf(out_buf, cap, "%s/%s", mem->import_dir, raw);
    } else {
        snprintf(out_buf, cap, "%s", raw);
    }
    return true;
}

Value eval_file_io_op(MemArena *mem, SymTable *tab, IntrinsicId sub_op, Value a, Value b) {
    char path[512];
    if (!eval_resolve_io_path(mem, a, path, sizeof(path))) return val_void();

    if (sub_op == INTR_FILE_EXISTS) { /* @file_exists(path) */
        FILE *fp = fopen(path, "rb");
        if (fp) {
            fclose(fp);
            return val_make_f64(mem, mem->fast_real_type, 1.0);
        }
        return val_make_f64(mem, mem->fast_real_type, 0.0);
    }

    if (sub_op == INTR_FILE_READ) { /* @file_read(path) */
        FILE *fp = fopen(path, "rb");
        if (!fp) {
            mem_err(mem, "Cannot open file '%s' for reading", path);
            return val_void();
        }
        if (fseek(fp, 0, SEEK_END) != 0) {
            fclose(fp);
            mem_err(mem, "Failed to seek file '%s'", path);
            return val_void();
        }
        long sz = ftell(fp);
        if (sz < 0 || sz > 28000L) {
            fclose(fp);
            mem_err(mem, "File '%s' size (%ld bytes) exceeds string pool limit", path, sz);
            return val_void();
        }
        rewind(fp);
        char *buf = (char *)malloc((size_t)sz + 1u);
        if (!buf) {
            fclose(fp);
            mem_err(mem, "Out of memory reading file '%s'", path);
            return val_void();
        }
        size_t n = fread(buf, 1, (size_t)sz, fp);
        fclose(fp);
        buf[n] = '\0';
        StrSlice s = mem_intern_str(mem, buf, (uint16_t)n);
        free(buf);
        if (mem->has_error) return val_void();
        return val_make_str(mem, mem->str_type_id, s);
    }

    if (sub_op == INTR_FILE_WRITE || sub_op == INTR_FILE_APPEND) { /* @file_write / @file_append */
        const char *mode = (sub_op == INTR_FILE_APPEND) ? "ab" : "wb";
        FILE *fp = fopen(path, mode);
        if (!fp) {
            mem_err(mem, "Cannot open file '%s' for writing", path);
            return val_void();
        }
        const char *data_ptr = NULL;
        size_t data_len = 0;
        char fmt_buf[1024];
        if (mem->str_type_id != 0 && val_get_type(mem, b) == mem->str_type_id) {
            StrSlice s = val_get_str(mem, b);
            data_ptr = mem_str_ptr(mem, s);
            data_len = s.len;
        } else {
            Value sv = delegate_eval_intrinsic(mem, tab, INTR_TOSTR, b, val_void(), val_void(), 0);
            if (mem->has_error) { fclose(fp); return val_void(); }
            StrSlice s = val_get_str(mem, sv);
            size_t clen = (s.len < sizeof(fmt_buf) - 1u) ? s.len : (sizeof(fmt_buf) - 1u);
            memcpy(fmt_buf, mem_str_ptr(mem, s), clen);
            fmt_buf[clen] = '\0';
            data_ptr = fmt_buf;
            data_len = clen;
        }
        size_t written = (data_len > 0) ? fwrite(data_ptr, 1, data_len, fp) : 0u;
        fclose(fp);
        if (written != data_len) {
            mem_err(mem, "Incomplete write to file '%s'", path);
            return val_void();
        }
        return val_make_f64(mem, mem->fast_real_type, (double)written);
    }
    return val_void();
}

static void json_skip_ws(const char *s, size_t len, size_t *pos) {
    while (*pos < len) {
        char c = s[*pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') (*pos)++;
        else break;
    }
}

static int json_hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static bool json_parse_string_slice(MemArena *mem, const char *s, size_t len,
                                    size_t *pos, StrSlice *out_slice) {
    if (*pos >= len || s[*pos] != '"') {
        mem_err(mem, "JSON parse error: expected '\"' at offset %u", (unsigned)*pos);
        return false;
    }
    (*pos)++;
    char stack_buf[2048];
    char *buf = stack_buf;
    size_t cap = sizeof(stack_buf);
    size_t out_len = 0;

    while (*pos < len) {
        char c = s[(*pos)++];
        if (c == '"') {
            *out_slice = mem_intern_str(mem, buf, (uint16_t)out_len);
            if (buf != stack_buf) free(buf);
            return !mem->has_error;
        }
        if (out_len + 8u >= cap) {
            size_t new_cap = cap * 2u;
            if (new_cap > 28000u) {
                if (buf != stack_buf) free(buf);
                mem_err(mem, "JSON string literal exceeds maximum length");
                return false;
            }
            char *nb = (char *)malloc(new_cap);
            if (!nb) {
                if (buf != stack_buf) free(buf);
                mem_err(mem, "Out of memory parsing JSON string");
                return false;
            }
            memcpy(nb, buf, out_len);
            if (buf != stack_buf) free(buf);
            buf = nb;
            cap = new_cap;
        }
        if (c == '\\') {
            if (*pos >= len) break;
            char esc = s[(*pos)++];
            switch (esc) {
                case '"':  buf[out_len++] = '"';  break;
                case '\\': buf[out_len++] = '\\'; break;
                case '/':  buf[out_len++] = '/';  break;
                case 'b':  buf[out_len++] = '\b'; break;
                case 'f':  buf[out_len++] = '\f'; break;
                case 'n':  buf[out_len++] = '\n'; break;
                case 'r':  buf[out_len++] = '\r'; break;
                case 't':  buf[out_len++] = '\t'; break;
                case 'u': {
                    if (*pos + 4u > len) {
                        if (buf != stack_buf) free(buf);
                        mem_err(mem, "JSON parse error: incomplete \\uXXXX escape");
                        return false;
                    }
                    int d0 = json_hex_digit(s[*pos + 0]);
                    int d1 = json_hex_digit(s[*pos + 1]);
                    int d2 = json_hex_digit(s[*pos + 2]);
                    int d3 = json_hex_digit(s[*pos + 3]);
                    if (d0 < 0 || d1 < 0 || d2 < 0 || d3 < 0) {
                        if (buf != stack_buf) free(buf);
                        mem_err(mem, "JSON parse error: invalid hex in \\uXXXX escape");
                        return false;
                    }
                    (*pos) += 4u;
                    uint32_t cp = ((uint32_t)d0 << 12u) | ((uint32_t)d1 << 8u) |
                                  ((uint32_t)d2 << 4u)  | (uint32_t)d3;
                    if (cp < 0x80u) {
                        buf[out_len++] = (char)cp;
                    } else if (cp < 0x800u) {
                        buf[out_len++] = (char)(0xC0u | (cp >> 6u));
                        buf[out_len++] = (char)(0x80u | (cp & 0x3Fu));
                    } else {
                        buf[out_len++] = (char)(0xE0u | (cp >> 12u));
                        buf[out_len++] = (char)(0x80u | ((cp >> 6u) & 0x3Fu));
                        buf[out_len++] = (char)(0x80u | (cp & 0x3Fu));
                    }
                    break;
                }
                default:
                    buf[out_len++] = esc;
                    break;
            }
        } else {
            buf[out_len++] = c;
        }
    }
    if (buf != stack_buf) free(buf);
    mem_err(mem, "JSON parse error: unterminated string literal");
    return false;
}

static Value json_parse_val_rec(MemArena *mem, SymTable *tab, const char *s,
                                size_t len, size_t *pos, uint8_t depth) {
    if (depth > 24u) {
        mem_err(mem, "JSON nesting depth limit (24) exceeded");
        return val_void();
    }
    json_skip_ws(s, len, pos);
    if (*pos >= len) {
        mem_err(mem, "JSON parse error: unexpected end of input");
        return val_void();
    }
    char c = s[*pos];

    /* 1. String */
    if (c == '"') {
        StrSlice sl = { 0, 0 };
        if (!json_parse_string_slice(mem, s, len, pos, &sl)) return val_void();
        return val_make_str(mem, mem->str_type_id, sl);
    }

    /* 2. Object -> $map */
    if (c == '{') {
        (*pos)++;
        uint16_t hdr_idx = mem_map_alloc_hdr(mem, SYM_TYPE_ANY, 4);
        if (mem->has_error) return val_void();
        Value mval = val_make_comp(mem, mem->map_type_id, hdr_idx, MEM_MAP_HDR_SLOTS);
        uint16_t saved_tr = mem_push_temp_root(mem, &mval);

        json_skip_ws(s, len, pos);
        if (*pos < len && s[*pos] == '}') {
            (*pos)++;
            mem_write_pool(mem, (uint16_t)(val_get_pool_idx(mval) + 0), 0.0);
            mem_pop_temp_roots(mem, saved_tr);
            return mval;
        }
        uint16_t common_tid  = 0;
        bool     mixed_types = false;
        uint16_t entry_cnt   = 0;
        while (*pos < len && !mem->has_error) {
            json_skip_ws(s, len, pos);
            StrSlice kslice = { 0, 0 };
            if (!json_parse_string_slice(mem, s, len, pos, &kslice)) {
                mem_pop_temp_roots(mem, saved_tr);
                return val_void();
            }
            Value kval = val_make_str(mem, mem->str_type_id, kslice);
            (void)mem_push_temp_root(mem, &kval);

            json_skip_ws(s, len, pos);
            if (*pos >= len || s[*pos] != ':') {
                mem_pop_temp_roots(mem, saved_tr);
                mem_err(mem, "JSON parse error: expected ':' after object key at offset %u", (unsigned)*pos);
                return val_void();
            }
            (*pos)++;

            Value vval = json_parse_val_rec(mem, tab, s, len, pos, (uint8_t)(depth + 1));
            if (mem->has_error) {
                mem_pop_temp_roots(mem, saved_tr);
                return val_void();
            }
            uint16_t vtid = val_get_type(mem, vval);
            if (entry_cnt == 0) {
                common_tid = vtid;
            } else if (vtid != common_tid) {
                mixed_types = true;
            }
            entry_cnt++;

            (void)delegate_eval_intrinsic(mem, tab, INTR_MAP_SET, mval, kval, vval, 0);
            mem->temp_root_count = (uint16_t)(saved_tr + 1u);
            if (mem->has_error) {
                mem_pop_temp_roots(mem, saved_tr);
                return val_void();
            }

            json_skip_ws(s, len, pos);
            if (*pos < len && s[*pos] == ',') {
                (*pos)++;
                continue;
            }
            if (*pos < len && s[*pos] == '}') {
                (*pos)++;
                uint16_t final_tid = mixed_types ? (uint16_t)SYM_TYPE_ANY : common_tid;
                mem_write_pool(mem, (uint16_t)(val_get_pool_idx(mval) + 0), (double)final_tid);
                mem_pop_temp_roots(mem, saved_tr);
                return mval;
            }
            mem_pop_temp_roots(mem, saved_tr);
            mem_err(mem, "JSON parse error: expected ',' or '}' in object at offset %u", (unsigned)*pos);
            return val_void();
        }
        mem_pop_temp_roots(mem, saved_tr);
        mem_err(mem, "JSON parse error: unterminated object '{'");
        return val_void();
    }

    /* 3. Array -> $vec */
    if (c == '[') {
        (*pos)++;
        json_skip_ws(s, len, pos);
        if (*pos < len && s[*pos] == ']') {
            (*pos)++;
            uint16_t hdr_idx = mem_vec_alloc_hdr(mem, 0, 0);
            if (mem->has_error) return val_void();
            return val_make_comp(mem, mem->vec_type_id, hdr_idx, MEM_VEC_HDR_SLOTS);
        }

        Value items[256];
        uint16_t cnt = 0;
        uint16_t saved_tr = mem->temp_root_count;
        uint16_t common_tid = 0;
        bool     mixed_types = false;

        while (*pos < len && !mem->has_error) {
            if (cnt >= 256u) {
                mem_pop_temp_roots(mem, saved_tr);
                mem_err(mem, "JSON array exceeds 256 elements");
                return val_void();
            }
            items[cnt] = json_parse_val_rec(mem, tab, s, len, pos, (uint8_t)(depth + 1));
            if (mem->has_error) {
                mem_pop_temp_roots(mem, saved_tr);
                return val_void();
            }
            (void)mem_push_temp_root(mem, &items[cnt]);
            uint16_t it_tid = val_get_type(mem, items[cnt]);
            if (cnt == 0) {
                common_tid = it_tid;
            } else if (it_tid != common_tid) {
                mixed_types = true;
            }
            cnt++;

            json_skip_ws(s, len, pos);
            if (*pos < len && s[*pos] == ',') {
                (*pos)++;
                continue;
            }
            if (*pos < len && s[*pos] == ']') {
                (*pos)++;
                uint16_t elem_tid = mixed_types ? (uint16_t)SYM_TYPE_ANY : common_tid;
                uint16_t stride   = (elem_tid != SYM_TYPE_ANY && elem_tid > 0 && elem_tid < tab->type_count)
                                    ? tab->types[elem_tid].flat_size : 1u;
                uint16_t hdr_idx  = mem_vec_alloc_hdr(mem, elem_tid, cnt);
                if (mem->has_error || !mem_vec_reserve(mem, hdr_idx, stride, cnt)) {
                    mem_pop_temp_roots(mem, saved_tr);
                    return val_void();
                }
                uint16_t data_idx = (uint16_t)mem_read_pool(mem, (uint16_t)(hdr_idx + 3));
                for (uint16_t i = 0; i < cnt; ++i) {
                    delegate_vec_write_elem(mem, tab, elem_tid, data_idx, (uint32_t)i * (uint32_t)stride, items[i]);
                }
                mem_write_pool(mem, (uint16_t)(hdr_idx + 1), (double)cnt);
                mem_pop_temp_roots(mem, saved_tr);
                return val_make_comp(mem, mem->vec_type_id, hdr_idx, MEM_VEC_HDR_SLOTS);
            }
            mem_pop_temp_roots(mem, saved_tr);
            mem_err(mem, "JSON parse error: expected ',' or ']' in array at offset %u", (unsigned)*pos);
            return val_void();
        }
        mem_pop_temp_roots(mem, saved_tr);
        mem_err(mem, "JSON parse error: unterminated array '['");
        return val_void();
    }

    /* 4. Booleans & null */
    if (*pos + 4u <= len && memcmp(s + *pos, "true", 4) == 0) {
        (*pos) += 4u;
        return val_make_f64(mem, mem->fast_real_type, 1.0);
    }
    if (*pos + 5u <= len && memcmp(s + *pos, "false", 5) == 0) {
        (*pos) += 5u;
        return val_make_f64(mem, mem->fast_real_type, 0.0);
    }
    if (*pos + 4u <= len && memcmp(s + *pos, "null", 4) == 0) {
        (*pos) += 4u;
        return val_make_f64(mem, mem->fast_real_type, (double)NAN);
    }

    /* 5. Number */
    if (c == '-' || (c >= '0' && c <= '9')) {
        char *endp = NULL;
        double d = strtod(s + *pos, &endp);
        if (endp && endp > s + *pos) {
            *pos = (size_t)(endp - s);
            return val_make_f64(mem, mem->fast_real_type, d);
        }
    }

    mem_err(mem, "JSON parse error: unexpected token near '%.12s' at offset %u",
            s + *pos, (unsigned)*pos);
    return val_void();
}

Value eval_json_parse_op(MemArena *mem, SymTable *tab, Value a) {
    if (mem->str_type_id == 0 || val_get_type(mem, a) != mem->str_type_id) {
        mem_err(mem, "json_parse() expects a JSON string ($str)");
        return val_void();
    }
    StrSlice sl = val_get_str(mem, a);
    char *copy = (char *)malloc((size_t)sl.len + 1u);
    if (!copy) {
        mem_err(mem, "Out of memory in json_parse()");
        return val_void();
    }
    if (sl.len > 0) memcpy(copy, mem_str_ptr(mem, sl), sl.len);
    copy[sl.len] = '\0';

    size_t pos = 0;
    Value res = json_parse_val_rec(mem, tab, copy, sl.len, &pos, 0);
    if (!mem->has_error) {
        json_skip_ws(copy, sl.len, &pos);
        if (pos < sl.len) {
            mem_err(mem, "JSON parse error: trailing characters at offset %u", (unsigned)pos);
            free(copy);
            return val_void();
        }
    }
    free(copy);
    return res;
}

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} JsonBuf;

static void jbuf_append_raw(JsonBuf *jb, const char *s, size_t n) {
    if (!jb->buf || n == 0) return;
    if (jb->len + n + 1u > jb->cap) {
        size_t new_cap = jb->cap * 2u;
        while (new_cap < jb->len + n + 1u) new_cap *= 2u;
        if (new_cap > 28000u) return;
        char *nb = (char *)realloc(jb->buf, new_cap);
        if (!nb) return;
        jb->buf = nb;
        jb->cap = new_cap;
    }
    memcpy(jb->buf + jb->len, s, n);
    jb->len += n;
    jb->buf[jb->len] = '\0';
}

static void jbuf_append_cstr(JsonBuf *jb, const char *s) {
    jbuf_append_raw(jb, s, strlen(s));
}

static void jbuf_append_indent(JsonBuf *jb, int spaces) {
    for (int i = 0; i < spaces; ++i) jbuf_append_raw(jb, " ", 1u);
}

static void jbuf_append_escaped_str(JsonBuf *jb, const char *s, uint16_t slen) {
    jbuf_append_raw(jb, "\"", 1u);
    for (uint16_t i = 0; i < slen; ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  jbuf_append_raw(jb, "\\\"", 2u); break;
            case '\\': jbuf_append_raw(jb, "\\\\", 2u); break;
            case '\b': jbuf_append_raw(jb, "\\b", 2u);  break;
            case '\f': jbuf_append_raw(jb, "\\f", 2u);  break;
            case '\n': jbuf_append_raw(jb, "\\n", 2u);  break;
            case '\r': jbuf_append_raw(jb, "\\r", 2u);  break;
            case '\t': jbuf_append_raw(jb, "\\t", 2u);  break;
            default:
                if (c < 0x20u) {
                    char uesc[8];
                    snprintf(uesc, sizeof(uesc), "\\u%04x", (unsigned)c);
                    jbuf_append_raw(jb, uesc, 6u);
                } else {
                    char ch = (char)c;
                    jbuf_append_raw(jb, &ch, 1u);
                }
                break;
        }
    }
    jbuf_append_raw(jb, "\"", 1u);
}

static void json_format_val_rec(MemArena *mem, const SymTable *tab, Value v,
                                int indent_step, int cur_indent, JsonBuf *jb, uint8_t depth) {
    if (depth > 20u) {
        jbuf_append_cstr(jb, "null");
        return;
    }
    uint16_t tid = val_get_type(mem, v);
    if (tid == MEM_TYPE_VOID) {
        jbuf_append_cstr(jb, "null");
        return;
    }
    if (tid == mem->str_type_id && tid != 0) {
        StrSlice s = val_get_str(mem, v);
        jbuf_append_escaped_str(jb, mem_str_ptr(mem, s), s.len);
        return;
    }
    if (tid == mem->vec_type_id && tid != 0) {
        uint16_t hdr_idx  = val_get_pool_idx(v);
        uint16_t raw_hdr0 = (uint16_t)mem_read_pool(mem, (uint16_t)(hdr_idx + 0));
        uint16_t elem_tid = (uint16_t)(raw_hdr0 & 0xFFu);
        uint8_t  elem_uid = (uint8_t)((raw_hdr0 >> 8u) & 0x7Fu);
        uint32_t len      = (uint32_t)mem_read_pool(mem, (uint16_t)(hdr_idx + 1));
        uint16_t data_idx = (uint16_t)mem_read_pool(mem, (uint16_t)(hdr_idx + 3));
        uint16_t stride   = (elem_tid != SYM_TYPE_ANY && elem_tid > 0 && elem_tid < tab->type_count)
                            ? tab->types[elem_tid].flat_size : 1u;
        if (len == 0) {
            jbuf_append_cstr(jb, "[]");
            return;
        }
        jbuf_append_cstr(jb, "[");
        for (uint32_t i = 0; i < len; ++i) {
            if (i > 0) jbuf_append_cstr(jb, ",");
            if (indent_step > 0) {
                jbuf_append_cstr(jb, "\n");
                jbuf_append_indent(jb, cur_indent + indent_step);
            }
            Value ev = delegate_vec_read_elem(mem, tab, elem_tid, elem_uid, data_idx, i * (uint32_t)stride);
            json_format_val_rec(mem, tab, ev, indent_step, cur_indent + indent_step, jb, (uint8_t)(depth + 1));
        }
        if (indent_step > 0) {
            jbuf_append_cstr(jb, "\n");
            jbuf_append_indent(jb, cur_indent);
        }
        jbuf_append_cstr(jb, "]");
        return;
    }
    if (tid == mem->map_type_id && tid != 0) {
        uint16_t hdr_idx  = val_get_pool_idx(v);
        uint16_t len      = (uint16_t)mem_read_pool(mem, (uint16_t)(hdr_idx + 1));
        uint16_t data_idx = (uint16_t)mem_read_pool(mem, (uint16_t)(hdr_idx + 3));
        if (len == 0) {
            jbuf_append_cstr(jb, "{}");
            return;
        }
        jbuf_append_cstr(jb, "{");
        for (uint16_t i = 0; i < len; ++i) {
            if (i > 0) jbuf_append_cstr(jb, ",");
            if (indent_step > 0) {
                jbuf_append_cstr(jb, "\n");
                jbuf_append_indent(jb, cur_indent + indent_step);
            }
            StrSlice ks = mem_map_entry_key(mem, data_idx, i);
            Value    ev = mem_map_entry_val(mem, data_idx, i);
            jbuf_append_escaped_str(jb, mem_str_ptr(mem, ks), ks.len);
            jbuf_append_cstr(jb, indent_step > 0 ? ": " : ":");
            json_format_val_rec(mem, tab, ev, indent_step, cur_indent + indent_step, jb, (uint8_t)(depth + 1));
        }
        if (indent_step > 0) {
            jbuf_append_cstr(jb, "\n");
            jbuf_append_indent(jb, cur_indent);
        }
        jbuf_append_cstr(jb, "}");
        return;
    }
    if (tid == mem->mat_type_id && tid != 0) {
        uint8_t  rank = 0;
        uint16_t tot_len = 0, data_idx = 0, dims[4] = { 0 };
        delegate_mat_get_info(mem, v, &rank, &tot_len, &data_idx, dims);
        if (rank <= 1) {
            jbuf_append_cstr(jb, "[");
            for (uint16_t i = 0; i < tot_len; ++i) {
                if (i > 0) jbuf_append_cstr(jb, ",");
                char nbuf[48];
                snprintf(nbuf, sizeof(nbuf), "%.15g", mem_data_read(mem, data_idx, i));
                jbuf_append_cstr(jb, nbuf);
            }
            jbuf_append_cstr(jb, "]");
        } else {
            uint16_t rows = dims[0];
            uint16_t cols = dims[1];
            jbuf_append_cstr(jb, "[");
            for (uint16_t r = 0; r < rows; ++r) {
                if (r > 0) jbuf_append_cstr(jb, ",");
                jbuf_append_cstr(jb, "[");
                for (uint16_t c = 0; c < cols; ++c) {
                    if (c > 0) jbuf_append_cstr(jb, ",");
                    char nbuf[48];
                    snprintf(nbuf, sizeof(nbuf), "%.15g", mem_data_read(mem, data_idx, (uint32_t)r * cols + c));
                    jbuf_append_cstr(jb, nbuf);
                }
                jbuf_append_cstr(jb, "]");
            }
            jbuf_append_cstr(jb, "]");
        }
        return;
    }
    if (tid > 0 && tid < tab->type_count && !tab->types[tid].is_consuming) {
        const TypeDef *td = &tab->types[tid];
        uint16_t pidx = val_get_pool_idx(v);
        /* Regular $struct -> JSON object {"field": val, ...} */
        jbuf_append_cstr(jb, "{");
        for (uint8_t i = 0; i < td->field_count; ++i) {
            if (i > 0) jbuf_append_cstr(jb, ",");
            if (indent_step > 0) {
                jbuf_append_cstr(jb, "\n");
                jbuf_append_indent(jb, cur_indent + indent_step);
            }
            const FieldDef *fd = &tab->fields[td->start_idx + i];
            uint16_t ftid = sym_unpack_outer_type(mem, fd->type_id);
            jbuf_append_escaped_str(jb, mem_str_ptr(mem, fd->name), fd->name.len);
            jbuf_append_cstr(jb, indent_step > 0 ? ": " : ":");
            uint16_t fpidx = (uint16_t)(pidx + fd->flat_offset);
            Value fv;
            if (ftid == mem->str_type_id && mem->str_type_id != 0) {
                uint32_t pk = (uint32_t)mem_read_pool(mem, fpidx);
                StrSlice fs = { (uint16_t)(pk >> 16u), (uint16_t)(pk & 0xFFFFu) };
                fv = val_make_str(mem, ftid, fs);
            } else if (!tab->types[ftid].is_consuming) {
                fv = val_make_comp(mem, ftid, fpidx, tab->types[ftid].flat_size);
            } else {
                fv = val_make_f64(mem, ftid, mem_read_pool(mem, fpidx));
            }
            json_format_val_rec(mem, tab, fv, indent_step, cur_indent + indent_step, jb, (uint8_t)(depth + 1));
        }
        if (indent_step > 0 && td->field_count > 0) {
            jbuf_append_cstr(jb, "\n");
            jbuf_append_indent(jb, cur_indent);
        }
        jbuf_append_cstr(jb, "}");
        return;
    }

    double d = val_get_f64(mem, v);
    if (!isfinite(d)) {
        jbuf_append_cstr(jb, "null");
    } else {
        char nbuf[48];
        snprintf(nbuf, sizeof(nbuf), "%.15g", d);
        jbuf_append_cstr(jb, nbuf);
    }
}

Value eval_json_stringify_op(MemArena *mem, const SymTable *tab, Value a, Value b) {
    int indent_step = 0;
    if (val_get_type(mem, b) != MEM_TYPE_VOID) {
        double id = val_get_f64(mem, b);
        if (isfinite(id) && id > 0.0) {
            indent_step = (id > 8.0) ? 8 : (int)id;
        }
    }
    JsonBuf jb;
    jb.cap = 512u;
    jb.len = 0u;
    jb.buf = (char *)malloc(jb.cap);
    if (!jb.buf) {
        mem_err(mem, "Out of memory in json_stringify()");
        return val_void();
    }
    jb.buf[0] = '\0';
    json_format_val_rec(mem, tab, a, indent_step, 0, &jb, 0);
    StrSlice s = mem_intern_str(mem, jb.buf, (uint16_t)jb.len);
    free(jb.buf);
    if (mem->has_error) return val_void();
    return val_make_str(mem, mem->str_type_id, s);
}
