// Thingino PAL: service discovery via troglobit mdnsd .service files.
// Replaces the dns_sd (Bonjour/Avahi) backend from HomeKitADK PAL/POSIX.
// SPDX-License-Identifier: Apache-2.0

#ifndef HAP_PLATFORM_SERVICE_DISCOVERY_INIT_H
#define HAP_PLATFORM_SERVICE_DISCOVERY_INIT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <net/if.h>
#include <stdbool.h>

#include "HAPPlatform.h"

#if __has_feature(nullability)
#pragma clang assume_nonnull begin
#endif

typedef struct {
    /** Unused by this backend (mdnsd chooses the interface); kept for API compatibility. */
    const char* _Nullable interfaceName;
} HAPPlatformServiceDiscoveryOptions;

#define kHAPPlatformServiceDiscovery_MaxNameBytes (64)
#define kHAPPlatformServiceDiscovery_MaxTXTRecordBufferBytes (512)

struct HAPPlatformServiceDiscovery {
    /**@cond */
    bool isRegistered;
    char name[kHAPPlatformServiceDiscovery_MaxNameBytes];
    char protocol[32];
    HAPNetworkPort port;
    /**@endcond */
};

void HAPPlatformServiceDiscoveryCreate(
        HAPPlatformServiceDiscoveryRef serviceDiscovery,
        const HAPPlatformServiceDiscoveryOptions* options);

#if __has_feature(nullability)
#pragma clang assume_nonnull end
#endif

#ifdef __cplusplus
}
#endif

#endif
