// SPDX-License-Identifier: GPL-3.0-or-later
// Decodes a SelectedRTPStreamConfiguration write into one readable log line (see rtpdump.h).
// TLV layout from HAP-NodeJS src/lib/camera/RTPStreamManagement.ts (Apache-2.0, Copyright The HAP-NodeJS
// contributors).
#include "rtpdump.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tlvb.h"

static const HAPLogObject logObject = { .subsystem = "rhkd", .category = "SelectedRTP" };

typedef enum { kU, kHex, kF32, kUUID, kBox } kind_t;

typedef struct field {
    uint8_t type;
    const char* name;
    kind_t kind;
    const struct field* sub; // kBox: its fields, ended by a NULL name
} field_t;

static const field_t kSession[] = { { 0x01, "id", kUUID, NULL }, { 0x02, "cmd", kU, NULL }, { 0, NULL, kU, NULL } };
static const field_t kVideoCodec[] = { { 0x01, "profile", kU, NULL },
                                       { 0x02, "level", kU, NULL },
                                       { 0x03, "packetization", kU, NULL },
                                       { 0x04, "cvo", kU, NULL },
                                       { 0x05, "cvoId", kU, NULL },
                                       { 0, NULL, kU, NULL } };
static const field_t kVideoAttrs[] = {
    { 0x01, "w", kU, NULL }, { 0x02, "h", kU, NULL }, { 0x03, "fps", kU, NULL }, { 0, NULL, kU, NULL }
};
static const field_t kVideoRtp[] = { { 0x01, "pt", kU, NULL },
                                     { 0x02, "ssrc", kHex, NULL },
                                     { 0x03, "maxKbps", kU, NULL },
                                     { 0x04, "rtcpS", kF32, NULL },
                                     { 0x05, "mtu", kU, NULL },
                                     { 0, NULL, kU, NULL } };
static const field_t kVideo[] = { { 0x01, "codec", kU, NULL },
                                  { 0x02, "params", kBox, kVideoCodec },
                                  { 0x03, "attrs", kBox, kVideoAttrs },
                                  { 0x04, "rtp", kBox, kVideoRtp },
                                  { 0, NULL, kU, NULL } };
static const field_t kAudioCodec[] = { { 0x01, "channels", kU, NULL },
                                       { 0x02, "vbr", kU, NULL },
                                       { 0x03, "rate", kU, NULL },
                                       { 0x04, "ptimeMs", kU, NULL },
                                       { 0, NULL, kU, NULL } };
static const field_t kAudioRtp[] = { { 0x01, "pt", kU, NULL },
                                     { 0x02, "ssrc", kHex, NULL },
                                     { 0x03, "maxKbps", kU, NULL },
                                     { 0x04, "rtcpS", kF32, NULL },
                                     { 0x06, "cnPt", kU, NULL },
                                     { 0, NULL, kU, NULL } };
static const field_t kAudio[] = { { 0x01, "codec", kU, NULL },
                                  { 0x02, "params", kBox, kAudioCodec },
                                  { 0x03, "rtp", kBox, kAudioRtp },
                                  { 0x04, "cn", kU, NULL },
                                  { 0, NULL, kU, NULL } };
static const field_t kTop[] = { { 0x01, "session", kBox, kSession },
                                { 0x02, "video", kBox, kVideo },
                                { 0x03, "audio", kBox, kAudio },
                                { 0, NULL, kU, NULL } };

typedef struct {
    char* s;
    size_t cap, len;
    unsigned unknown;
} out_t;

static void Put(out_t* o, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void Put(out_t* o, const char* fmt, ...) {
    if (o->len >= o->cap)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->s + o->len, o->cap - o->len, fmt, ap);
    va_end(ap);
    if (n > 0)
        o->len = o->len + (size_t) n < o->cap ? o->len + (size_t) n : o->cap - 1;
}

static void PutHex(out_t* o, const uint8_t* v, size_t n) {
    for (size_t i = 0; i < n && i < 32; i++)
        Put(o, "%02x", v[i]);
    if (n > 32)
        Put(o, "..(%zu)", n);
}

static void Walk(out_t* o, const uint8_t* buf, size_t len, const field_t* fields) {
    uint8_t v[256];
    size_t pos = 0, n;
    uint8_t t;
    bool first = true;
    while (tlv_next(buf, len, &pos, &t, v, sizeof v, &n)) {
        if (!first)
            Put(o, " ");
        first = false;
        const field_t* f = fields;
        while (f->name && f->type != t)
            f++;
        if (!f->name) { // not in HAP-NodeJS's layout: worth knowing about
            o->unknown++;
            Put(o, "?%02x=", t);
            PutHex(o, v, n);
            continue;
        }
        switch (f->kind) {
            case kBox:
                Put(o, "%s{", f->name);
                Walk(o, v, n, f->sub);
                Put(o, "}");
                break;
            case kF32: {
                float x = 0;
                if (n == 4) {
                    uint32_t u = (uint32_t) v[0] | (uint32_t) v[1] << 8 | (uint32_t) v[2] << 16 | (uint32_t) v[3] << 24;
                    memcpy(&x, &u, 4);
                    Put(o, "%s=%g", f->name, (double) x);
                } else {
                    Put(o, "%s=?", f->name);
                    PutHex(o, v, n);
                }
                break;
            }
            case kUUID:
                Put(o, "%s=", f->name);
                PutHex(o, v, n > 4 ? 4 : n); // enough to tell sessions apart
                break;
            case kHex: {
                uint32_t u = 0;
                for (size_t i = 0; i < n && i < 4; i++)
                    u |= (uint32_t) v[i] << (8 * i);
                Put(o, "%s=0x%08x", f->name, u);
                break;
            }
            default: {
                if (n > 8) {
                    Put(o, "%s=", f->name);
                    PutHex(o, v, n);
                    break;
                }
                uint64_t u = 0;
                for (size_t i = 0; i < n; i++)
                    u |= (uint64_t) v[i] << (8 * i);
                Put(o, "%s=%llu", f->name, (unsigned long long) u);
                break;
            }
        }
    }
}

void rtpdump_selected(int slot, const uint8_t* tlv, size_t len) {
    char line[768];
    out_t o = { .s = line, .cap = sizeof line };
    line[0] = 0;
    Walk(&o, tlv, len, kTop);
    if (o.unknown)
        HAPLogInfo(&logObject, "%d: %u TLV item(s) not in the known layout: %s", slot, o.unknown, line);
    else
        HAPLogDebug(&logObject, "%d: %s", slot, line);
}
