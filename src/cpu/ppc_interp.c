// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "cpu/ppc_interp.h"
#include "cpu/ppc_decode.h"
#include "mem/wii_memory.h"

#include <whb/log.h>

#include <string.h>

// XER carry bit
#define XER_CA 0x20000000u

static void set_cr_field(PpcContext *c, int field, uint8_t val4) {
    int shift = (7 - field) * 4;
    c->cr = (c->cr & ~(0xFu << shift)) | ((uint32_t)val4 << shift);
}

// CR0 from a signed 32-bit result folding in the current XER summary overflow.
static void set_cr0(PpcContext *c, int32_t r) {
    uint8_t f = (r < 0) ? 0x8 : (r > 0) ? 0x4 : 0x2;
    if (c->xer & 0x80000000u)   // SO
        f |= 0x1;
    set_cr_field(c, 0, f);
}

static void cmp_field(PpcContext *c, int field, uint32_t a, uint32_t b, bool sign) {
    uint8_t f;
    if (sign)
        f = ((int32_t)a < (int32_t)b) ? 0x8 : ((int32_t)a > (int32_t)b) ? 0x4 : 0x2;
    else
        f = (a < b) ? 0x8 : (a > b) ? 0x4 : 0x2;
    if (c->xer & 0x80000000u)
        f |= 0x1;
    set_cr_field(c, field, f);
}

static uint32_t rotl32(uint32_t v, int sh) {
    sh &= 31;
    return sh ? (v << sh) | (v >> (32 - sh)) : v;
}

static uint32_t mask32(int mb, int me) {
    // PowerPC MB..ME inclusive bit mask.
    uint32_t m;
    if (mb <= me) {
        m = (me == 31) ? 0xFFFFFFFFu : ((1u << (31 - me)) - 1u) ^ 0xFFFFFFFFu;
        m &= (mb == 0) ? 0xFFFFFFFFu : ((1u << (32 - mb)) - 1u);
    } else {
        uint32_t a = mask32(mb, 31);
        uint32_t b = mask32(0, me);
        m = a | b;
    }
    return m;
}

static bool exec_integer_d(PpcContext *c, const PpcInst *in) {
    uint32_t a = c->gpr[in->ra];
    uint32_t s = c->gpr[in->rd];
    uint16_t uimm = (uint16_t)in->raw;
    switch (in->primary) {
    case 14:  // addi / li
        c->gpr[in->rd] = (in->ra ? a : 0) + (uint32_t)in->imm; return true;
    case 15:  // addis / lis
        c->gpr[in->rd] = (in->ra ? a : 0) + ((uint32_t)in->imm << 16); return true;
    case 7:   // mulli
        c->gpr[in->rd] = (uint32_t)((int32_t)a * in->imm); return true;
    case 12:  // addic
    case 13:  // addic.
        {
            uint64_t r = (uint64_t)a + (uint32_t)in->imm;
            c->gpr[in->rd] = (uint32_t)r;
            c->xer = (r >> 32) ? (c->xer | XER_CA) : (c->xer & ~XER_CA);
            if (in->primary == 13) set_cr0(c, (int32_t)r);
            return true;
        }
    case 8:   // subfic
        {
            uint64_t r = (uint64_t)(uint32_t)in->imm + (uint32_t)(~a) + 1u;
            c->gpr[in->rd] = (uint32_t)r;
            c->xer = (r >> 32) ? (c->xer | XER_CA) : (c->xer & ~XER_CA);
            return true;
        }
    case 11:  // cmpi
        cmp_field(c, (in->raw >> 23) & 7, a, (uint32_t)in->imm, true); return true;
    case 10:  // cmpli
        cmp_field(c, (in->raw >> 23) & 7, a, uimm, false); return true;
    case 24:  // ori
        c->gpr[in->ra] = s | uimm; return true;
    case 25:  // oris
        c->gpr[in->ra] = s | ((uint32_t)uimm << 16); return true;
    case 26:  // xori
        c->gpr[in->ra] = s ^ uimm; return true;
    case 27:  // xoris
        c->gpr[in->ra] = s ^ ((uint32_t)uimm << 16); return true;
    case 28:  // andi.
        c->gpr[in->ra] = s & uimm; set_cr0(c, (int32_t)c->gpr[in->ra]); return true;
    case 29:  // andis.
        c->gpr[in->ra] = s & ((uint32_t)uimm << 16); set_cr0(c, (int32_t)c->gpr[in->ra]); return true;
    case 21:  // rlwinm
        {
            int sh = in->rb, mb = (in->raw >> 6) & 0x1F, me = (in->raw >> 1) & 0x1F;
            c->gpr[in->ra] = rotl32(s, sh) & mask32(mb, me);
            if (in->rc) set_cr0(c, (int32_t)c->gpr[in->ra]);
            return true;
        }
    case 20:  // rlwimi
        {
            int sh = in->rb, mb = (in->raw >> 6) & 0x1F, me = (in->raw >> 1) & 0x1F;
            uint32_t m = mask32(mb, me);
            c->gpr[in->ra] = (rotl32(s, sh) & m) | (c->gpr[in->ra] & ~m);
            if (in->rc) set_cr0(c, (int32_t)c->gpr[in->ra]);
            return true;
        }
    default:
        return false;
    }
}

