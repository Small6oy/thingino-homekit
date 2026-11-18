// SPDX-License-Identifier: GPL-3.0-or-later
// HomeKit Data Stream message encoding (OPACK). Tag values and the back-reference rule follow HAP-NodeJS
// src/lib/datastream/DataStreamParser.ts (Apache-2.0, Copyright The HAP-NodeJS contributors): every decoded
// value whose encoding is longer than the tag byte is remembered, and 0xA0 + index refers back to it.
#include "opack.h"

#include <string.h>

enum {
    T_TRUE = 0x01, T_FALSE = 0x02, T_TERMINATOR = 0x03, T_NULL = 0x04, T_UUID = 0x05, T_DATE = 0x06,
    T_MINUS_ONE = 0x07, T_INT_0 = 0x08, T_INT_39 = 0x2E,
    T_INT8 = 0x30, T_INT16 = 0x31, T_INT32 = 0x32, T_INT64 = 0x33, T_FLOAT32 = 0x35, T_FLOAT64 = 0x36,
    T_STR_0 = 0x40, T_STR_32 = 0x60, T_STR_L8 = 0x61, T_STR_L16 = 0x62, T_STR_L32 = 0x63, T_STR_L64 = 0x64,
    T_STR_NUL = 0x6F,
    T_DATA_0 = 0x70, T_DATA_32 = 0x90, T_DATA_L8 = 0x91, T_DATA_L16 = 0x92, T_DATA_L32 = 0x93, T_DATA_L64 = 0x94,
    T_REF_0 = 0xA0, T_REF_LAST_DIRECT = 0xC0,
    T_ARRAY_0 = 0xD0, T_ARRAY_14 = 0xDE, T_ARRAY_TERM = 0xDF,
    T_DICT_0 = 0xE0, T_DICT_14 = 0xEE, T_DICT_TERM = 0xEF,
};

// ---------------------------------------------------------------------------------------------------------------------
// Decoder

typedef struct {
    op_arena_t* a;
    const uint8_t* b;
    size_t len, pos;
    int depth;
} rd_t;

