// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "cpu/ppc_interp.h"
#include "cpu/ppc_decode.h"
#include "mem/wii_memory.h"

#include <coreinit/time.h>
#include <whb/log.h>

#include <limits.h>
#include <math.h>
#include <string.h>

// XER carry bit
#define XER_CA 0x20000000u
#define PPC_MSR_EE 0x00008000u
#define PPC_MSR_PR 0x00004000u
#define PPC_SYSTEMCALL_VECTOR 0x80000C00u

#define PPC_SPR_XER   1u
#define PPC_SPR_DEC   22u
#define PPC_SPR_LR    8u
#define PPC_SPR_CTR   9u
#define PPC_SPR_SRR0  26u
#define PPC_SPR_SRR1  27u
#define PPC_SPR_TBL   268u
#define PPC_SPR_TBU   269u
#define PPC_SPR_TBL_W 284u
#define PPC_SPR_TBU_W 285u
#define PPC_SPR_SPRG0 272u
#define PPC_SPR_GQR0  912u

#define WII_TIMEBASE_HZ 60750000u

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

static bool cr_bit(const PpcContext *c, uint8_t bit) {
    return (c->cr >> (31u - bit)) & 1u;
}

static void set_cr_bit(PpcContext *c, uint8_t bit, bool value) {
    uint32_t mask = 1u << (31u - bit);
    c->cr = value ? (c->cr | mask) : (c->cr & ~mask);
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
    case 8: {                                                    // subfc
        uint64_t r = (uint64_t)b + (uint32_t)~a + 1u;
        *rd = (uint32_t)r;
        c->xer = (r >> 32) ? (c->xer | XER_CA) : (c->xer & ~XER_CA);
        break;
    }
    case 10: {                                                   // addc
        uint64_t r = (uint64_t)a + b;
        *rd = (uint32_t)r;
        c->xer = (r >> 32) ? (c->xer | XER_CA) : (c->xer & ~XER_CA);
        break;
    }
    case 136: {                                                  // subfe
        uint64_t r = (uint64_t)b + (uint32_t)~a + ((c->xer & XER_CA) ? 1u : 0u);
        *rd = (uint32_t)r;
        c->xer = (r >> 32) ? (c->xer | XER_CA) : (c->xer & ~XER_CA);
        break;
    }
    case 138: {                                                  // adde
        uint64_t r = (uint64_t)a + b + ((c->xer & XER_CA) ? 1u : 0u);
        *rd = (uint32_t)r;
        c->xer = (r >> 32) ? (c->xer | XER_CA) : (c->xer & ~XER_CA);
        break;
    }
    case 200: {                                                  // subfze
        uint64_t r = (uint64_t)~a + ((c->xer & XER_CA) ? 1u : 0u);
        *rd = (uint32_t)r;
        c->xer = (r >> 32) ? (c->xer | XER_CA) : (c->xer & ~XER_CA);
        break;
    }
    case 202: {                                                  // addze
        uint64_t r = (uint64_t)a + ((c->xer & XER_CA) ? 1u : 0u);
        *rd = (uint32_t)r;
        c->xer = (r >> 32) ? (c->xer | XER_CA) : (c->xer & ~XER_CA);
        break;
    }
    case 232: {                                                  // subfme
        uint64_t r = (uint64_t)~a + ((c->xer & XER_CA) ? 1u : 0u) + 0xFFFFFFFFu;
        *rd = (uint32_t)r;
        c->xer = (r >> 32) ? (c->xer | XER_CA) : (c->xer & ~XER_CA);
        break;
    }
    case 234: {                                                  // addme
        uint64_t r = (uint64_t)a + ((c->xer & XER_CA) ? 1u : 0u) + 0xFFFFFFFFu;
        *rd = (uint32_t)r;
        c->xer = (r >> 32) ? (c->xer | XER_CA) : (c->xer & ~XER_CA);
        break;
    }
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
    case 339:                                                 // mfspr
        if (in->spr == PPC_SPR_XER) c->gpr[in->rd] = c->xer;
        else if (in->spr == PPC_SPR_LR) c->gpr[in->rd] = c->lr;
        else if (in->spr == PPC_SPR_CTR) c->gpr[in->rd] = c->ctr;
        else return false;
        return true;
    case 467:                                                 // mtspr
        if (in->spr == PPC_SPR_XER) c->xer = s;
        else if (in->spr == PPC_SPR_LR) c->lr = s;
        else if (in->spr == PPC_SPR_CTR) c->ctr = s;
        else return false;
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
        bool logical = !(in->xo == 266 || in->xo == 40 || in->xo == 8 ||
                         in->xo == 10 || in->xo == 136 || in->xo == 138 || in->xo == 200 ||
                         in->xo == 202 || in->xo == 232 || in->xo == 234 || in->xo == 235 ||
                         in->xo == 75 || in->xo == 11 || in->xo == 491 ||
                         in->xo == 459 || in->xo == 104);
        set_cr0(c, (int32_t)(logical ? *rA : *rd));
    }
    return true;
}