static bool exec_integer_x(PpcContext *c, const PpcInst *in) {
    uint32_t a = c->gpr[in->ra];
    uint32_t b = c->gpr[in->rb];
    uint32_t s = c->gpr[in->rd];
    uint32_t *rd = &c->gpr[in->rd];   // arithmetic destination
    uint32_t *rA = &c->gpr[in->ra];   // logical/shift destination
    bool rc = in->rc;

    switch (in->xo) {
    case 266: *rd = a + b; break;                               // add
    case 40:  *rd = b - a; break;                               // subf
    case 235: *rd = (uint32_t)((int32_t)a * (int32_t)b); break; // mullw
    case 75:  *rd = (uint32_t)(((int64_t)(int32_t)a * (int32_t)b) >> 32); break;  // mulhw
    case 11:  *rd = (uint32_t)(((uint64_t)a * b) >> 32); break;                   // mulhwu
    case 491: *rd = (int32_t)b ? (uint32_t)((int32_t)a / (int32_t)b) : 0; break;  // divw
    case 459: *rd = b ? (a / b) : 0; break;                    // divwu
    case 104: *rd = (uint32_t)(-(int32_t)a); break;            // neg
    case 28:  *rA = s & b; break;                              // and
    case 444: *rA = s | b; break;                              // or (mr)
    case 316: *rA = s ^ b; break;                              // xor
    case 476: *rA = ~(s & b); break;                           // nand
    case 124: *rA = ~(s | b); break;                           // nor
    case 60:  *rA = s & ~b; break;                             // andc
    case 412: *rA = s | ~b; break;                             // orc
    case 284: *rA = ~(s ^ b); break;                           // eqv
    case 24:  *rA = (b & 0x20) ? 0 : (s << (b & 0x1F)); break; // slw
    case 536: *rA = (b & 0x20) ? 0 : (s >> (b & 0x1F)); break; // srw
    case 792: {                                                // sraw
        int n = b & 0x3F;
        int32_t sv = (int32_t)s;
        if (n >= 32) { *rA = (sv < 0) ? 0xFFFFFFFFu : 0; c->xer = (sv < 0) ? (c->xer | XER_CA) : (c->xer & ~XER_CA); }
        else {
            *rA = (uint32_t)(sv >> n);
            uint32_t lost = s & ((n ? (1u << n) : 1u) - 1u);
            c->xer = (sv < 0 && lost) ? (c->xer | XER_CA) : (c->xer & ~XER_CA);
        }
        break;
    }
    case 824: {                                                // srawi
        int n = in->rb;
        int32_t sv = (int32_t)s;
        *rA = (uint32_t)(sv >> n);
        uint32_t lost = n ? (s & ((1u << n) - 1u)) : 0;
        c->xer = (sv < 0 && lost) ? (c->xer | XER_CA) : (c->xer & ~XER_CA);
        break;
    }
    case 954: *rA = (uint32_t)(int32_t)(int8_t)s; break;       // extsb
    case 922: *rA = (uint32_t)(int32_t)(int16_t)s; break;      // extsh
    case 26: {                                                 // cntlzw
        uint32_t n = 0; while (n < 32 && !(s & (0x80000000u >> n))) ++n; *rA = n; break;
    }
    case 0:   cmp_field(c, (in->raw >> 23) & 7, a, b, true);  return true;  // cmp
    case 32:  cmp_field(c, (in->raw >> 23) & 7, a, b, false); return true;  // cmpl
    case 339:                                                  // mfspr
        c->gpr[in->rd] = (in->spr == 8) ? c->lr : (in->spr == 9) ? c->ctr
                       : (in->spr == 1) ? c->xer : 0;
        return true;
    case 467:                                                  // mtspr
        if (in->spr == 8) c->lr = s; else if (in->spr == 9) c->ctr = s;
        else if (in->spr == 1) c->xer = s;
        return true;
    case 19:  c->gpr[in->rd] = c->cr; return true;             // mfcr
    case 144: {                                                // mtcrf
        uint32_t crm = (in->raw >> 12) & 0xFF, mask = 0;
        for (int i = 0; i < 8; ++i) if (crm & (1u << i)) mask |= 0xFu << (i * 4);
        c->cr = (c->cr & ~mask) | (s & mask);
        return true;
    }
    default:
        return false;
    }

    // The arithmetic/logical/shift group above falls through to here.
    if (rc) {
        bool logical = !(in->xo == 266 || in->xo == 40 || in->xo == 235 ||
                         in->xo == 75 || in->xo == 11 || in->xo == 491 ||
                         in->xo == 459 || in->xo == 104);
        set_cr0(c, (int32_t)(logical ? *rA : *rd));
    }
    return true;
}

