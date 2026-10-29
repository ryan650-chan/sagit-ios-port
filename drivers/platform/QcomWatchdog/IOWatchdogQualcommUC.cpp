// SPDX-License-Identifier: BSD-2-Clause
#include "IOWatchdogQualcommUC.h"
#include "QualcommWatchdogTimer.h"
#include <IOKit/IOLib.h>

#define super IOUserClient
OSDefineMetaClassAndStructors(IOWatchdogQualcommUC, super);

bool IOWatchdogQualcommUC::initWithTask(task_t task, void *token, UInt32 type,
    OSDictionary *properties)
{
    // watchdogd does not hold kIOClientPrivilegeAdministrator, so no privilege
    // gate here; the type is recorded so an unexpected opener is visible.
    if (!task) {
        return false;
    }
    if (!super::initWithTask(task, token, type, properties)) {
        return false;
    }
    setProperty("WatchdogClientType", type, 32);
    return true;
}

bool IOWatchdogQualcommUC::start(IOService *provider)
{
    auto *timer = OSDynamicCast(QualcommWatchdogTimer, provider);
    if (!timer || !super::start(provider)) {
        return false;
    }
    controller = timer;
    controller->retain();
    IOLog("QualcommWatchdogTimer: user client started, provider=%s\n",
        provider->getName());
    return true;
}

IOReturn IOWatchdogQualcommUC::clientClose()
{
    if (controller) {
        IOLog("QualcommWatchdogTimer: user client closed after %llu calls\n",
            (unsigned long long)calls);
    }
    terminate();
    return kIOReturnSuccess;
}

void IOWatchdogQualcommUC::stop(IOService *provider)
{
    super::stop(provider);
}

void IOWatchdogQualcommUC::free()
{
    OSSafeReleaseNULL(controller);
    super::free();
}

IOReturn IOWatchdogQualcommUC::externalMethod(UInt32 selector,
    IOExternalMethodArguments *args, IOExternalMethodDispatch *, OSObject *, void *)
{
    if (isInactive()) {
        return kIOReturnNotAttached;
    }
    if (!args) {
        return kIOReturnBadArgument;
    }

    // Record what the daemon asks for. The selector numbering belongs to the
    // Apple framework, not to this driver, so the first few calls are the only
    // reliable way to learn it on real hardware.
    calls++;
    IOLog("QualcommWatchdogTimer: external selector=%u in=%u out=%u struct_in=%u struct_out=%u call=%llu\n",
        selector, args->scalarInputCount, args->scalarOutputCount,
        args->structureInputSize, args->structureOutputSize,
        (unsigned long long)calls);

    if (args->scalarOutput && args->scalarOutputCount) {
        for (UInt64 i = 0; i < args->scalarOutputCount; i++) {
            args->scalarOutput[i] = 0;
        }
    }
    // Keep the hardware fed on any control call; tick() also does this every 5 s
    // so a lost call cannot stop the feed.
    if (controller) {
        controller->resetWatchdog();
    }
    return kIOReturnSuccess;
}
