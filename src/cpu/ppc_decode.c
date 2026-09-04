// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "cpu/ppc_decode.h"

#include <whb/log.h>

#include <string.h>

static inline uint32_t bits(uint32_t w, int hi, int lo) {
    return (w >> lo) & ((1u << (hi - lo + 1)) - 1u);
}

// The 10-bit SPR field is stored as two swapped 5-bit halves.
static uint32_t decode_spr(uint32_t w) {
    uint32_t f = (w >> 11) & 0x3FF;
    return ((f & 0x1F) << 5) | ((f >> 5) & 0x1F);
}

static int32_t sign_extend(uint32_t v, int width) {
    uint32_t m = 1u << (width - 1);
    return (int32_t)((v ^ m) - m);
}

// Memory shape for the direct (rA + D) load/store primary opcodes 32..55.
static bool direct_mem(uint8_t op, PpcInst *o) {
    struct { uint8_t op, size; bool store, update, fp; } t[] = {
        {32, 4, false, false, false}, {33, 4, false, true,  false},  // lwz  lwzu
        {34, 1, false, false, false}, {35, 1, false, true,  false},  // lbz  lbzu
        {40, 2, false, false, false}, {41, 2, false, true,  false},  // lhz  lhzu
        {42, 2, false, false, false}, {43, 2, false, true,  false},  // lha  lhau
        {46, 0, false, false, false},                                // lmw
        {36, 4, true,  false, false}, {37, 4, true,  true,  false},  // stw  stwu
        {38, 1, true,  false, false}, {39, 1, true,  true,  false},  // stb  stbu
        {44, 2, true,  false, false}, {45, 2, true,  true,  false},  // sth  sthu
        {47, 0, true,  false, false},                                // stmw
        {48, 4, false, false, true},  {49, 4, false, true,  true},   // lfs  lfsu
        {50, 8, false, false, true},  {51, 8, false, true,  true},   // lfd  lfdu
        {52, 4, true,  false, true},  {53, 4, true,  true,  true},   // stfs stfsu
        {54, 8, true,  false, true},  {55, 8, true,  true,  true},   // stfd stfdu
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; ++i) {
        if (t[i].op != op)
            continue;
        o->class     = t[i].store ? PPC_CLASS_STORE : PPC_CLASS_LOAD;
        o->is_mem    = true;
        o->is_fp_mem = t[i].fp;
        o->mem_update = t[i].update;
        o->mem_size  = t[i].size;
        return true;
    }
    return false;
}

// Memory shape for the indexed (rA + rB) load/store forms under opcode 31.
static bool indexed_mem(uint16_t xo, PpcInst *o) {
    struct { uint16_t xo, size; bool store, update, fp; } t[] = {
        { 23, 4, false, false, false}, { 55, 4, false, true,  false},  // lwzx  lwzux
        { 87, 1, false, false, false}, {119, 1, false, true,  false},  // lbzx  lbzux
        {279, 2, false, false, false}, {311, 2, false, true,  false},  // lhzx  lhzux
        {343, 2, false, false, false}, {375, 2, false, true,  false},  // lhax  lhaux
        {151, 4, true,  false, false}, {183, 4, true,  true,  false},  // stwx  stwux
        {215, 1, true,  false, false}, {247, 1, true,  true,  false},  // stbx  stbux
        {407, 2, true,  false, false}, {439, 2, true,  true,  false},  // sthx  sthux
        {535, 4, false, false, true},  {567, 4, false, true,  true},   // lfsx  lfsux
        {599, 8, false, false, true},  {631, 8, false, true,  true},   // lfdx  lfdux
        {663, 4, true,  false, true},  {695, 4, true,  true,  true},   // stfsx stfsux
        {727, 8, true,  false, true},  {759, 8, true,  true,  true},   // stfdx stfdux
        {983, 4, true,  false, true},                                  // stfiwx
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; ++i) {
        if (t[i].xo != xo)
            continue;
        o->class      = t[i].store ? PPC_CLASS_STORE : PPC_CLASS_LOAD;
        o->is_mem     = true;
        o->is_fp_mem  = t[i].fp;
        o->mem_update = t[i].update;
        o->mem_indexed = true;
        o->mem_size   = t[i].size;
        return true;
    }
    return false;
}