static uint64_t wii_timebase_ticks(uint64_t host_ticks) {
    uint64_t host_hz = OSSecondsToTicks(1);
    return (host_ticks / host_hz) * WII_TIMEBASE_HZ +
           ((host_ticks % host_hz) * WII_TIMEBASE_HZ) / host_hz;
}

static uint64_t read_timebase(PpcContext *c) {
    uint64_t host_now = (uint64_t)OSGetTime();
    if (c->timebase_host_ticks == 0)
        c->timebase_host_ticks = host_now;
    return c->timebase + wii_timebase_ticks(host_now - c->timebase_host_ticks);
}

static void write_timebase(PpcContext *c, uint64_t value) {
    c->timebase = value;
    c->timebase_host_ticks = (uint64_t)OSGetTime();
}

static void sync_decrementer(PpcContext *c) {
    if (!c->decrementer_armed)
        return;

    uint64_t host_now = (uint64_t)OSGetTime();
    uint64_t elapsed = host_now - c->decrementer_host_ticks;
    uint64_t ticks = wii_timebase_ticks(elapsed);
    if (ticks == 0)
        return;

    c->decrementer_host_ticks = host_now;
    if (ticks >= (uint32_t)c->decrementer) {
        c->decrementer = -1;
        c->decrementer_armed = false;
        c->decrementer_exception_pending = true;
        return;
    }
    c->decrementer -= (int32_t)ticks;
}

bool ppc_decrementer_pending(PpcContext *c) {
    sync_decrementer(c);
    return c->decrementer_exception_pending;
}

void ppc_decrementer_acknowledge_exception(PpcContext *c) {
    c->decrementer_exception_pending = false;
}

static uint32_t read_spr(PpcContext *c, uint32_t spr) {
    switch (spr) {
    case PPC_SPR_XER:  return c->xer;
    case PPC_SPR_DEC:
        sync_decrementer(c);
        return (uint32_t)c->decrementer;
    case PPC_SPR_LR:   return c->lr;
    case PPC_SPR_CTR:  return c->ctr;
    case PPC_SPR_SRR0: return c->srr0;
    case PPC_SPR_SRR1: return c->srr1;
    case PPC_SPR_TBL:  return (uint32_t)read_timebase(c);
    case PPC_SPR_TBU:  return (uint32_t)(read_timebase(c) >> 32);
    default:
        if (spr >= PPC_SPR_SPRG0 && spr < PPC_SPR_SPRG0 + 4u)
            return c->sprg[spr - PPC_SPR_SPRG0];
        if (spr >= PPC_SPR_GQR0 && spr < PPC_SPR_GQR0 + 8u)
            return c->gqr[spr - PPC_SPR_GQR0];
        return 0;
    }
}