static bool exec_fp(PpcContext *c, const PpcInst *in) {
    double a = c->fpr[in->ra].d;
    double b = c->fpr[in->rb].d;
    double frc = c->fpr[(in->raw >> 6) & 0x1F].d;
    bool single = (in->primary == 59);

    // op63 move/convert forms use the full 10-bit extended opcode.
    if (in->primary == 63) {
        switch ((in->raw >> 1) & 0x3FF) {
        case 72:  c->fpr[in->rd].d =  b;        return true;  // fmr
        case 40:  c->fpr[in->rd].d = -b;        return true;  // fneg
        case 264: c->fpr[in->rd].d = (b < 0) ? -b : b; return true;  // fabs
        case 136: c->fpr[in->rd].d = (b < 0) ? b : -b; return true;  // fnabs
        case 12:  c->fpr[in->rd].d = (double)(float)b; return true;  // frsp
        default: break;
        }
    }

    double r;
    switch (in->xo) {              // A-form 5-bit extended opcode
    case 21: r = a + b;            break;  // fadd(s)
    case 20: r = a - b;            break;  // fsub(s)
    case 25: r = a * frc;          break;  // fmul(s)
    case 18: r = a / b;            break;  // fdiv(s)
    case 29: r = a * frc + b;      break;  // fmadd(s)
    case 28: r = a * frc - b;      break;  // fmsub(s)
    case 31: r = -(a * frc + b);   break;  // fnmadd(s)
    case 30: r = -(a * frc - b);   break;  // fnmsub(s)
    default: return false;
    }
    c->fpr[in->rd].d = single ? (double)(float)r : r;
    return true;
}

static bool exec_mem(PpcContext *c, const PpcInst *in) {
    uint32_t base = (in->ra == 0 && !in->mem_update) ? 0 : c->gpr[in->ra];
    uint32_t ea = in->mem_indexed ? base + c->gpr[in->rb] : base + (uint32_t)in->imm;

    bool sign = (in->primary == 42 || in->primary == 43 ||
                 in->xo == 343 || in->xo == 375);   // lha / lhax family

    if (in->class == PPC_CLASS_LOAD) {
        if (in->is_fp_mem) {
            if (in->mem_size == 8) {
                c->fpr[in->rd].u = wii_read_u64(ea);
            } else {
                uint32_t bits = wii_read_u32(ea);
                float f; memcpy(&f, &bits, 4);
                c->fpr[in->rd].d = (double)f;
            }
        } else {
            uint32_t v;
            switch (in->mem_size) {
            case 1: v = wii_read_u8(ea); break;
            case 2: v = sign ? (uint32_t)(int32_t)(int16_t)wii_read_u16(ea)
                             : wii_read_u16(ea); break;
            default: v = wii_read_u32(ea); break;
            }
            c->gpr[in->rd] = v;
        }
    } else {  // STORE
        if (in->is_fp_mem) {
            if (in->mem_size == 8) {
                wii_write_u64(ea, c->fpr[in->rd].u);
            } else {
                float f = (float)c->fpr[in->rd].d;
                uint32_t bits; memcpy(&bits, &f, 4);
                wii_write_u32(ea, bits);
            }
        } else {
            switch (in->mem_size) {
            case 1: wii_write_u8(ea, (uint8_t)c->gpr[in->rd]); break;
            case 2: wii_write_u16(ea, (uint16_t)c->gpr[in->rd]); break;
            default: wii_write_u32(ea, c->gpr[in->rd]); break;
            }
        }
    }

    if (in->mem_update)
        c->gpr[in->ra] = ea;
    return true;
}