static void decode_branch_imm(uint32_t w, PpcInst *o) {
    bool aa = (w >> 1) & 1;
    o->class       = PPC_CLASS_BRANCH;
    o->branch      = aa ? PPC_BR_ABSOLUTE : PPC_BR_RELATIVE;
    o->branch_link = w & 1;
    o->writes_pc   = true;
    o->ends_block  = true;

    if (o->primary == 18) {                    // b / ba / bl / bla
        o->branch_cond = false;
        o->branch_disp = sign_extend(w & 0x03FFFFFC, 26);
    } else {                                    // bc (16)
        o->branch_cond = true;
        o->branch_bo   = bits(w, 25, 21);
        o->branch_bi   = bits(w, 20, 16);
        o->branch_disp = sign_extend(w & 0xFFFC, 16);
    }
}

static bool integer_x_alu(uint16_t xo) {
    switch (xo) {
    case 0: case 8: case 10: case 11: case 19: case 24: case 26: case 28: case 32: case 40:
    case 60: case 75: case 104: case 124: case 136: case 138: case 144: case 200: case 202:
    case 232: case 234: case 235: case 266:
    case 284: case 316: case 412: case 444: case 459: case 476: case 491:
    case 536: case 792: case 824: case 922: case 954:
        return true;
    default:
        return false;
    }
}

bool ppc_decode(uint32_t word, PpcInst *out) {
    memset(out, 0, sizeof *out);
    out->raw     = word;
    out->primary = (word >> 26) & 0x3F;
    out->rd      = (word >> 21) & 0x1F;
    out->ra      = (word >> 16) & 0x1F;
    out->rb      = (word >> 11) & 0x1F;
    out->imm     = (int16_t)(word & 0xFFFF);
    out->rc      = word & 1;

    uint8_t op = out->primary;

    switch (op) {
    case 4: { // Paired-single family
        out->class = PPC_CLASS_PS;
        out->xo    = (word >> 1) & 0x3FF;
        uint8_t ps_op = (word >> 1) & 0x3Fu;
        if (ps_op == 6 || ps_op == 7 || ps_op == 38 || ps_op == 39) {
            out->is_mem      = true;
            out->mem_indexed = true;
            out->mem_update  = ps_op == 38 || ps_op == 39;
            out->ps_w        = (word >> 10) & 1u;
            out->ps_i        = (word >> 7) & 7u;
            out->ps_op       = ps_op;
        }
        return true;
    }

    case 7:   // mulli
    case 8:   // subfic
    case 10:  // cmpli
    case 11:  // cmpi
    case 12:  // addic
    case 13:  // addic.
    case 14:  // addi
    case 15:  // addis
    case 20:  // rlwimi
    case 21:  // rlwinm
    case 23:  // rlwnm
    case 24:  // ori
    case 25:  // oris
    case 26:  // xori
    case 27:  // xoris
    case 28:  // andi.
    case 29:  // andis.
        out->class = PPC_CLASS_ALU;
        return true;

    case 16:  // bc
    case 18:  // b
        decode_branch_imm(word, out);
        return true;

    case 17:  // sc
        out->class     = PPC_CLASS_SYSTEM;
        out->writes_pc = true;
        out->ends_block = true;
        return true;

    case 19: {
        out->xo = (word >> 1) & 0x3FF;
        if (out->xo == 16 || out->xo == 528) {  // bclr / bcctr
            out->class       = PPC_CLASS_BRANCH;
            out->branch      = PPC_BR_INDIRECT;
            out->branch_link = word & 1;
            out->branch_bo   = bits(word, 25, 21);
            out->branch_bi   = bits(word, 20, 16);
            // BO 0b1x1xx (== 20 for a plain blr/bctr) is branch always.
            out->branch_cond = (out->branch_bo & 0x14) != 0x14;
            out->writes_pc   = true;
            out->ends_block  = true;
            return true;
        }
        if (out->xo == 50) {                     // rfi
            out->class      = PPC_CLASS_SYSTEM;
            out->writes_pc  = true;
            out->ends_block = true;
            return true;
        }
        out->class = PPC_CLASS_SYSTEM;            // isync, CR-logical ops
        return true;
    }

    case 31: {
        uint16_t xo = (word >> 1) & 0x3FF;
        out->xo = xo;
        if (indexed_mem(xo, out))
            return true;
        if (xo == 339 || xo == 371 || xo == 467) { // mfspr / mftb / mtspr
            out->spr   = decode_spr(word);
            out->class = (xo == 339 && (out->spr == 1 || out->spr == 8 || out->spr == 9))
                       ? PPC_CLASS_ALU : PPC_CLASS_SYSTEM;
            return true;
        }
        // Opcode 31 includes privileged register, cache, and TLB operations.
        out->class = integer_x_alu(xo) ? PPC_CLASS_ALU : PPC_CLASS_SYSTEM;
        return true;
    }

    case 59:  // fadds/fsubs/fmuls/fdivs/fmadds...
    case 63:  // fadd/fsub/fmul/fdiv/fmr/fneg/...
        out->class = PPC_CLASS_FP;
        out->xo    = (word >> 1) & 0x1F;         // A-form primary extended op
        return true;

    case 56:  // psq_l
    case 57:  // psq_lu
        out->class      = PPC_CLASS_PS;
        out->is_mem     = true;
        out->mem_update = (op == 57);
        out->imm        = sign_extend(word & 0xFFFu, 12);
        out->ps_w       = (word >> 15) & 1u;
        out->ps_i       = (word >> 12) & 7u;
        out->ps_op      = op;
        return true;
    case 60:  // psq_st
    case 61:  // psq_stu
        out->class      = PPC_CLASS_PS;
        out->is_mem     = true;
        out->mem_update = (op == 61);
        out->imm        = sign_extend(word & 0xFFFu, 12);
        out->ps_w       = (word >> 15) & 1u;
        out->ps_i       = (word >> 12) & 7u;
        out->ps_op      = op;
        return true;

    default:
        if (direct_mem(op, out))
            return true;
        out->class = PPC_CLASS_ILLEGAL;
        return false;
    }
}

