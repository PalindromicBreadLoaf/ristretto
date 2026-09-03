// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef RISTRETTO_CPU_PPC_INTERP_H
#define RISTRETTO_CPU_PPC_INTERP_H

#include <stdbool.h>
#include <stdint.h>

// PowerPC interpreter over a guest context struct.

typedef struct {
    uint32_t gpr[32];
    union { uint64_t u; double d; } fpr[32];
    uint32_t cr;
    uint32_t xer;
    uint32_t lr;
    uint32_t ctr;
    uint32_t pc;

    // Virtual privileged state.
    uint32_t msr;
    uint32_t srr0;
    uint32_t srr1;
    uint32_t sprg[4];
    uint32_t gqr[8];
    uint32_t sr[16];
    uint64_t timebase;
    uint64_t timebase_host_ticks;
    int32_t  decrementer;
    uint64_t decrementer_host_ticks;
    bool     decrementer_armed;
    bool     decrementer_exception_pending;
} PpcContext;

typedef enum {
    PPC_INTERP_STOP,     // reached the caller's stop PC
    PPC_INTERP_LIMIT,    // hit the instruction budget first
    PPC_INTERP_ILLEGAL,  // an unimplemented / illegal instruction
} PpcInterpResult;

// Execute from ctx->pc until pc == stop_pc, an illegal instruction is hit, or
// max_insts instructions have run.
PpcInterpResult ppc_interp_run(PpcContext *ctx, uint32_t stop_pc,
                               uint32_t max_insts, uint32_t *executed);

bool ppc_decrementer_pending(PpcContext *ctx);
void ppc_decrementer_acknowledge_exception(PpcContext *ctx);

// Self test
bool ppc_interp_selftest(void);

#endif  // RISTRETTO_CPU_PPC_INTERP_H