// Returns the branch target, or leaves *taken false to fall through.
static uint32_t eval_branch(PpcContext *c, const PpcInst *in, bool *taken) {
    bool ctr_ok = true, cond_ok = true;
    if (in->branch_cond) {
        uint8_t bo = (in->branch == PPC_BR_INDIRECT || in->primary == 16)
                     ? in->branch_bo : 20;
        if (!(bo & 0x04)) {                 // decrement and test CTR
            c->ctr -= 1;
            ctr_ok = (bo & 0x02) ? (c->ctr == 0) : (c->ctr != 0);
        }
        if (!(bo & 0x10)) {                 // test a CR bit
            bool bit = (c->cr >> (31 - in->branch_bi)) & 1;
            cond_ok = (bit == ((bo & 0x08) != 0));
        }
    }
    *taken = ctr_ok && cond_ok;
    if (!*taken)
        return c->pc + 4;

    switch (in->branch) {
    case PPC_BR_RELATIVE: return c->pc + (uint32_t)in->branch_disp;
    case PPC_BR_ABSOLUTE: return (uint32_t)in->branch_disp;
    case PPC_BR_INDIRECT: return (in->xo == 528 ? c->ctr : c->lr) & ~3u;
    default:              return c->pc + 4;
    }
}

PpcInterpResult ppc_interp_run(PpcContext *ctx, uint32_t stop_pc,
                               uint32_t max_insts, uint32_t *executed) {
    uint32_t n = 0;
    PpcInterpResult res = PPC_INTERP_STOP;

    for (; n < max_insts; ++n) {
        if (ctx->pc == stop_pc)
            break;

        PpcInst in;
        if (!ppc_decode(wii_read_u32(ctx->pc), &in)) {
            res = PPC_INTERP_ILLEGAL;
            break;
        }

        bool ok = true;
        uint32_t next = ctx->pc + 4;

        switch (in.class) {
        case PPC_CLASS_BRANCH: {
            uint32_t link = ctx->pc + 4;
            bool taken;
            next = eval_branch(ctx, &in, &taken);
            if (in.branch_link)
                ctx->lr = link;
            break;
        }
        case PPC_CLASS_LOAD:
        case PPC_CLASS_STORE:
            ok = exec_mem(ctx, &in);
            break;
        case PPC_CLASS_FP:
            ok = exec_fp(ctx, &in);
            break;
        case PPC_CLASS_ALU:
            ok = (in.primary == 31) ? exec_integer_x(ctx, &in)
                                    : exec_integer_d(ctx, &in);
            break;
        default:
            ok = false;   // PS / SYSTEM / ILLEGAL
            break;
        }

        if (!ok) {
            res = PPC_INTERP_ILLEGAL;
            break;
        }
        ctx->pc = next;
    }

    if (n == max_insts && ctx->pc != stop_pc)
        res = PPC_INTERP_LIMIT;
    if (executed)
        *executed = n;
    return res;
}

bool ppc_interp_selftest(void) {
    static const uint32_t code[] = {
        0x38600064, 0x38800007, 0x7CA321D6, 0x38A50005,
        0x3CC09000, 0x90A60010, 0x80E60010, 0xC8260020,
        0xC8460028, 0xFC6100B2, 0xD8660030, 0x4E800020,
    };
    const uint32_t code_ea = 0x90001000;   // stage the routine in guest MEM2
    for (size_t i = 0; i < sizeof code / sizeof code[0]; ++i)
        wii_write_u32(code_ea + (uint32_t)i * 4, code[i]);

    union { uint64_t u; double d; } a = {.d = 2.5}, b = {.d = 4.0};
    wii_write_u64(0x90000020, a.u);
    wii_write_u64(0x90000028, b.u);

    PpcContext ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.pc = code_ea;
    ctx.lr = 0xFFFFFFFCu;   // sentinel return address the blr lands on

    uint32_t executed = 0;
    PpcInterpResult r = ppc_interp_run(&ctx, ctx.lr, 64, &executed);
    if (r != PPC_INTERP_STOP) {
        WHBLogPrintf("ppc_interp: run stopped result=%d after %u insts", r, executed);
        return false;
    }

    union { uint64_t u; double d; } out = {.u = wii_read_u64(0x90000030)};
    bool ok = ctx.gpr[5] == 705 && ctx.gpr[7] == 705 &&
              wii_read_u32(0x90000010) == 705 &&
              ctx.fpr[3].d == 10.0 && out.d == 10.0;
    if (!ok) {
        WHBLogPrintf("ppc_interp: r5=%u r7=%u mem=%u f3=%d/10 out=%d/10 MISMATCH",
                     ctx.gpr[5], ctx.gpr[7], wii_read_u32(0x90000010),
                     (int)ctx.fpr[3].d, (int)out.d);
        return false;
    }
    return true;
}
