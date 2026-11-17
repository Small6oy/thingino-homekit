// Motion state from raptor: rmd's debounced state machine, or rvd's raw IVS flag as a fallback.
// SPDX-License-Identifier: GPL-3.0-or-later (links raptor-ipc)
#ifndef RHKD_MOTION_H
#define RHKD_MOTION_H
#include <stdbool.h>

// Called on the HAP run loop whenever motion or availability changes.
typedef void (*motion_changed_fn)(bool motion, bool available);

// Starts the polling thread (every @intervalMs).
int motion_start(motion_changed_fn onChange, unsigned intervalMs);
bool motion_detected(void);
bool motion_available(void); // false while neither rmd nor rvd answers
#endif
