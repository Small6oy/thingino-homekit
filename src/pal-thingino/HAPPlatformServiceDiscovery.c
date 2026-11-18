// Thingino PAL: service discovery via troglobit mdnsd.
//
// The camera already runs mdnsd, which advertises every *.service file in
// /etc/mdns.d and reloads them on SIGHUP. We write our record to a file in
// /run (RAM, so TXT updates don't wear the flash) and keep a symlink to it in
// /etc/mdns.d.
//
// SPDX-License-Identifier: Apache-2.0

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "HAPPlatform+Init.h"
#include "HAPPlatformServiceDiscovery+Init.h"

static const HAPLogObject logObject = { .subsystem = kHAPPlatform_LogSubsystem, .category = "ServiceDiscovery" };

#ifndef HAP_MDNSD_SERVICE_DIR
#define HAP_MDNSD_SERVICE_DIR "/etc/mdns.d"
#endif
// The record file is /run/<name>.service, linked from /etc/mdns.d/<name>.service. <name> is "hap" unless
// RHKD_SERVICE_NAME says otherwise, so a test instance can advertise next to the real one without
// overwriting its record.
static char gRunFile[96], gLinkFile[96];

static void InitPaths(void) {
    if (gRunFile[0])
        return;
    const char* name = getenv("RHKD_SERVICE_NAME");
    if (!name || !*name || strchr(name, '/') || strlen(name) > 32)
        name = "hap";
    snprintf(gRunFile, sizeof gRunFile, "/run/%s.service", name);
    snprintf(gLinkFile, sizeof gLinkFile, HAP_MDNSD_SERVICE_DIR "/%s.service", name);
}
#define HAP_MDNSD_RUN_FILE (InitPaths(), gRunFile)
#ifndef HAP_MDNSD_PIDFILE
#define HAP_MDNSD_PIDFILE "/var/run/mdnsd.pid"
#endif

static void SignalMdnsd(void) {
    FILE* f = fopen(HAP_MDNSD_PIDFILE, "r");
    long pid = 0;
    if (f) {
        if (fscanf(f, "%ld", &pid) != 1) {
            pid = 0;
        }
        fclose(f);
    }
    if (pid <= 1 || kill((pid_t) pid, SIGHUP) != 0) {
        // Fall back to signalling by name.
        if (system("killall -HUP mdnsd >/dev/null 2>&1") != 0) {
            HAPLogError(&logObject, "Could not signal mdnsd to reload; is it running?");
        }
    }
}

static void EnsureSymlink(void) {
    const char* run = HAP_MDNSD_RUN_FILE;
    const char* link = gLinkFile;
    char target[256];
    ssize_t n = readlink(link, target, sizeof target - 1);
    if (n > 0) {
        target[n] = '\0';
        if (strcmp(target, run) == 0) {
            return;
        }
    }
    unlink(link);
    if (symlink(run, link) != 0) {
        HAPLogError(&logObject, "symlink %s -> %s failed: %d.", link, run, errno);
        HAPFatalError();
    }
}

static bool IsPrintable(const void* bytes, size_t numBytes) {
    const unsigned char* b = bytes;
    for (size_t i = 0; i < numBytes; i++) {
        if (b[i] < 0x20 || b[i] > 0x7E) {
            return false;
        }
    }
    return true;
}

