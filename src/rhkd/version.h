// SPDX-License-Identifier: GPL-3.0-or-later
// rhkd version. Reported to HomeKit as the accessory's firmware revision.
// It must only ever increase: HomeKitADK stores it and refuses to start on a downgrade
// ("Firmware must not be downgraded"). Format: major.minor.revision (all numeric).
#ifndef RHKD_VERSION_H
#define RHKD_VERSION_H
#define RHKD_VERSION "0.7.0"
#endif
