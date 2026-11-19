// SPDX-License-Identifier: GPL-3.0-or-later
// Detecting a recreated raptor ring. rvd/rad recreate a ring either as a new shm file (daemon restart:
// rss_ring_stale sees the new inode) or on the same file with the header's incarnation bumped (an aux resize,
// rvd_aux_detach + attach). rss_ring_stale misses the second case, and rss_ring_read then returns RSS_EOVERFLOW on
// every call, so a reader that treats that as an ordinary overflow spins forever (seen in ringdump, 2026-11-19).
// Readers keep the incarnation from open and treat a change as "reopen", never as a plain overflow.
#ifndef RHKD_RINGUTIL_H
#define RHKD_RINGUTIL_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "rss_ipc.h"

static inline uint32_t ring_incarnation(rss_ring_t* r) {
    return atomic_load(&((rss_ring_header_t*) rss_ring_get_header(r))->incarnation);
}

static inline bool ring_recreated(rss_ring_t* r, uint32_t openIncarnation) {
    return rss_ring_stale(r) || ring_incarnation(r) != openIncarnation;
}

#endif
