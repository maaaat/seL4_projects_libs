/*
 * Copyright 2019, Data61
 * Commonwealth Scientific and Industrial Research Organisation (CSIRO)
 * ABN 41 687 119 230.
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(DATA61_BSD)
 */
#pragma once

#include <sel4/sel4.h>
#include <sel4vm/arch/processor.h>

#define MODE_USER       0x10
#define MODE_FIQ        0x11
#define MODE_IRQ        0x12
#define MODE_SUPERVISOR 0x13
#define MODE_MONITOR    0x16
#define MODE_ABORT      0x17
#define MODE_HYP        0x1a
#define MODE_UNDEFINED  0x1b
#define MODE_SYSTEM     0x1f

typedef enum processor_mode {
    PMODE_USER       = MODE_USER,
    PMODE_FIQ        = MODE_FIQ,
    PMODE_IRQ        = MODE_IRQ,
    PMODE_SUPERVISOR = MODE_SUPERVISOR,
    PMODE_MONITOR    = MODE_MONITOR,
    PMODE_ABORT      = MODE_ABORT,
    PMODE_HYPERVISOR = MODE_HYP,
    PMODE_UNDEFINED  = MODE_UNDEFINED,
    PMODE_SYSTEM     = MODE_SYSTEM
} processor_mode_t;

#define seL4_UnknownSyscall_ARG0 seL4_UnknownSyscall_R0

#define HSR_UNKNOWN_EXCEPTION       (0x0)
#define HSR_WFx_EXCEPTION           (0x1)
#define HSR_CP15_32_EXCEPTION       (0x3)
#define HSR_CP15_64_EXCEPTION       (0x4)
#define HSR_CP14_32_EXCEPTION       (0x5)
#define HSR_CP14_LC_EXCEPTION       (0x6)
#define HSR_SIMD_EXCEPTION          (0x7)
#define HSR_CP10_EXCEPTION          (0x8)
#define HSR_CP14_64_EXCEPTION       (0xC)
#define HSR_ILLEGAL_EXCEPTION       (0xE)
#define HSR_SVC_EXCEPTION           (0x11)
#define HSR_HVC_EXCEPTION           (0x12)
#define HSR_SMC_EXCEPTION           (0x13)
#define HSR_PREFETCH_EXCEPTION      (0x20)
#define HSR_PREFETCH_HYP_EXCEPTION  (0x21)
#define HSR_PC_ALIGN_EXCEPTION      (0x22)
#define HSR_DABT_EXCEPTION          (0x24)
#define HSR_DABT_HYP_EXCEPTION      (0x25)
/* Remaining values (0x26 - 0x3f) are reserved */

#define HSR_REASON_ENTRY(HSR) [HSR] = #HSR

static const char *hsr_reasons[HSR_MAX_EXCEPTION] = {
    [0 ... HSR_MAX_EXCEPTION - 1] = "Unknown Exception Class",
    HSR_REASON_ENTRY(HSR_UNKNOWN_EXCEPTION),
    HSR_REASON_ENTRY(HSR_WFx_EXCEPTION),
    HSR_REASON_ENTRY(HSR_CP15_32_EXCEPTION),
    HSR_REASON_ENTRY(HSR_CP15_64_EXCEPTION),
    HSR_REASON_ENTRY(HSR_CP14_32_EXCEPTION),
    HSR_REASON_ENTRY(HSR_CP14_LC_EXCEPTION),
    HSR_REASON_ENTRY(HSR_SIMD_EXCEPTION),
    HSR_REASON_ENTRY(HSR_CP10_EXCEPTION),
    HSR_REASON_ENTRY(HSR_CP14_64_EXCEPTION),
    HSR_REASON_ENTRY(HSR_ILLEGAL_EXCEPTION),
    HSR_REASON_ENTRY(HSR_SVC_EXCEPTION),
    HSR_REASON_ENTRY(HSR_HVC_EXCEPTION),
    HSR_REASON_ENTRY(HSR_SMC_EXCEPTION),
    HSR_REASON_ENTRY(HSR_PREFETCH_EXCEPTION),
    HSR_REASON_ENTRY(HSR_PREFETCH_HYP_EXCEPTION),
    HSR_REASON_ENTRY(HSR_PC_ALIGN_EXCEPTION),
    HSR_REASON_ENTRY(HSR_DABT_EXCEPTION),
    HSR_REASON_ENTRY(HSR_DABT_HYP_EXCEPTION)
};
