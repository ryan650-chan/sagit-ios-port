// SPDX-License-Identifier: BSD-2-Clause
#ifndef BRIDGE_GIC_HANDOFF_H
#define BRIDGE_GIC_HANDOFF_H
#include <stdint.h>
// MSM8998 sagit DT: gic@17a00000 (distributor) + redistributors at 0x17b00000,
// GICv3. Matches the kernel's SDM835_GICD_BASE (kernel/pexpert/pexpert/arm64/SDM835.h).
// The EDK2 ArmGic driver clears GICD_CTLR (writes 0x0) from its ExitBootServices
// callback (ArmPkg/Drivers/ArmGic/ArmGicLib.c ArmGicDisableDistributor), which is
// the Linux handoff convention: leave the OS a disabled distributor to program.
// XNU follows the Apple/iBoot convention instead: pe_sdm835_init_interrupts()
// (kernel/osfmk/arm64/sdm835.c) panics with "firmware must enable affinity
// routing and Group1NS" unless GICD_CTLR keeps affinity routing and Group 1
// non-secure forwarding set. Re-arm the distributor as the last firmware action
// before entering the kernel; the kernel re-initializes its own CPU interface
// and leaves every SPI/PPI source disabled, so no interrupt can fire early.
#define BRIDGE_GICD_BASE 0x17a00000ULL
// GICD_CTLR bits: bit 1 EnableGrp1 (Group 1 non-secure), bit 4 ARE
// (ARM_GIC_ICDDCR_ARE, ArmPkg/Include/Library/ArmGicLib.h). This is exactly
// the 0x12 mask sdm835.c requires; Group 0 stays off (the kernel's pe_fiq.c
// enables it later, and XNU never uses secure Group 1).
#define BRIDGE_GIC_CTLR_HANDOFF 0x12U
// Returns the GICD_CTLR value observed before re-arming (diagnostics).
static inline uint32_t bridge_gic_handoff(void) {
    volatile uint32_t *ctlr = (volatile uint32_t *)BRIDGE_GICD_BASE;
    uint32_t control = *ctlr;
    __asm__ volatile("dmb oshld" ::: "memory");
    *ctlr = control | BRIDGE_GIC_CTLR_HANDOFF;
    __asm__ volatile("dsb sy" ::: "memory");
    return control;
}
#endif
