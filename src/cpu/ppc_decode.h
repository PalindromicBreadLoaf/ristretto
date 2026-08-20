// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef RISTRETTO_CPU_PPC_DECODE_H
#define RISTRETTO_CPU_PPC_DECODE_H

#include <stdbool.h>
#include <stdint.h>

// Decode one guest PowerPC instruction word into the fields every later stage consumes.

typedef enum {
    PPC_CLASS_ILLEGAL = 0,  // not decoded / unhandled encoding
    PPC_CLASS_ALU,          // integer arithmetic/logical/compare
    PPC_CLASS_LOAD,         // memory load
    PPC_CLASS_STORE,        // memory store
    PPC_CLASS_BRANCH,       // b/bl/bc/bclr/bcctr
    PPC_CLASS_FP,           // scalar floating point
    PPC_CLASS_PS,           // paired single (ps_*)
    PPC_CLASS_SYSTEM,       // sc/rfi/sync/isync and other non-memory system ops
} PpcClass;

typedef enum {
    PPC_BR_NONE = 0,
    PPC_BR_RELATIVE,   // target = PC + displacement    (AA = 0)
    PPC_BR_ABSOLUTE,   // target = displacement         (AA = 1)
    PPC_BR_INDIRECT,   // target from CTR (bcctr) or LR (bclr)
} PpcBranchKind;

typedef struct {
    uint32_t raw;
    PpcClass class;

    uint8_t  primary;   // primary opcode
    uint16_t xo;        // extended opcode

    // Register operands
    uint8_t  rd;
    uint8_t  ra;
    uint8_t  rb;
    bool     rc;        // record bit

    int32_t  imm;       // sign-extended D/SIMM displacement or immediate
    uint32_t spr;       // decoded SPR number for mfspr/mtspr

    // Branch decode
    PpcBranchKind branch;
    bool     branch_link;   // LK bit
    bool     branch_cond;   // conditional (bc/bclr/bcctr) vs unconditional (b)
    int32_t  branch_disp;   // byte displacement (relative) or target (absolute)
    uint8_t  branch_bo;     // BO field (conditional branches)
    uint8_t  branch_bi;     // BI field (condition bit index)

    // Access shape.
    bool     is_mem;
    bool     is_fp_mem;     // memory op targets an FPR
    bool     mem_update;    // update form
    bool     mem_indexed;   // indexed form (EA = rA + rB) rather than rA + D
    uint8_t  mem_size;      // 1/2/4/8 bytes

    bool     writes_pc;     // control leaves this instruction non-sequentially
    bool     ends_block;    // basic block terminates at this instruction
} PpcInst;

// Decode a single big-endian-native instruction word.
bool ppc_decode(uint32_t word, PpcInst *out);

bool ppc_decode_selftest(void);

#endif  // RISTRETTO_CPU_PPC_DECODE_H