typedef struct {
    const char   *name;
    uint32_t      word;
    PpcClass      class;
    PpcBranchKind branch;
    bool          ends_block;
    bool          is_mem;
    bool          writes_pc;
} DecodeCase;

bool ppc_decode_selftest(void) {
    static const DecodeCase cases[] = {
        {"addi",  0x38630007, PPC_CLASS_ALU,    PPC_BR_NONE,     false, false, false},
        {"mullw", 0x7C6321D6, PPC_CLASS_ALU,    PPC_BR_NONE,     false, false, false},
        {"add",   0x7CA32214, PPC_CLASS_ALU,    PPC_BR_NONE,     false, false, false},
        {"lwz",   0x80640000, PPC_CLASS_LOAD,   PPC_BR_NONE,     false, true,  false},
        {"stw",   0x90640000, PPC_CLASS_STORE,  PPC_BR_NONE,     false, true,  false},
        {"lhz",   0xA0640000, PPC_CLASS_LOAD,   PPC_BR_NONE,     false, true,  false},
        {"lbz",   0x88640000, PPC_CLASS_LOAD,   PPC_BR_NONE,     false, true,  false},
        {"lfd",   0xC8240000, PPC_CLASS_LOAD,   PPC_BR_NONE,     false, true,  false},
        {"stfd",  0xD8240000, PPC_CLASS_STORE,  PPC_BR_NONE,     false, true,  false},
        {"lwzx",  0x7C64282E, PPC_CLASS_LOAD,   PPC_BR_NONE,     false, true,  false},
        {"stwx",  0x7C64292E, PPC_CLASS_STORE,  PPC_BR_NONE,     false, true,  false},
        {"b",     0x48000088, PPC_CLASS_BRANCH, PPC_BR_RELATIVE, true,  false, true},
        {"bl",    0x48000085, PPC_CLASS_BRANCH, PPC_BR_RELATIVE, true,  false, true},
        {"beq",   0x41820080, PPC_CLASS_BRANCH, PPC_BR_RELATIVE, true,  false, true},
        {"bdnz",  0x42000078, PPC_CLASS_BRANCH, PPC_BR_RELATIVE, true,  false, true},
        {"blr",   0x4E800020, PPC_CLASS_BRANCH, PPC_BR_INDIRECT, true,  false, true},
        {"bctr",  0x4E800420, PPC_CLASS_BRANCH, PPC_BR_INDIRECT, true,  false, true},
        {"mflr",  0x7C0802A6, PPC_CLASS_ALU,    PPC_BR_NONE,     false, false, false},
        {"mftb",  0x7C6C42E6, PPC_CLASS_SYSTEM, PPC_BR_NONE,     false, false, false},
        {"crclr", 0x4CC63182, PPC_CLASS_SYSTEM, PPC_BR_NONE,     false, false, false},
        {"mfmsr", 0x7C6000A6, PPC_CLASS_SYSTEM, PPC_BR_NONE,     false, false, false},
        {"mtsrr0",0x7C7A03A6, PPC_CLASS_SYSTEM, PPC_BR_NONE,     false, false, false},
        {"sync",  0x7C0004AC, PPC_CLASS_SYSTEM, PPC_BR_NONE,     false, false, false},
        {"fmuls", 0xEC2100B2, PPC_CLASS_FP,     PPC_BR_NONE,     false, false, false},
        {"fmul",  0xFC6100B2, PPC_CLASS_FP,     PPC_BR_NONE,     false, false, false},
        {"ps_mul",0x10210072, PPC_CLASS_PS,     PPC_BR_NONE,     false, false, false},
        {"psq_l", 0xE0230000, PPC_CLASS_PS,     PPC_BR_NONE,     false, true,  false},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        const DecodeCase *c = &cases[i];
        PpcInst in;
        ppc_decode(c->word, &in);
        if (in.class != c->class || in.branch != c->branch ||
            in.ends_block != c->ends_block || in.is_mem != c->is_mem ||
            in.writes_pc != c->writes_pc) {
            WHBLogPrintf("ppc_decode: %s (0x%08X) class=%d br=%d end=%d mem=%d pc=%d MISMATCH",
                         c->name, c->word, in.class, in.branch,
                         in.ends_block, in.is_mem, in.writes_pc);
            return false;
        }
    }

    PpcInst in;
    ppc_decode(0x7C0802A6, &in);            // mflr r0 -> SPR 8 (LR)
    if (in.spr != 8) {
        WHBLogPrintf("ppc_decode: mflr spr=%u (want 8) MISMATCH", in.spr);
        return false;
    }
    ppc_decode(0x7C6C42E6, &in);            // mftb r3 -> SPR 268 (TBL)
    if (in.spr != 268) {
        WHBLogPrintf("ppc_decode: mftb spr=%u (want 268) MISMATCH", in.spr);
        return false;
    }
    ppc_decode(0x48000088, &in);            // b .+0x88
    if (in.branch_disp != 0x88 || in.branch_link) {
        WHBLogPrintf("ppc_decode: b disp=%d link=%d MISMATCH", in.branch_disp, in.branch_link);
        return false;
    }
    ppc_decode(0x48000085, &in);            // bl .+0x84
    if (in.branch_disp != 0x84 || !in.branch_link) {
        WHBLogPrintf("ppc_decode: bl disp=%d link=%d MISMATCH", in.branch_disp, in.branch_link);
        return false;
    }
    ppc_decode(0xC8260020, &in);            // lfd f1, 0x20(r6)
    if (!in.is_fp_mem || in.mem_size != 8 || in.imm != 0x20 || in.ra != 6) {
        WHBLogPrint("ppc_decode: lfd operand decode MISMATCH");
        return false;
    }
    ppc_decode(0xE023A004, &in);  // psq_l f1,4(r3),1,2
    if (in.class != PPC_CLASS_PS || in.imm != 4 || !in.ps_w || in.ps_i != 2 ||
        in.mem_update || !in.is_mem) {
        WHBLogPrint("ppc_decode: psq_l operand decode MISMATCH");
        return false;
    }
    ppc_decode(0x104327CE, &in);  // psq_stux f2,r3,r4,1,7
    if (in.class != PPC_CLASS_PS || !in.mem_indexed || !in.mem_update || !in.ps_w ||
        in.ps_i != 7 || in.ps_op != 39) {
        WHBLogPrint("ppc_decode: psq_stux operand decode MISMATCH");
        return false;
    }
    return true;
}