static void write_spr(PpcContext *c, uint32_t spr, uint32_t value) {
    switch (spr) {
    case PPC_SPR_XER:  c->xer = value; return;
    case PPC_SPR_DEC:
        c->decrementer = (int32_t)value;
        c->decrementer_host_ticks = (uint64_t)OSGetTime();
        c->decrementer_armed = c->decrementer >= 0;
        c->decrementer_exception_pending = false;
        return;
    case PPC_SPR_LR:   c->lr = value; return;
    case PPC_SPR_CTR:  c->ctr = value; return;
    case PPC_SPR_SRR0: c->srr0 = value; return;
    case PPC_SPR_SRR1: c->srr1 = value; return;
    case PPC_SPR_TBL_W: {
        uint64_t tb = read_timebase(c);
        write_timebase(c, (tb & 0xFFFFFFFF00000000ull) | value);
        return;
    }
    case PPC_SPR_TBU_W: {
        uint64_t tb = read_timebase(c);
        write_timebase(c, ((uint64_t)value << 32) | (uint32_t)tb);
        return;
    }
    default:
        if (spr >= PPC_SPR_SPRG0 && spr < PPC_SPR_SPRG0 + 4u)
            c->sprg[spr - PPC_SPR_SPRG0] = value;
        else if (spr >= PPC_SPR_GQR0 && spr < PPC_SPR_GQR0 + 8u)
            c->gqr[spr - PPC_SPR_GQR0] = value;
        return;
    }
}

