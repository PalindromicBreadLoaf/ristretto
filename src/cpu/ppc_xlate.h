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

// Why a session ended.
typedef enum {
    PPC_XSTOP_STOP_PC,     // reached the caller's stop PC
    PPC_XSTOP_GX_WRITE,    // hit the first write-gather-pipe (GX) write
    PPC_XSTOP_BUDGET,      // ran out of the block budget
    PPC_XSTOP_FAULT,       // an instruction neither translator nor interpreter handles
} PpcXlateStop;

#define PPC_XLATE_TRANSFER_TRACE 24u

typedef struct {
    uint32_t pc;
    uint32_t word;
    uint32_t target;
    uint32_t lr;
    uint32_t sp;
    uint32_t r3;
    uint32_t repeats;
} PpcXlateTransfer;

typedef struct {
    uint32_t blocks_run;
    uint32_t cache_hits;
    uint32_t cache_misses;
    PpcXlateStop stop;
    uint32_t first_gx_write_ea;   // valid when stop == PPC_XSTOP_GX_WRITE
    uint32_t last_pc;             // guest PC of the faulting instruction (FAULT)
    uint32_t last_word;           // guest instruction word at last_pc (FAULT)
    uint8_t  last_class;          // PpcClass of the faulting instruction (FAULT)
    uint32_t last_transfer_pc;    // most recent guest control-transfer instruction
    uint32_t last_transfer_word;
    uint32_t last_transfer_target;
    uint32_t transfer_count;
    PpcXlateTransfer transfers[PPC_XLATE_TRANSFER_TRACE];
} PpcXlateSession;

// Pass as `stop_pc` to run until the first write-gather-pipe (GX) write.
#define PPC_XLATE_RUN_TO_GX 0xFFFFFFFFu

// Translate guest code from `entry_pc` until pc == stop_pc, the first GX write,
// a fault, or `max_blocks` blocks have run.
PpcXlateResult ppc_xlate_run(PpcContext *ctx, uint32_t entry_pc, uint32_t stop_pc,
                             uint32_t max_blocks, PpcXlateSession *out);

bool ppc_xlate_identity_selftest(void);

bool ppc_xlate_memblock_selftest(void);

bool ppc_xlate_branch_selftest(void);

bool ppc_xlate_interrupt_selftest(void);

bool ppc_xlate_mmio_selftest(void);

bool ppc_xlate_entry_selftest(void);

#endif  // RISTRETTO_CPU_PPC_XLATE_H
