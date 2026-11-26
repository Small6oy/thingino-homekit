// Detection classes from raptor.conf [motion], for one HomeKit motion sensor per class.
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef RHKD_CLASSES_H
#define RHKD_CLASSES_H
#include <stdbool.h>
#include <stdint.h>

// One sensor per detected class, at most this many (each is 5 HAP attributes).
#define RHKD_MAX_CLASS_SENSORS 8
// rvd reports classes in a 16-bit class_mask (RSS_IVS_MAX_CLASSES in raptor-hal).
#define RHKD_MAX_CLASS_ID 16

typedef struct {
    bool yolo;                             // [motion] algorithm = yolo: rvd reports classes
    int count;                             // class sensors
    uint8_t id[RHKD_MAX_CLASS_SENSORS];     // their class ids
    char name[RHKD_MAX_CLASS_SENSORS][33]; // default names ("Person", from class_names)
    uint16_t recordMask;                   // [motion] record_classes
    bool recordAll;                        // unset (or nothing resolved): every class
} classes_t;

// Reads [motion] from path. Without yolo there are no class sensors and every motion records.
// Never fails: a missing file or section gives the plain-motion setup.
void classes_load(classes_t* c, const char* path);

// Parses a class list ("person, 2", "none", "") against class_names; out: mask + all.
// Exposed for tests. Returns the number of entries that did not resolve.
int classes_parse_list(const char* list, const char* names, uint16_t* mask, bool* all);

// Changes whenever the sensors (ids or default names) change: part of the accessory database revision.
uint16_t classes_fingerprint(const classes_t* c);

// Whether the main (HKSV) sensor should count this motion: the record classes only, when rvd says which.
bool classes_record_hit(const classes_t* c, bool motion, bool hasMask, int classMask);
#endif
