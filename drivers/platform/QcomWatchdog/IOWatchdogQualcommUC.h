// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/IOUserClient.h>

class QualcommWatchdogTimer;

// watchdogd opens a user client and then calls into it. The framework supplies
// its own entitled client, and on this port the daemon dies within ~50 ms of a
// successful open (see the 2026-10-26 v4/v6 logs: open -> panic-on-crash
// disabled -> corpse, five times, then "too many corpses"). This client
// replaces that path so the selectors watchdogd actually uses become visible in
// the kernel log, and so the daemon can proceed instead of crashing.
//
// It is deliberately permissive: every selector is logged and answered with
// success. The SoC watchdog keeps being armed and fed by
// QualcommWatchdogTimer::tick() every 5 s independently of this client, so a
// permissive answer cannot leave the hardware unfed.
class IOWatchdogQualcommUC : public IOUserClient {
    OSDeclareDefaultStructors(IOWatchdogQualcommUC);
    friend class QualcommWatchdogTimer;
    QualcommWatchdogTimer *controller = nullptr;
    UInt64 calls = 0;
public:
    bool initWithTask(task_t, void *, UInt32, OSDictionary *) APPLE_KEXT_OVERRIDE;
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    IOReturn clientClose() APPLE_KEXT_OVERRIDE;
    IOReturn externalMethod(UInt32, IOExternalMethodArguments *,
        IOExternalMethodDispatch *, OSObject *, void *) APPLE_KEXT_OVERRIDE;
};
