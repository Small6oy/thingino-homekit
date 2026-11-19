// SPDX-License-Identifier: GPL-3.0-or-later
#include "snapshot.h"
#include "rss_ipc.h"
#include "ringutil.h"
#include <errno.h>
#include <stdatomic.h>
#include <time.h>
#include "HAP.h"

static const HAPLogObject logObject = { .subsystem = "rhkd", .category = "Snapshot" };

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

size_t snapshot_take(const char* ringName, uint8_t* out, size_t cap, unsigned timeoutMs) {
    rss_ring_t* ring = rss_ring_open(ringName);
    if (!ring) {
        HAPLogError(&logObject, "cannot open ring %s", ringName);
        return 0;
    }
    size_t result = 0;
    if (!rss_ring_version_ok(ring, NULL)) {
        HAPLogError(&logObject, "ring %s version mismatch", ringName);
        goto out;
    }
    const rss_ring_header_t* hdr = rss_ring_get_header(ring);
    rss_ring_acquire(ring); // demand wakes the JPEG encoder
    uint64_t seq = atomic_load(&hdr->write_seq) + 1;
    uint32_t inc = ring_incarnation(ring);
    uint64_t start = now_ms();
    while (now_ms() - start < timeoutMs) {
        // Read first, wait only on -EAGAIN (the raptor rad/ringdump pattern): waiting first can
        // miss a frame published before the wait's futex snapshot.
        uint32_t len = 0;
        rss_ring_slot_t meta;
        int ret = rss_ring_read(ring, &seq, out, (uint32_t) cap, &len, &meta);
        if (ret == -EAGAIN) {
            rss_ring_wait(ring, 100);
            continue;
        }
        if (ret == RSS_EOVERFLOW) {
            if (ring_recreated(ring, inc)) {
                HAPLogInfo(&logObject, "%s was recreated during the snapshot; giving up", ringName);
                break; // every read would overflow until the timeout
            }
            seq = atomic_load(&hdr->write_seq);
            continue;
        }
        if (ret == -ENOSPC) {
            HAPLogError(&logObject, "JPEG larger than %u bytes", (unsigned) cap);
            break;
        }
        if (ret == 0 && len >= 2 && out[0] == 0xFF && out[1] == 0xD8) {
            result = len;
            HAPLogInfo(&logObject, "%s: %u bytes in %u ms", ringName, len, (unsigned)(now_ms() - start));
            break;
        }
    }
    rss_ring_release(ring);
out:
    rss_ring_close(ring);
    if (!result)
        HAPLogError(&logObject, "no JPEG from %s within %u ms", ringName, timeoutMs);
    return result;
}
