// SPDX-License-Identifier: GPL-3.0-or-later
// HomeKit Data Stream message encoding (Apple's OPACK format), as decoded by HAP-NodeJS
// src/lib/datastream/DataStreamParser.ts (Apache-2.0, Copyright The HAP-NodeJS contributors).
#ifndef RHKD_OPACK_H
#define RHKD_OPACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------------------------------------------------
// Decoding into a small tree. Strings and data point into the source buffer (not NUL-terminated).

typedef enum { OP_NULL, OP_BOOL, OP_INT, OP_FLOAT, OP_STR, OP_DATA, OP_ARRAY, OP_DICT } op_type_t;

typedef struct op_val {
    op_type_t type;
    int64_t i;            // OP_BOOL (0/1), OP_INT
    double f;             // OP_FLOAT
    const uint8_t* p;     // OP_STR, OP_DATA
    size_t n;             // OP_STR/OP_DATA bytes; OP_ARRAY elements; OP_DICT entries
    struct op_val* child; // OP_ARRAY: first element; OP_DICT: first key (key, value, key, value, ...)
    struct op_val* next;
} op_val_t;

#define OP_MAX_NODES   256
#define OP_MAX_TRACKED 64

typedef struct {
    op_val_t nodes[OP_MAX_NODES];
    size_t used;
    op_val_t* tracked[OP_MAX_TRACKED]; // back-reference targets (0xA0 + index)
    size_t numTracked;
} op_arena_t;

// Decodes one value from @buf. Returns NULL on malformed input or when the arena is full.
op_val_t* op_decode(op_arena_t* a, const uint8_t* buf, size_t len, size_t* consumed);

// Dictionary lookup by string key; NULL if absent.
const op_val_t* op_get(const op_val_t* dict, const char* key);
bool op_is_str(const op_val_t* v, const char* s);
// Copies a string value into @out (NUL-terminated, truncated). Returns false if @v is not a string.
bool op_str(const op_val_t* v, char* out, size_t cap);

// ---------------------------------------------------------------------------------------------------------------------
// Encoding. No back-references are written (optional in the format).

typedef struct {
    uint8_t* b;
    size_t cap, len;
    bool overflow;
} op_writer_t;

void op_w_init(op_writer_t* w, uint8_t* buf, size_t cap);
void op_w_dict(op_writer_t* w, unsigned entries); // entries <= 14; follow with key, value pairs
void op_w_array(op_writer_t* w, unsigned elements); // elements <= 12
void op_w_str(op_writer_t* w, const char* s);
void op_w_data(op_writer_t* w, const uint8_t* p, size_t n);
void op_w_int(op_writer_t* w, int64_t v);   // shortest encoding
void op_w_int64(op_writer_t* w, int64_t v); // always the 8-byte form (HAP-NodeJS uses it for id/status)
void op_w_bool(op_writer_t* w, bool v);
void op_w_null(op_writer_t* w);

#endif