static uint64_t le(const uint8_t* p, int n) {
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

static op_val_t* node(rd_t* r, op_type_t t) {
    if (r->a->used >= OP_MAX_NODES)
        return NULL;
    op_val_t* v = &r->a->nodes[r->a->used++];
    memset(v, 0, sizeof *v);
    v->type = t;
    return v;
}

static op_val_t* track(rd_t* r, op_val_t* v) {
    if (v && r->a->numTracked < OP_MAX_TRACKED)
        r->a->tracked[r->a->numTracked++] = v;
    return v;
}

static bool need(rd_t* r, size_t n) {
    return r->pos + n <= r->len;
}

static op_val_t* bytes_val(rd_t* r, op_type_t t, size_t n) {
    if (!need(r, n))
        return NULL;
    op_val_t* v = node(r, t);
    if (!v)
        return NULL;
    v->p = r->b + r->pos;
    v->n = n;
    r->pos += n;
    return v;
}

static op_val_t* decode(rd_t* r, bool* isTerminator);

// Reads container children until @count (or the terminator when count < 0).
static op_val_t* container(rd_t* r, op_type_t t, long count, int perEntry) {
    op_val_t* c = node(r, t);
    if (!c || ++r->depth > 16)
        return NULL;
    op_val_t** tail = &c->child;
    for (long i = 0; count < 0 || i < count * perEntry; i++) {
        bool term = false;
        op_val_t* e = decode(r, &term);
        if (term) {
            if (count >= 0 || (i % perEntry) != 0)
                return NULL;
            break;
        }
        if (!e)
            return NULL; // every decoded value is a fresh node (references copy their target), so linking is safe
        *tail = e;
        tail = &e->next;
        if ((i % perEntry) == perEntry - 1)
            c->n++;
    }
    r->depth--;
    return c;
}

static op_val_t* decode(rd_t* r, bool* isTerminator) {
    if (!need(r, 1))
        return NULL;
    uint8_t tag = r->b[r->pos++];
    op_val_t* v;
    if (tag == T_TRUE || tag == T_FALSE) {
        if ((v = node(r, OP_BOOL)))
            v->i = tag == T_TRUE;
        return v;
    }
    if (tag == T_TERMINATOR) {
        *isTerminator = true;
        return NULL;
    }
    if (tag == T_NULL)
        return node(r, OP_NULL);
    if (tag == T_UUID)
        return track(r, bytes_val(r, OP_DATA, 16));
    if (tag == T_DATE || tag == T_FLOAT64 || tag == T_FLOAT32) {
        size_t n = tag == T_FLOAT32 ? 4 : 8;
        if (!need(r, n) || !(v = node(r, OP_FLOAT)))
            return NULL;
        if (n == 4) {
            uint32_t u = (uint32_t) le(r->b + r->pos, 4);
            float f;
            memcpy(&f, &u, 4);
            v->f = f;
        } else {
            uint64_t u = le(r->b + r->pos, 8);
            memcpy(&v->f, &u, 8);
        }
        r->pos += n;
        return track(r, v);
    }
    if (tag == T_MINUS_ONE || (tag >= T_INT_0 && tag <= T_INT_39)) {
        if ((v = node(r, OP_INT)))
            v->i = tag == T_MINUS_ONE ? -1 : tag - T_INT_0;
        return v;
    }
    if (tag >= T_INT8 && tag <= T_INT64) {
        int n = 1 << (tag - T_INT8);
        if (!need(r, (size_t) n) || !(v = node(r, OP_INT)))
            return NULL;
        uint64_t u = le(r->b + r->pos, n);
        if (n < 8 && (u >> (n * 8 - 1)) & 1)
            u |= ~0ULL << (n * 8); // sign-extend
        v->i = (int64_t) u;
        r->pos += (size_t) n;
        return track(r, v);
    }
    if (tag >= T_STR_0 && tag <= T_STR_32) {
        if (tag == T_STR_0) {
            if ((v = node(r, OP_STR)))
                v->p = r->b + r->pos;
            return v;
        }
        return track(r, bytes_val(r, OP_STR, tag - T_STR_0));
    }
    if (tag >= T_DATA_0 && tag <= T_DATA_32) {
        if (tag == T_DATA_0) {
            if ((v = node(r, OP_DATA)))
                v->p = r->b + r->pos;
            return v;
        }
        return track(r, bytes_val(r, OP_DATA, tag - T_DATA_0));
    }
    if ((tag >= T_STR_L8 && tag <= T_STR_L64) || (tag >= T_DATA_L8 && tag <= T_DATA_L64)) {
        bool str = tag <= T_STR_L64;
        int n = 1 << (tag - (str ? T_STR_L8 : T_DATA_L8));
        if (!need(r, (size_t) n))
            return NULL;
        uint64_t l = le(r->b + r->pos, n);
        r->pos += (size_t) n;
        if (l > r->len)
            return NULL;
        return track(r, bytes_val(r, str ? OP_STR : OP_DATA, (size_t) l));
    }
    if (tag == T_STR_NUL) {
        const uint8_t* z = memchr(r->b + r->pos, 0, r->len - r->pos);
        if (!z)
            return NULL;
        size_t n = (size_t) (z - (r->b + r->pos));
        v = bytes_val(r, OP_STR, n);
        r->pos++; // NUL
        return track(r, v);
    }
    if (tag >= T_REF_0 && tag <= T_REF_LAST_DIRECT) {
        size_t idx = tag - T_REF_0;
        if (idx >= r->a->numTracked)
            return NULL;
        op_val_t* src = r->a->tracked[idx];
        if (!(v = node(r, src->type)))
            return NULL;
        *v = *src;
        v->next = NULL;
        return v; // a reference is not itself tracked
    }
    if (tag >= T_ARRAY_0 && tag <= T_ARRAY_14)
        return container(r, OP_ARRAY, tag - T_ARRAY_0, 1);
    if (tag == T_ARRAY_TERM)
        return container(r, OP_ARRAY, -1, 1);
    if (tag >= T_DICT_0 && tag <= T_DICT_14)
        return container(r, OP_DICT, tag - T_DICT_0, 2);
    if (tag == T_DICT_TERM)
        return container(r, OP_DICT, -1, 2);
    return NULL; // 0x00, unsupported (0xC1-0xCF long references, 0x37-0x3F, ...)
}

op_val_t* op_decode(op_arena_t* a, const uint8_t* buf, size_t len, size_t* consumed) {
    rd_t r = { .a = a, .b = buf, .len = len };
    bool term = false;
    op_val_t* v = decode(&r, &term);
    if (consumed)
        *consumed = r.pos;
    return term ? NULL : v;
}

const op_val_t* op_get(const op_val_t* dict, const char* key) {
    if (!dict || dict->type != OP_DICT)
        return NULL;
    for (const op_val_t* k = dict->child; k && k->next; k = k->next->next)
        if (op_is_str(k, key))
            return k->next;
    return NULL;
}

bool op_is_str(const op_val_t* v, const char* s) {
    size_t n = strlen(s);
    return v && v->type == OP_STR && v->n == n && memcmp(v->p, s, n) == 0;
}

bool op_str(const op_val_t* v, char* out, size_t cap) {
    if (!v || v->type != OP_STR || !cap)
        return false;
    size_t n = v->n < cap - 1 ? v->n : cap - 1;
    memcpy(out, v->p, n);
    out[n] = 0;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Encoder

void op_w_init(op_writer_t* w, uint8_t* buf, size_t cap) {
    w->b = buf;
    w->cap = cap;
    w->len = 0;
    w->overflow = false;
}

static void put(op_writer_t* w, const void* p, size_t n) {
    if (w->overflow || w->len + n > w->cap) {
        w->overflow = true;
        return;
    }
    memcpy(w->b + w->len, p, n);
    w->len += n;
}

static void put8(op_writer_t* w, uint8_t v) {
    put(w, &v, 1);
}

static void putle(op_writer_t* w, uint64_t v, int n) {
    uint8_t b[8];
    for (int i = 0; i < n; i++)
        b[i] = (uint8_t) (v >> (8 * i));
    put(w, b, (size_t) n);
}

void op_w_dict(op_writer_t* w, unsigned entries) {
    if (entries > 14)
        w->overflow = true;
    put8(w, (uint8_t) (T_DICT_0 + entries));
}

void op_w_array(op_writer_t* w, unsigned elements) {
    if (elements > 12)
        w->overflow = true;
    put8(w, (uint8_t) (T_ARRAY_0 + elements));
}

static void sized(op_writer_t* w, uint8_t small0, uint8_t l8, const uint8_t* p, size_t n) {
    if (n <= 32) {
        put8(w, (uint8_t) (small0 + n));
    } else if (n <= 0xFF) {
        put8(w, l8);
        putle(w, n, 1);
    } else if (n <= 0xFFFF) {
        put8(w, (uint8_t) (l8 + 1));
        putle(w, n, 2);
    } else {
        put8(w, (uint8_t) (l8 + 2));
        putle(w, n, 4);
    }
    put(w, p, n);
}

void op_w_str(op_writer_t* w, const char* s) {
    sized(w, T_STR_0, T_STR_L8, (const uint8_t*) s, strlen(s));
}

void op_w_data(op_writer_t* w, const uint8_t* p, size_t n) {
    sized(w, T_DATA_0, T_DATA_L8, p, n);
}

void op_w_int(op_writer_t* w, int64_t v) {
    if (v == -1) {
        put8(w, T_MINUS_ONE);
    } else if (v >= 0 && v <= 39) {
        put8(w, (uint8_t) (T_INT_0 + v));
    } else if (v >= -128 && v <= 127) {
        put8(w, T_INT8);
        putle(w, (uint64_t) v, 1);
    } else if (v >= -32768 && v <= 32767) {
        put8(w, T_INT16);
        putle(w, (uint64_t) v, 2);
    } else if (v >= INT32_MIN && v <= INT32_MAX) {
        put8(w, T_INT32);
        putle(w, (uint64_t) v, 4);
    } else {
        op_w_int64(w, v);
    }
}

void op_w_int64(op_writer_t* w, int64_t v) {
    put8(w, T_INT64);
    putle(w, (uint64_t) v, 8);
}

void op_w_bool(op_writer_t* w, bool v) {
    put8(w, v ? T_TRUE : T_FALSE);
}

void op_w_null(op_writer_t* w) {
    put8(w, T_NULL);
}