static void WriteServiceFile(
        HAPPlatformServiceDiscoveryRef serviceDiscovery,
        HAPPlatformServiceDiscoveryTXTRecord* txtRecords,
        size_t numTXTRecords) {
    char tmp[112];
    snprintf(tmp, sizeof tmp, "%s.tmp", HAP_MDNSD_RUN_FILE);
    FILE* f = fopen(tmp, "w");
    if (!f) {
        HAPLogError(&logObject, "open %s failed: %d.", tmp, errno);
        HAPFatalError();
    }
    fprintf(f, "# Written by the HomeKit accessory server. Do not edit.\n");
    fprintf(f, "name %s\n", serviceDiscovery->name);
    fprintf(f, "type %s\n", serviceDiscovery->protocol);
    fprintf(f, "port %u\n", serviceDiscovery->port);
    for (size_t i = 0; i < numTXTRecords; i++) {
        HAPPrecondition(!txtRecords[i].value.numBytes || txtRecords[i].value.bytes);
        if (!txtRecords[i].value.bytes) {
            fprintf(f, "txt %s\n", txtRecords[i].key);
            continue;
        }
        if (!IsPrintable(txtRecords[i].value.bytes, txtRecords[i].value.numBytes)) {
            HAPLogError(&logObject, "TXT record \"%s\" is not printable; mdnsd files cannot carry it.", txtRecords[i].key);
            HAPFatalError();
        }
        fprintf(f, "txt %s=%.*s\n", txtRecords[i].key, (int) txtRecords[i].value.numBytes,
                (const char*) txtRecords[i].value.bytes);
        {
            // HAPLog has no %.*s, so copy into a terminated buffer.
            char value[256];
            size_t n = txtRecords[i].value.numBytes < sizeof value - 1 ? txtRecords[i].value.numBytes : sizeof value - 1;
            memcpy(value, txtRecords[i].value.bytes, n);
            value[n] = '\0';
            HAPLogDebug(&logObject, "txt %s=%s", txtRecords[i].key, value);
        }
    }
    if (fclose(f) != 0 || rename(tmp, HAP_MDNSD_RUN_FILE) != 0) {
        HAPLogError(&logObject, "writing %s failed: %d.", HAP_MDNSD_RUN_FILE, errno);
        HAPFatalError();
    }
    SignalMdnsd();
}

void HAPPlatformServiceDiscoveryCreate(
        HAPPlatformServiceDiscoveryRef serviceDiscovery,
        const HAPPlatformServiceDiscoveryOptions* options) {
    HAPPrecondition(serviceDiscovery);
    HAPPrecondition(options);
    HAPRawBufferZero(serviceDiscovery, sizeof *serviceDiscovery);
}

void HAPPlatformServiceDiscoveryRegister(
        HAPPlatformServiceDiscoveryRef serviceDiscovery,
        const char* name,
        const char* protocol,
        HAPNetworkPort port,
        HAPPlatformServiceDiscoveryTXTRecord* txtRecords,
        size_t numTXTRecords) {
    HAPPrecondition(serviceDiscovery);
    HAPPrecondition(!serviceDiscovery->isRegistered);
    HAPPrecondition(name);
    HAPPrecondition(protocol);
    HAPPrecondition(txtRecords);

    if (strlen(name) >= sizeof serviceDiscovery->name || strlen(protocol) >= sizeof serviceDiscovery->protocol) {
        HAPLogError(&logObject, "Service name or protocol too long.");
        HAPFatalError();
    }
    snprintf(serviceDiscovery->name, sizeof serviceDiscovery->name, "%s", name);
    snprintf(serviceDiscovery->protocol, sizeof serviceDiscovery->protocol, "%s", protocol);
    serviceDiscovery->port = port;

    HAPLogInfo(&logObject, "Registering \"%s\" %s port %u via mdnsd.", name, protocol, port);
    EnsureSymlink();
    WriteServiceFile(serviceDiscovery, txtRecords, numTXTRecords);
    serviceDiscovery->isRegistered = true;
}

void HAPPlatformServiceDiscoveryUpdateTXTRecords(
        HAPPlatformServiceDiscoveryRef serviceDiscovery,
        HAPPlatformServiceDiscoveryTXTRecord* txtRecords,
        size_t numTXTRecords) {
    HAPPrecondition(serviceDiscovery);
    HAPPrecondition(serviceDiscovery->isRegistered);
    HAPPrecondition(txtRecords);
    HAPLogInfo(&logObject, "Updating TXT records.");
    WriteServiceFile(serviceDiscovery, txtRecords, numTXTRecords);
}

void HAPPlatformServiceDiscoveryStop(HAPPlatformServiceDiscoveryRef serviceDiscovery) {
    HAPPrecondition(serviceDiscovery);
    HAPPrecondition(serviceDiscovery->isRegistered);
    unlink(HAP_MDNSD_RUN_FILE);
    SignalMdnsd();
    serviceDiscovery->isRegistered = false;
}
