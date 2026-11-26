// Detection classes from raptor.conf [motion] (see classes.h).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "classes.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// The [motion] keys this needs; raptor.conf values are short.
typedef struct {
    char algorithm[16];
    char classNames[256];
    char classes[256];
    char recordClasses[256];
} motion_keys_t;

static char* Trim(char* s) {
    while (*s == ' ' || *s == '\t')
        s++;
    char* e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
        *--e = 0;
    return s;
}

static void ReadMotionKeys(const char* path, motion_keys_t* k) {
    memset(k, 0, sizeof *k);
    FILE* f = fopen(path, "r");
    if (!f)
        return;
    char line[320];
    bool inMotion = false;
    while (fgets(line, sizeof line, f)) {
        char* s = Trim(line);
        if (*s == '[') {
            inMotion = strncmp(s, "[motion]", 8) == 0;
            continue;
        }
        if (!inMotion || *s == '#' || *s == ';')
            continue;
        char* eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = 0;
        char* key = Trim(s);
        char* val = eq + 1;
        char* hash = strchr(val, '#'); // raptor.conf allows trailing comments
        if (hash)
            *hash = 0;
        val = Trim(val);
        char* dst = NULL;
        size_t cap = 0;
        if (!strcmp(key, "algorithm"))
            dst = k->algorithm, cap = sizeof k->algorithm;
        else if (!strcmp(key, "class_names"))
            dst = k->classNames, cap = sizeof k->classNames;
        else if (!strcmp(key, "classes"))
            dst = k->classes, cap = sizeof k->classes;
        else if (!strcmp(key, "record_classes"))
            dst = k->recordClasses, cap = sizeof k->recordClasses;
        if (dst)
            snprintf(dst, cap, "%s", val);
    }
    fclose(f);
}

// Entry n (0-based) of a comma-separated list into out; false when there is none.
static bool ListEntry(const char* list, int n, char* out, size_t cap) {
    const char* p = list;
    for (int i = 0; p && *p; i++) {
        size_t len = strcspn(p, ",");
        if (i == n) {
            char tmp[64];
            snprintf(tmp, sizeof tmp, "%.*s", (int) (len < sizeof tmp - 1 ? len : sizeof tmp - 1), p);
            snprintf(out, cap, "%s", Trim(tmp));
            return true;
        }
        p += len;
        if (*p == ',')
            p++;
    }
    return false;
}

static int NameId(const char* names, const char* s, size_t n) {
    char entry[64];
    for (int id = 0; id < RHKD_MAX_CLASS_ID && ListEntry(names, id, entry, sizeof entry); id++)
        if (strlen(entry) == n && strncasecmp(entry, s, n) == 0)
            return id;
    return -1;
}

int classes_parse_list(const char* list, const char* names, uint16_t* mask, bool* all) {
    *mask = 0;
    *all = true;
    if (!list)
        return 0;
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%s", list);
    if (!strcasecmp(Trim(tmp), "none")) {
        *all = false;
        return 0;
    }
    int bad = 0;
    const char* s = list;
    while (*s) {
        while (*s == ' ' || *s == ',')
            s++;
        size_t n = strcspn(s, ",");
        while (n && s[n - 1] == ' ')
            n--;
        if (!n)
            break;
        int id = strspn(s, "0123456789") == n ? atoi(s) : NameId(names, s, n);
        if (id >= 0 && id < RHKD_MAX_CLASS_ID)
            *mask |= (uint16_t) (1u << id);
        else
            bad++;
        s += n;
        while (*s && *s != ',')
            s++;
    }
    if (*mask)
        *all = false;
    return bad;
}

void classes_load(classes_t* c, const char* path) {
    memset(c, 0, sizeof *c);
    c->recordAll = true;
    motion_keys_t k;
    ReadMotionKeys(path, &k);
    c->yolo = !strcmp(k.algorithm, "yolo");
    if (!c->yolo)
        return;

    uint16_t detect;
    bool detectAll;
    classes_parse_list(k.classes, k.classNames, &detect, &detectAll);
    classes_parse_list(k.recordClasses, k.classNames, &c->recordMask, &c->recordAll);

    // A sensor for each detected class that has a name in class_names (an unnamed class has
    // no label to show in Home), in class order.
    for (int id = 0; id < RHKD_MAX_CLASS_ID && c->count < RHKD_MAX_CLASS_SENSORS; id++) {
        char entry[33];
        if (!ListEntry(k.classNames, id, entry, sizeof entry) || !*entry)
            break;
        if (!detectAll && !(detect & (1u << id)))
            continue;
        c->id[c->count] = (uint8_t) id;
        snprintf(c->name[c->count], sizeof c->name[0], "%s", entry);
        c->name[c->count][0] = (char) toupper((unsigned char) c->name[c->count][0]);
        c->count++;
    }
}

uint16_t classes_fingerprint(const classes_t* c) {
    uint32_t h = 2166136261u; // FNV-1a
    for (int i = 0; i < c->count; i++) {
        h = (h ^ c->id[i]) * 16777619u;
        for (const char* p = c->name[i]; *p; p++)
            h = (h ^ (uint8_t) *p) * 16777619u;
    }
    return (uint16_t) (h ^ (h >> 16));
}

bool classes_record_hit(const classes_t* c, bool motion, bool hasMask, int classMask) {
    if (!motion)
        return false;
    if (!hasMask || c->recordAll)
        return true;
    return (classMask & c->recordMask) != 0;
}
