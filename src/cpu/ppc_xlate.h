// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef RISTRETTO_CPU_PPC_XLATE_H
#define RISTRETTO_CPU_PPC_XLATE_H

#include <stdbool.h>
#include <stdint.h>

#include "cpu/ppc_interp.h"

// Mostly identity binary translator

typedef enum {
    PPC_XLATE_OK,           // block built, ran, context updated
    PPC_XLATE_UNSUPPORTED,  // block contains an instruction the emitter can't rewrite
    PPC_XLATE_UNAVAILABLE,  // codegen area or fastmem window not present
    PPC_XLATE_ERROR,        // codegen drive failed
} PpcXlateResult;

// Translate and run one straight line block of `count` guest instruction words,
// updating *ctx in place.
PpcXlateResult ppc_xlate_run_block(PpcContext *ctx, uint32_t guest_pc,
                                   const uint32_t *guest, uint32_t count);

bool ppc_xlate_identity_selftest(void);

bool ppc_xlate_memblock_selftest(void);

#endif  // RISTRETTO_CPU_PPC_XLATE_H