static bool exec_system(PpcContext *c, const PpcInst *in, uint32_t *next) {
    if (in->primary == 17) {  // sc
        c->srr0 = c->pc + 4u;
        c->srr1 = c->msr;
        c->msr &= ~(PPC_MSR_EE | PPC_MSR_PR);
        *next = PPC_SYSTEMCALL_VECTOR;
        return true;
    }

    if (in->primary == 19) {
        if (in->xo == 50) {  // rfi
            c->msr = c->srr1;
            *next = c->srr0 & ~3u;
            return true;
        }
        if (in->xo == 150)  // isync
            return true;

        bool a = cr_bit(c, in->ra);
        bool b = cr_bit(c, in->rb);
        bool value;
        switch (in->xo) {
        case 33:  value = !(a | b); break;  // crnor
        case 129: value = a & !b;   break;  // crandc
        case 193: value = a ^ b;    break;  // crxor / crclr
        case 225: value = !(a & b); break;  // crnand
        case 257: value = a & b;    break;  // crand
        case 289: value = !(a ^ b); break;  // creqv / crset
        case 417: value = a | !b;   break;  // crorc
        case 449: value = a | b;    break;  // cror
        default: return false;
        }
        set_cr_bit(c, in->rd, value);
        return true;
    }

    if (in->primary != 31)
        return false;

    switch (in->xo) {
    case 83:   // mfmsr
        c->gpr[in->rd] = c->msr;
        return true;
    case 146:  // mtmsr
        c->msr = c->gpr[in->rd];
        return true;
    case 210:  // mtsr
        c->sr[in->ra & 15u] = c->gpr[in->rd];
        return true;
    case 339:  // mfspr
        c->gpr[in->rd] = read_spr(c, in->spr);
        return true;
    case 371:  // mftb
        if (in->spr != PPC_SPR_TBL && in->spr != PPC_SPR_TBU)
            return false;
        c->gpr[in->rd] = read_spr(c, in->spr);
        return true;
    case 467:  // mtspr
        write_spr(c, in->spr, c->gpr[in->rd]);
        return true;
    case 595:  // mfsr
        c->gpr[in->rd] = c->sr[in->ra & 15u];
        return true;
    case 598:  // sync
    case 854:  // eieio
        return true;
    case 54:   // dcbst
    case 86:   // dcbf
    case 246:  // dcbtst
    case 278:  // dcbt
    case 470:  // dcbi
    case 758:  // dcba
    case 982:  // icbi
        return true;
    case 1014: { // dcbz
        uint32_t ea = ((in->ra == 0) ? 0 : c->gpr[in->ra]) + c->gpr[in->rb];
        ea &= ~31u;
        for (uint32_t i = 0; i < 32; ++i)
            wii_write_u8(ea + i, 0);
        return true;
    }
    default:
        return false;
    }
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

    if (in->primary == 46) {  // lmw
        for (uint32_t r = in->rd; r < 32; ++r)
            c->gpr[r] = wii_read_u32(ea + (r - in->rd) * 4u);
        return true;
    }
    if (in->primary == 47) {  // stmw
        for (uint32_t r = in->rd; r < 32; ++r)
            wii_write_u32(ea + (r - in->rd) * 4u, c->gpr[r]);
        return true;
    }

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
                c->ps1[in->rd] = (double)f;
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

static float ps_dequant_scale(uint8_t scale) {
    return scale <= 31 ? 1.0f / (float)(1ull << scale)
                       : (float)(1ull << (64u - scale));
}

static float ps_quant_scale(uint8_t scale) {
    return scale <= 31 ? (float)(1ull << scale)
                       : 1.0f / (float)(1ull << (64u - scale));
}

static float ps_load_float(uint32_t ea) {
    uint32_t bits = wii_read_u32(ea);
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static void ps_store_float(uint32_t ea, double value) {
    float single = (float)value;
    uint32_t bits;
    memcpy(&bits, &single, sizeof bits);
    wii_write_u32(ea, bits);
}

static double ps_load_quantized(uint32_t ea, uint8_t type, uint8_t scale) {
    float factor = ps_dequant_scale(scale);
    switch (type) {
    case 0: return ps_load_float(ea);
    case 4: return (double)((float)wii_read_u8(ea) * factor);
    case 5: return (double)((float)wii_read_u16(ea) * factor);
    case 6: return (double)((float)(int8_t)wii_read_u8(ea) * factor);
    case 7: return (double)((float)(int16_t)wii_read_u16(ea) * factor);
    default: return 0.0;
    }
}

static uint32_t ps_quant_size(uint8_t type) {
    return type == 0 || type == 5 || type == 7 ? 4u >> (type != 0) : 1u;
}

static void ps_store_quantized(uint32_t ea, uint8_t type, uint8_t scale, double value) {
    if (type == 0) {
        ps_store_float(ea, value);
        return;
    }

    float scaled = (float)value * ps_quant_scale(scale);
    switch (type) {
    case 4: {
        uint8_t v = scaled <= 0.0f ? 0u : scaled >= UCHAR_MAX ? UCHAR_MAX : (uint8_t)scaled;
        wii_write_u8(ea, v);
        break;
    }
    case 5: {
        uint16_t v = scaled <= 0.0f ? 0u : scaled >= USHRT_MAX ? USHRT_MAX : (uint16_t)scaled;
        wii_write_u16(ea, v);
        break;
    }
    case 6: {
        int8_t v = scaled <= SCHAR_MIN ? SCHAR_MIN : scaled >= SCHAR_MAX ? SCHAR_MAX : (int8_t)scaled;
        wii_write_u8(ea, (uint8_t)v);
        break;
    }
    case 7: {
        int16_t v = scaled <= SHRT_MIN ? SHRT_MIN : scaled >= SHRT_MAX ? SHRT_MAX : (int16_t)scaled;
        wii_write_u16(ea, (uint16_t)v);
        break;
    }
    default:
        break;
    }
}

static bool exec_psq(PpcContext *c, const PpcInst *in) {
    const bool indexed = in->mem_indexed;
    uint32_t ea = indexed ? ((in->ra ? c->gpr[in->ra] : 0u) + c->gpr[in->rb])
                          : ((in->ra ? c->gpr[in->ra] : 0u) + (uint32_t)in->imm);
    const bool store = in->ps_op == 60 || in->ps_op == 61 || in->ps_op == 7 || in->ps_op == 39;
    uint32_t gqr = c->gqr[in->ps_i];
    uint8_t type = store ? (uint8_t)(gqr & 7u) : (uint8_t)((gqr >> 16) & 7u);
    uint8_t scale = store ? (uint8_t)((gqr >> 8) & 0x3Fu) : (uint8_t)((gqr >> 24) & 0x3Fu);
    if (type == 1 || type == 2 || type == 3)
        return false;

    uint32_t size = ps_quant_size(type);
    if (store) {
        ps_store_quantized(ea, type, scale, c->fpr[in->rd].d);
        if (!in->ps_w)
            ps_store_quantized(ea + size, type, scale, c->ps1[in->rd]);
    } else {
        c->fpr[in->rd].d = ps_load_quantized(ea, type, scale);
        c->ps1[in->rd] = in->ps_w ? 1.0 : ps_load_quantized(ea + size, type, scale);
    }
    if (in->mem_update)
        c->gpr[in->ra] = ea;
    return true;
}

static void ps_set(PpcContext *c, uint8_t fd, double a, double b) {
    c->fpr[fd].d = (double)(float)a;
    c->ps1[fd] = (double)(float)b;
}

static void ps_compare(PpcContext *c, uint8_t field, double a, double b) {
    uint8_t result = isnan(a) || isnan(b) ? 1u : a < b ? 8u : a > b ? 4u : 2u;
    set_cr_field(c, field, result);
}

static bool exec_ps(PpcContext *c, const PpcInst *in) {
    if (in->is_mem)
        return exec_psq(c, in);

    uint8_t fd = in->rd, fa = in->ra, fb = in->rb, fc = (in->raw >> 6) & 31u;
    double a0 = c->fpr[fa].d, a1 = c->ps1[fa];
    double b0 = c->fpr[fb].d, b1 = c->ps1[fb];
    double c0 = c->fpr[fc].d, c1 = c->ps1[fc];
    switch (in->xo) {
    case 0: ps_compare(c, (in->raw >> 23) & 7u, a0, b0); return true;
    case 32: ps_compare(c, (in->raw >> 23) & 7u, a0, b0); return true;
    case 64: ps_compare(c, (in->raw >> 23) & 7u, a1, b1); return true;
    case 96: ps_compare(c, (in->raw >> 23) & 7u, a1, b1); return true;
    case 40: ps_set(c, fd, -b0, -b1); return true;
    case 72: ps_set(c, fd, b0, b1); return true;
    case 136: ps_set(c, fd, -fabs(b0), -fabs(b1)); return true;
    case 264: ps_set(c, fd, fabs(b0), fabs(b1)); return true;
    case 528: ps_set(c, fd, a0, b0); return true;
    case 560: ps_set(c, fd, a0, b1); return true;
    case 592: ps_set(c, fd, a1, b0); return true;
    case 624: ps_set(c, fd, a1, b1); return true;
    default: break;
    }
    switch ((in->raw >> 1) & 31u) {
    case 10: ps_set(c, fd, a0 + b1, c1); return true;
    case 11: ps_set(c, fd, c0, a0 + b1); return true;
    case 12: ps_set(c, fd, a0 * c0, a1 * c0); return true;
    case 13: ps_set(c, fd, a0 * c1, a1 * c1); return true;
    case 14: ps_set(c, fd, a0 * c0 + b0, a1 * c0 + b1); return true;
    case 15: ps_set(c, fd, a0 * c1 + b0, a1 * c1 + b1); return true;
    case 18: ps_set(c, fd, a0 / b0, a1 / b1); return true;
    case 20: ps_set(c, fd, a0 - b0, a1 - b1); return true;
    case 21: ps_set(c, fd, a0 + b0, a1 + b1); return true;
    case 23: ps_set(c, fd, a0 >= -0.0 ? c0 : b0, a1 >= -0.0 ? c1 : b1); return true;
    case 24: ps_set(c, fd, 1.0 / b0, 1.0 / b1); return true;
    case 25: ps_set(c, fd, a0 * c0, a1 * c1); return true;
    case 26: ps_set(c, fd, 1.0 / sqrt(b0), 1.0 / sqrt(b1)); return true;
    case 28: ps_set(c, fd, a0 * c0 - b0, a1 * c1 - b1); return true;
    case 29: ps_set(c, fd, a0 * c0 + b0, a1 * c1 + b1); return true;
    case 30: ps_set(c, fd, b0 - a0 * c0, b1 - a1 * c1); return true;
    case 31: ps_set(c, fd, -(a0 * c0 + b0), -(a1 * c1 + b1)); return true;
    default: return false;
    }
}

// Returns the branch target, or leaves *taken false to fall through.
static uint32_t eval_branch(PpcContext *c, const PpcInst *in, bool *taken) {
    bool ctr_ok = true, cond_ok = true;
    if (in->branch_cond) {
        uint8_t bo = in->branch_bo;
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
        case PPC_CLASS_PS:
            ok = exec_ps(ctx, &in);
            break;
        case PPC_CLASS_ALU:
            ok = (in.primary == 31) ? exec_integer_x(ctx, &in)
                                    : exec_integer_d(ctx, &in);
            break;
        case PPC_CLASS_SYSTEM:
            ok = exec_system(ctx, &in, &next);
            break;
        default:
            ok = false;
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
    static const uint32_t system_code[] = {
        0x7C7A03A6,  // mtsrr0 r3
        0x7C9B03A6,  // mtsrr1 r4
        0x4C000064,  // rfi
        0x7CA000A6,  // mfmsr r5
    };
    const uint32_t system_ea = 0x90007000;
    for (size_t i = 0; i < sizeof system_code / sizeof system_code[0]; ++i)
        wii_write_u32(system_ea + (uint32_t)i * 4, system_code[i]);

    memset(&ctx, 0, sizeof ctx);
    ctx.pc = system_ea;
    ctx.gpr[3] = system_ea + 12u;
    ctx.gpr[4] = 0x00002000u;
    if (ppc_interp_run(&ctx, system_ea + 16u, 8, &executed) != PPC_INTERP_STOP ||
        ctx.pc != system_ea + 16u || ctx.msr != 0x00002000u ||
        ctx.gpr[5] != 0x00002000u) {
        WHBLogPrintf("ppc_interp: virtual rfi failed pc=0x%08X msr=0x%08X r5=0x%08X",
                     ctx.pc, ctx.msr, ctx.gpr[5]);
        return false;
    }

    static const uint32_t cr_code[] = {
        0x4D000842,  // crnor  cr2lt,cr0lt,cr0gt
        0x4D200902,  // crandc cr2gt,cr0lt,cr0gt
        0x4D400982,  // crxor  cr2eq,cr0lt,cr0gt
        0x4D6009C2,  // crnand cr2so,cr0lt,cr0gt
        0x4D800A02,  // crand  cr3lt,cr0lt,cr0gt
        0x4DA00A42,  // creqv  cr3gt,cr0lt,cr0gt
        0x4DC00B42,  // crorc  cr3eq,cr0lt,cr0gt
        0x4DE00B82,  // cror   cr3so,cr0lt,cr0gt
    };
    for (size_t i = 0; i < sizeof cr_code / sizeof cr_code[0]; ++i)
        wii_write_u32(system_ea + (uint32_t)i * 4, cr_code[i]);
    memset(&ctx, 0, sizeof ctx);
    ctx.pc = system_ea;
    ctx.cr = 0xAAAAAAAAu;
    if (ppc_interp_run(&ctx, system_ea + sizeof cr_code, 16, &executed) != PPC_INTERP_STOP ||
        ctx.cr != 0xAA73AAAAu) {
        WHBLogPrintf("ppc_interp: CR logical result=%08X MISMATCH", ctx.cr);
        return false;
    }

    static const uint32_t timebase_code[] = {
        0x7C9C43A6,  // mttbl r4
        0x7C7D43A6,  // mttbu r3
        0x7CAD42E6,  // mftbu r5
        0x7CCC42E6,  // mftb r6
    };
    for (size_t i = 0; i < sizeof timebase_code / sizeof timebase_code[0]; ++i)
        wii_write_u32(system_ea + (uint32_t)i * 4, timebase_code[i]);
    memset(&ctx, 0, sizeof ctx);
    ctx.pc = system_ea;
    ctx.gpr[3] = 0x12345678u;
    ctx.gpr[4] = 0x9ABCDEF0u;
    if (ppc_interp_run(&ctx, system_ea + sizeof timebase_code, 8, &executed) !=
            PPC_INTERP_STOP || ctx.gpr[5] != 0x12345678u ||
        ctx.gpr[6] < 0x9ABCDEF0u) {
        WHBLogPrintf("ppc_interp: timebase hi=0x%08X lo=0x%08X MISMATCH",
                     ctx.gpr[5], ctx.gpr[6]);
        return false;
    }

    static const uint32_t decrementer_code[] = {
        0x7C7603A6,  // mtdec r3
        0x7C9602A6,  // mfdec r4
    };
    for (size_t i = 0; i < sizeof decrementer_code / sizeof decrementer_code[0]; ++i)
        wii_write_u32(system_ea + (uint32_t)i * 4, decrementer_code[i]);
    memset(&ctx, 0, sizeof ctx);
    ctx.pc = system_ea;
    ctx.gpr[3] = 0x7FFFFFFFu;
    if (ppc_interp_run(&ctx, system_ea + sizeof decrementer_code, 8, &executed) !=
            PPC_INTERP_STOP || !ctx.decrementer_armed || ctx.gpr[4] > ctx.gpr[3]) {
        WHBLogPrintf("ppc_interp: decrementer write/read failed armed=%d dec=%08X",
                     ctx.decrementer_armed, ctx.gpr[4]);
        return false;
    }
    ctx.decrementer = -1;
    ctx.decrementer_armed = false;
    ctx.decrementer_exception_pending = true;
    if (!ppc_decrementer_pending(&ctx)) {
        WHBLogPrint("ppc_interp: decrementer did not become pending");
        return false;
    }
    ppc_decrementer_acknowledge_exception(&ctx);
    if (ppc_decrementer_pending(&ctx)) {
        WHBLogPrint("ppc_interp: decrementer exception was not one-shot");
        return false;
    }

    static const uint32_t branch_code[] = {
        0x2C030000,  // cmpwi r3,0
        0x4182000C,  // beq +12
        0x38800001,  // li r4,1
        0x48000008,  // b +8
        0x38800002,  // li r4,2
    };
    for (size_t i = 0; i < sizeof branch_code / sizeof branch_code[0]; ++i)
        wii_write_u32(system_ea + (uint32_t)i * 4, branch_code[i]);
    memset(&ctx, 0, sizeof ctx);
    ctx.pc = system_ea;
    if (ppc_interp_run(&ctx, system_ea + sizeof branch_code, 8, &executed) !=
            PPC_INTERP_STOP || ctx.gpr[4] != 2u) {
        WHBLogPrintf("ppc_interp: direct beq taken path failed r4=%08X", ctx.gpr[4]);
        return false;
    }
    memset(&ctx, 0, sizeof ctx);
    ctx.pc = system_ea;
    ctx.gpr[3] = 1;
    if (ppc_interp_run(&ctx, system_ea + sizeof branch_code, 8, &executed) !=
            PPC_INTERP_STOP || ctx.gpr[4] != 1u) {
        WHBLogPrintf("ppc_interp: direct beq fallthrough path failed r4=%08X", ctx.gpr[4]);
        return false;
    }

    const uint32_t cache_ea = 0x90008000u;
    wii_write_u32(cache_ea, 0xFFFFFFFFu);
    wii_write_u32(cache_ea + 4u, 0xFFFFFFFFu);
    static const uint32_t cache_code[] = {
        0x7C0018AC,  // dcbf 0,r3
        0x7C001FEC,  // dcbz 0,r3
    };
    for (size_t i = 0; i < sizeof cache_code / sizeof cache_code[0]; ++i)
        wii_write_u32(system_ea + (uint32_t)i * 4, cache_code[i]);
    memset(&ctx, 0, sizeof ctx);
    ctx.pc = system_ea;
    ctx.gpr[3] = cache_ea + 12u;
    if (ppc_interp_run(&ctx, system_ea + sizeof cache_code, 8, &executed) !=
        PPC_INTERP_STOP || wii_read_u32(cache_ea) != 0 || wii_read_u32(cache_ea + 4u) != 0) {
        WHBLogPrint("ppc_interp: cache maintenance failed");
        return false;
    }

    static const uint32_t syscall_vector[] = {
        0x7C9A02A6,  // mfsrr0 r4
        0x38630005,  // addi r3,r3,5
        0x4C000064,  // rfi
    };
    uint32_t saved_vector[sizeof syscall_vector / sizeof syscall_vector[0]];
    for (size_t i = 0; i < sizeof syscall_vector / sizeof syscall_vector[0]; ++i) {
        saved_vector[i] = wii_read_u32(PPC_SYSTEMCALL_VECTOR + (uint32_t)i * 4u);
        wii_write_u32(PPC_SYSTEMCALL_VECTOR + (uint32_t)i * 4u, syscall_vector[i]);
    }
    static const uint32_t syscall_code[] = {
        0x44000002,  // sc
        0x38630001,  // addi r3,r3,1
    };
    for (size_t i = 0; i < sizeof syscall_code / sizeof syscall_code[0]; ++i)
        wii_write_u32(system_ea + (uint32_t)i * 4u, syscall_code[i]);
    memset(&ctx, 0, sizeof ctx);
    ctx.pc = system_ea;
    ctx.gpr[3] = 1;
    ctx.msr = PPC_MSR_EE | PPC_MSR_PR | 0x00002000u;
    const uint32_t syscall_msr = ctx.msr;
    bool syscall_ok = ppc_interp_run(&ctx, system_ea + sizeof syscall_code, 8, &executed) ==
                          PPC_INTERP_STOP &&
                      ctx.gpr[3] == 7 && ctx.gpr[4] == system_ea + 4u &&
                      ctx.srr0 == system_ea + 4u && ctx.srr1 == syscall_msr &&
                      ctx.msr == syscall_msr;
    for (size_t i = 0; i < sizeof syscall_vector / sizeof syscall_vector[0]; ++i)
        wii_write_u32(PPC_SYSTEMCALL_VECTOR + (uint32_t)i * 4u, saved_vector[i]);
    if (!syscall_ok) {
        WHBLogPrintf("ppc_interp: system call vector failed pc=%08X r3=%08X r4=%08X msr=%08X",
                     ctx.pc, ctx.gpr[3], ctx.gpr[4], ctx.msr);
        return false;
    }

    static const uint32_t multi_code[] = {
        0xBB830000,  // lmw r28,0(r3)
        0xBF840000,  // stmw r28,0(r4)
    };
    const uint32_t multi_src = 0x90009000u;
    const uint32_t multi_dst = 0x90009100u;
    for (uint32_t i = 0; i < 4; ++i)
        wii_write_u32(multi_src + i * 4u, 0xA0B0C000u + i);
    for (size_t i = 0; i < sizeof multi_code / sizeof multi_code[0]; ++i)
        wii_write_u32(system_ea + (uint32_t)i * 4, multi_code[i]);
    memset(&ctx, 0, sizeof ctx);
    ctx.pc = system_ea;
    ctx.gpr[3] = multi_src;
    ctx.gpr[4] = multi_dst;
    if (ppc_interp_run(&ctx, system_ea + sizeof multi_code, 8, &executed) != PPC_INTERP_STOP ||
        ctx.gpr[28] != 0xA0B0C000u || ctx.gpr[31] != 0xA0B0C003u ||
        wii_read_u32(multi_dst) != 0xA0B0C000u ||
        wii_read_u32(multi_dst + 12u) != 0xA0B0C003u) {
        WHBLogPrint("ppc_interp: lmw/stmw failed");
        return false;
    }

    static const uint32_t ps_code[] = {
        0xE0230000,  // psq_l f1,0(r3),0,0
        0x10410072,  // ps_mul f2,f1,f1
        0xF0430004,  // psq_st f2,4(r3),0,0
        0xE0638000,  // psq_l f3,0(r3),1,0
    };
    const uint32_t ps_data = 0x9000A000u;
    wii_write_u8(ps_data, 8);
    wii_write_u8(ps_data + 1u, 16);
    for (size_t i = 0; i < sizeof ps_code / sizeof ps_code[0]; ++i)
        wii_write_u32(system_ea + (uint32_t)i * 4u, ps_code[i]);
    memset(&ctx, 0, sizeof ctx);
    ctx.pc = system_ea;
    ctx.gpr[3] = ps_data;
    ctx.gqr[0] = 0x00040006u;  // store S8, load U8
    if (ppc_interp_run(&ctx, system_ea + sizeof ps_code, 8, &executed) != PPC_INTERP_STOP ||
        ctx.fpr[1].d != 8.0 || ctx.ps1[1] != 16.0 || ctx.fpr[2].d != 64.0 ||
        ctx.ps1[2] != 256.0 || wii_read_u8(ps_data + 4u) != 64u ||
        wii_read_u8(ps_data + 5u) != 127u || ctx.fpr[3].d != 8.0 || ctx.ps1[3] != 1.0) {
        WHBLogPrintf("ppc_interp: paired-single quantization failed f1=%d,%d f2=%d,%d f3=%d,%d store=%u,%u",
                     (int)ctx.fpr[1].d, (int)ctx.ps1[1], (int)ctx.fpr[2].d, (int)ctx.ps1[2],
                     (int)ctx.fpr[3].d, (int)ctx.ps1[3], wii_read_u8(ps_data + 4u),
                     wii_read_u8(ps_data + 5u));
        return false;
    }
    return true;
}
