// SPDX-License-Identifier: BSD-2-Clause
.text
.p2align 4
.globl bridge_enter
bridge_enter:
    msr daifset, #15
    // EFI on this platform uses identity mappings. No memory access below.
    mrs x9, CurrentEL
    cmp x9, #8
    b.eq 2f
    mrs x9, sctlr_el1
    bic x9, x9, #1
    bic x9, x9, #4
    bic x9, x9, #0x1000
    dsb sy
    msr sctlr_el1, x9
    isb
    tlbi vmalle1
    dsb sy
    isb
    b 3f
2:
    mrs x9, sctlr_el2
    bic x9, x9, #1
    bic x9, x9, #4
    bic x9, x9, #0x1000
    dsb sy
    msr sctlr_el2, x9
    isb
    mov x9, #0x80000000
    msr hcr_el2, x9
    mov x9, #3
    msr cnthctl_el2, x9
    msr cntvoff_el2, xzr
    mov x9, #0x0800
    movk x9, #0x30d0, lsl #16
    msr sctlr_el1, x9
    adr x9, 3f
    msr elr_el2, x9
    mov x9, #0x3c5
    msr spsr_el2, x9
    eret
3:
    mov x9, #0x300000
    msr cpacr_el1, x9
    msr cntv_ctl_el0, xzr
    msr cntp_ctl_el0, xzr
    isb
    // Re-arm the GIC distributor for XNU now that the MMU is off.
    // pe_sdm835_init_interrupts (kernel/osfmk/arm64/sdm835.c) panics
    // unless firmware leaves GICD_CTLR.ARE (bit 4) and EnableGrp1
    // (bit 1) forwarding set. The earlier store from the EFI/bridge
    // context may have been served by a cacheable mapping and never
    // reached the distributor, so clean+invalidate any stale cached
    // line for this address and store again with the MMU disabled,
    // where the access is a direct physical device write.
    // Clobbers x10-x12 only; x0 (boot args) and x1 (kernel entry)
    // are preserved for the kernel entry protocol.
    movz x10, #0x17a0, lsl #16
    dc civac, x10
    dsb sy
    ldr x11, [x10]
    mov w12, #0x12
    orr x11, x11, x12
    str x11, [x10]
    dsb sy
    isb
    br x1
