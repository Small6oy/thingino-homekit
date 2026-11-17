// SPDX-License-Identifier: Apache-2.0
#include "tlvb.h"
#include <string.h>

void tlvb_init(tlvb_t* b, uint8_t* buf, size_t cap) {
    b->buf = buf;
    b->len = 0;
    b->cap = cap;
    b->overflow = false;
}

static void put_raw(tlvb_t* b, uint8_t type, const uint8_t* d, size_t n) {
    if (b->len + 2 + n > b->cap) {
        b->overflow = true;
        return;
    }
    b->buf[b->len++] = type;
    b->buf[b->len++] = (uint8_t) n;
    if (n)
        memcpy(b->buf + b->len, d, n);
    b->len += n;
}

void tlvb_put(tlvb_t* b, uint8_t type, const void* data, size_t n) {
    const uint8_t* d = data;
    if (n == 0) {
        put_raw(b, type, NULL, 0);
        return;
    }
    while (n > 0) {
        size_t c = n > 255 ? 255 : n;
        put_raw(b, type, d, c);
        d += c;
        n -= c;
    }
}

void tlvb_u8(tlvb_t* b, uint8_t type, uint8_t v) {
    tlvb_put(b, type, &v, 1);
}
void tlvb_u16(tlvb_t* b, uint8_t type, uint16_t v) {
    uint8_t x[2] = { (uint8_t) v, (uint8_t)(v >> 8) };
    tlvb_put(b, type, x, 2);
}
void tlvb_u32(tlvb_t* b, uint8_t type, uint32_t v) {
    uint8_t x[4] = { (uint8_t) v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    tlvb_put(b, type, x, 4);
}
void tlvb_sep(tlvb_t* b) {
    put_raw(b, 0, NULL, 0);
}

bool tlv_next(const uint8_t* buf, size_t len, size_t* pos, uint8_t* type, uint8_t* val, size_t cap, size_t* valLen) {
    size_t p = *pos;
    if (p + 2 > len)
        return false;
    uint8_t t = buf[p];
    size_t out = 0;
    bool first = true;
    // Merge fragments: consecutive items of the same type where the previous one was 255 bytes long.
    for (;;) {
        if (p + 2 > len)
            break;
        if (!first && buf[p] != t)
            break;
        size_t n = buf[p + 1];
        if (p + 2 + n > len)
            return false;
        if (out + n > cap)
            return false;
        memcpy(val + out, buf + p + 2, n);
        out += n;
        p += 2 + n;
        first = false;
        if (n != 255)
            break;
    }
    *type = t;
    *valLen = out;
    *pos = p;
    return true;
}

bool tlv_get(const uint8_t* buf, size_t len, uint8_t type, void* out, size_t cap, size_t* outLen) {
    size_t pos = 0;
    uint8_t t;
    static uint8_t tmp[2048];
    size_t n;
    while (tlv_next(buf, len, &pos, &t, tmp, sizeof tmp, &n)) {
        if (t == type) {
            if (n > cap)
                return false;
            memcpy(out, tmp, n);
            if (outLen)
                *outLen = n;
            return true;
        }
    }
    return false;
}

bool tlv_get_u8(const uint8_t* buf, size_t len, uint8_t type, uint8_t* v) {
    uint8_t x[8];
    size_t n;
    if (!tlv_get(buf, len, type, x, sizeof x, &n) || n < 1)
        return false;
    *v = x[0];
    return true;
}
bool tlv_get_u16(const uint8_t* buf, size_t len, uint8_t type, uint16_t* v) {
    uint8_t x[8];
    size_t n;
    if (!tlv_get(buf, len, type, x, sizeof x, &n) || n < 1)
        return false;
    *v = n >= 2 ? (uint16_t)(x[0] | (x[1] << 8)) : x[0];
    return true;
}
bool tlv_get_u32(const uint8_t* buf, size_t len, uint8_t type, uint32_t* v) {
    uint8_t x[8];
    size_t n;
    if (!tlv_get(buf, len, type, x, sizeof x, &n) || n < 1)
        return false;
    uint32_t r = 0;
    for (size_t i = 0; i < n && i < 4; i++)
        r |= (uint32_t) x[i] << (8 * i);
    *v = r;
    return true;
}
