// Motion state from raptor: rmd's debounced state machine, or rvd's raw IVS flag as a fallback.
// With a yolo model, also one state per detected class (classes.h).
// SPDX-License-Identifier: GPL-3.0-or-later (links raptor-ipc)
#ifndef RHKD_MOTION_H
#define RHKD_MOTION_H
#include <stdbool.h>

#include "classes.h"

// Called on the HAP run loop whenever motion or availability changes.
typedef void (*motion_changed_fn)(bool motion, bool available);
// The same for class sensor idx (an index into classes_t, not a class id).
typedef void (*motion_class_changed_fn)(int idx, bool motion, bool available);

// Starts the polling thread (every @intervalMs). @classes is kept (not copied) and must outlive it.
int motion_start(motion_changed_fn onChange, motion_class_changed_fn onClass, const classes_t* classes,
                 unsigned intervalMs);
bool motion_detected(void); // the main sensor: the record classes (every class without a yolo model)
bool motion_available(void); // false while neither rmd nor rvd answers
bool motion_class_detected(int idx);
bool motion_class_available(void); // false while rvd does not answer (rmd has no classes)
#endif
