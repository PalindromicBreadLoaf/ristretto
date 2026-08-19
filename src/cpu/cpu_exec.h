// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <stdbool.h>
#include <stdint.h>

// Probe codegen area to verify on-the-fly code loading works.

typedef struct {
    bool     available;  // codegen area granted to this app
    void    *area;       // virtual base of the codegen area
    uint32_t size;       // size of the area in bytes
    uint32_t core;       // the only core allowed to run codegen (OSGetCodegenCore)
    uint32_t mode;       // current OSGetCodegenMode() value
} CpuCodegenInfo;

typedef enum {
    CPU_EXEC_PASS,         // runtime-written code executed with correct results
    CPU_EXEC_UNAVAILABLE,  // codegen not granted in this environment
    CPU_EXEC_FAIL,         // codegen present but execution or results were wrong
} CpuExecStatus;

// Fill *out and log the codegen area details.
void cpu_exec_probe(CpuCodegenInfo *out);

// Copy small hand-assembled PPC routines into the codegen area, run them on the
// codegen core.
CpuExecStatus cpu_exec_selftest(void);

typedef enum {
    CPU_PS_ENABLED,   // a paired-single op executed correctly
    CPU_PS_ILLEGAL,   // ps_* raised a Program exception
    CPU_PS_ERROR,     // probe could not run
} CpuPsStatus;

// Wii guest code uses Gekko/Broadway paired-single (ps_*) floats heavily,
// and as such, we need paired singles to work properly. This checks whether paired
// singles are usable.
CpuPsStatus cpu_ps_probe_core(uint32_t core);

void cpu_ps_probe_all(void);
