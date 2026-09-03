// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "cpu/ppc_xlate.h"

#include "audio/wii_audio.h"
#include "cpu/ppc_decode.h"
#include "cpu/ppc_interp.h"
#include "gpu/gx_fifo.h"
#include "ios/ios_ipc.h"
#include "mem/wii_memory.h"
#include "mem/wii_mmio.h"
#include "mem/wii_vi.h"

#include <coreinit/cache.h>
#include <coreinit/codegen.h>
#include <coreinit/core.h>
#include <coreinit/interrupts.h>
#include <coreinit/thread.h>
#include <whb/log.h>

#include <malloc.h>
#include <stddef.h>
#include <string.h>

// The guest context is swapped into the real register file for the duration of a
// block, so its field offsets are baked into the emitted load/store displacements.
_Static_assert(offsetof(PpcContext, gpr) == 0,   "gpr at 0");
_Static_assert(offsetof(PpcContext, fpr) == 128, "fpr at 128");
_Static_assert(offsetof(PpcContext, cr)  == 384, "cr at 384");
_Static_assert(offsetof(PpcContext, xer) == 388, "xer at 388");
_Static_assert(offsetof(PpcContext, lr)  == 392, "lr at 392");
_Static_assert(offsetof(PpcContext, ctr) == 396, "ctr at 396");

#define CTX_G(i)  ((int32_t)((i) * 4))
#define CTX_F(i)  ((int32_t)(128 + (i) * 8))
#define CTX_CR    384
#define CTX_XER   388
#define CTX_LR    392
#define CTX_CTR   396

// r30/r31 are reserved by the prologue/epilogue.
#define REG_WINDOW 30
#define REG_SCRATCH 31

#define PPC_MSR_EE               0x00008000u
#define PPC_EXTERNAL_VECTOR      0x80000500u
#define PPC_DECREMENTER_VECTOR   0x80000900u
#define VI_FIELD_BLOCK_INTERVAL  2048u

// Host non-volatile state saved across a block.
// r13..r31 and f14..f31 are the EABI non-volatiles
// r1 (SP) and r2 are dedicated
// CR2..4 and LR are non-volatile.
typedef struct {
    uint32_t lr;
    uint32_t r1;
    uint32_t r2;
    uint32_t cr;
    uint32_t gpr[19];   // r13..r31
    double   fpr[18];   // f14..f31
} HostSave;

static HostSave s_host_save __attribute__((aligned(16)));

#define HS_LR   offsetof(HostSave, lr)
#define HS_R1   offsetof(HostSave, r1)
#define HS_R2   offsetof(HostSave, r2)
#define HS_CR   offsetof(HostSave, cr)
#define HS_G(r) (offsetof(HostSave, gpr) + ((r) - 13) * 4)
#define HS_F(f) (offsetof(HostSave, fpr) + ((f) - 14) * 8)

// Instruction encoders
static inline uint32_t enc_d(uint32_t op, uint32_t ds, uint32_t a, int32_t disp) {
    return (op << 26) | ((ds & 31) << 21) | ((a & 31) << 16) | ((uint32_t)disp & 0xFFFF);
}
static inline uint32_t enc_x(uint32_t op, uint32_t d, uint32_t a, uint32_t b, uint32_t xo) {
    return (op << 26) | ((d & 31) << 21) | ((a & 31) << 16) | ((b & 31) << 11) | (xo << 1);
}
static inline uint32_t enc_ori(uint32_t a, uint32_t s, uint32_t imm) {
    return (24u << 26) | ((s & 31) << 21) | ((a & 31) << 16) | (imm & 0xFFFF);
}
static inline uint32_t op_lis(uint32_t d, uint32_t hi)      { return enc_d(15, d, 0, (int32_t)(hi & 0xFFFF)); }
static inline uint32_t op_addi(uint32_t d, uint32_t a, int32_t s) { return enc_d(14, d, a, s); }
static inline uint32_t op_lwz(uint32_t d, uint32_t a, int32_t o)  { return enc_d(32, d, a, o); }
static inline uint32_t op_stw(uint32_t s, uint32_t a, int32_t o)  { return enc_d(36, s, a, o); }
static inline uint32_t op_lfd(uint32_t d, uint32_t a, int32_t o)  { return enc_d(50, d, a, o); }
static inline uint32_t op_stfd(uint32_t s, uint32_t a, int32_t o) { return enc_d(54, s, a, o); }
static inline uint32_t op_mflr(uint32_t d)  { return 0x7C0802A6u | ((d & 31) << 21); }
static inline uint32_t op_mtlr(uint32_t s)  { return 0x7C0803A6u | ((s & 31) << 21); }
static inline uint32_t op_mfcr(uint32_t d)  { return 0x7C000026u | ((d & 31) << 21); }
static inline uint32_t op_mtcr(uint32_t s)  { return 0x7C0FF120u | ((s & 31) << 21); }  // mtcrf 0xFF
static inline uint32_t op_mfctr(uint32_t d) { return 0x7C0902A6u | ((d & 31) << 21); }
static inline uint32_t op_mtctr(uint32_t s) { return 0x7C0903A6u | ((s & 31) << 21); }
static inline uint32_t op_mfxer(uint32_t d) { return 0x7C0102A6u | ((d & 31) << 21); }
static inline uint32_t op_mtxer(uint32_t s) { return 0x7C0103A6u | ((s & 31) << 21); }
static inline uint32_t op_mr(uint32_t a, uint32_t s) { return enc_x(31, s, a, s, 444); }
// rlwinm rSCRATCH, rSCRATCH, 0, 3, 31  == mask off the top 3 alias bits
#define OP_MASK_ALIAS 0x57FF00FEu

#define BLK_MAX 8192
static uint32_t s_code[BLK_MAX];
static uint32_t s_len;

static bool emit(uint32_t word) {
    if (s_len >= BLK_MAX)
        return false;
    s_code[s_len++] = word;
    return true;
}
static bool emit_imm32(uint32_t reg, uint32_t val) {
    return emit(op_lis(reg, val >> 16)) && emit(enc_ori(reg, reg, val & 0xFFFF));
}

static bool emit_prologue(uint32_t ctx_addr) {
    bool ok = true;
    // Save host non-volatiles.
    ok &= emit(op_mflr(0));
    ok &= emit(op_mr(12, REG_SCRATCH));
    ok &= emit_imm32(REG_SCRATCH, (uint32_t)(uintptr_t)&s_host_save);
    ok &= emit(op_stw(12, REG_SCRATCH, HS_G(31)));
    ok &= emit(op_stw(0,  REG_SCRATCH, HS_LR));
    ok &= emit(op_stw(1,  REG_SCRATCH, HS_R1));
    ok &= emit(op_stw(2,  REG_SCRATCH, HS_R2));
    for (uint32_t r = 13; r <= 30; ++r)
        ok &= emit(op_stw(r, REG_SCRATCH, HS_G(r)));
    ok &= emit(op_mfcr(0));
    ok &= emit(op_stw(0, REG_SCRATCH, HS_CR));
    for (uint32_t f = 14; f <= 31; ++f)
        ok &= emit(op_stfd(f, REG_SCRATCH, HS_F(f)));

    // Load the guest context into the register file.
    ok &= emit_imm32(REG_SCRATCH, ctx_addr);
    ok &= emit(op_lwz(0, REG_SCRATCH, CTX_XER)); ok &= emit(op_mtxer(0));
    ok &= emit(op_lwz(0, REG_SCRATCH, CTX_CR));  ok &= emit(op_mtcr(0));
    ok &= emit(op_lwz(0, REG_SCRATCH, CTX_LR));  ok &= emit(op_mtlr(0));
    ok &= emit(op_lwz(0, REG_SCRATCH, CTX_CTR)); ok &= emit(op_mtctr(0));
    for (uint32_t f = 0; f < 32; ++f)
        ok &= emit(op_lfd(f, REG_SCRATCH, CTX_F(f)));
    for (uint32_t r = 0; r <= 29; ++r)
        ok &= emit(op_lwz(r, REG_SCRATCH, CTX_G(r)));

    ok &= emit_imm32(REG_WINDOW, (uint32_t)(uintptr_t)wii_mem_fastmem_window());
    return ok;
}

static bool emit_epilogue(uint32_t ctx_addr) {
    bool ok = true;
    // Store the guest results back.
    ok &= emit_imm32(REG_SCRATCH, ctx_addr);
    for (uint32_t r = 0; r <= 29; ++r)
        ok &= emit(op_stw(r, REG_SCRATCH, CTX_G(r)));
    for (uint32_t f = 0; f < 32; ++f)
        ok &= emit(op_stfd(f, REG_SCRATCH, CTX_F(f)));
    ok &= emit(op_mfxer(0)); ok &= emit(op_stw(0, REG_SCRATCH, CTX_XER));
    ok &= emit(op_mfcr(0));  ok &= emit(op_stw(0, REG_SCRATCH, CTX_CR));
    ok &= emit(op_mflr(0));  ok &= emit(op_stw(0, REG_SCRATCH, CTX_LR));
    ok &= emit(op_mfctr(0)); ok &= emit(op_stw(0, REG_SCRATCH, CTX_CTR));

    // Restore host non-volatiles.
    ok &= emit_imm32(REG_SCRATCH, (uint32_t)(uintptr_t)&s_host_save);
    ok &= emit(op_lwz(1, REG_SCRATCH, HS_R1));
    ok &= emit(op_lwz(2, REG_SCRATCH, HS_R2));
    for (uint32_t r = 13; r <= 30; ++r)
        ok &= emit(op_lwz(r, REG_SCRATCH, HS_G(r)));
    ok &= emit(op_lwz(0, REG_SCRATCH, HS_CR)); ok &= emit(op_mtcr(0));
    for (uint32_t f = 14; f <= 31; ++f)
        ok &= emit(op_lfd(f, REG_SCRATCH, HS_F(f)));
    ok &= emit(op_lwz(0, REG_SCRATCH, HS_LR)); ok &= emit(op_mtlr(0));
    ok &= emit(op_lwz(REG_SCRATCH, REG_SCRATCH, HS_G(31)));
    ok &= emit(0x4E800020u);   // blr
    return ok;
}

static bool gpr_reserved(uint8_t r) { return r == REG_WINDOW || r == REG_SCRATCH; }

static bool alu_uses_reserved_gpr(const PpcInst *in) {
    return gpr_reserved(in->rd) || gpr_reserved(in->ra) ||
           (in->primary == 31 && gpr_reserved(in->rb));
}

static bool cache_noop(const PpcInst *in) {
    if (in->primary != 31)
        return false;
    switch (in->xo) {
    case 54: case 86: case 246: case 278: case 470: case 758: case 982:
        return true;
    default:
        return false;
    }
}

// Rewrite one D-form load/store so its EA resolves onto the fastmem window
static bool emit_mem(const PpcInst *in) {
    if (in->mem_indexed || in->mem_size == 0)
        return false;
    if (gpr_reserved(in->ra) || (!in->is_fp_mem && gpr_reserved(in->rd)))
        return false;

    uint32_t xo;
    if (in->class == PPC_CLASS_LOAD) {
        if (in->is_fp_mem)        xo = (in->mem_size == 8) ? 599 : 535;   // lfdx / lfsx
        else if (in->mem_size == 1) xo = 87;                              // lbzx
        else if (in->mem_size == 2) xo = (in->primary == 42) ? 343 : 279; // lhax / lhzx
        else                        xo = 23;                              // lwzx
    } else {
        if (in->is_fp_mem)        xo = (in->mem_size == 8) ? 727 : 663;   // stfdx / stfsx
        else if (in->mem_size == 1) xo = 215;                             // stbx
        else if (in->mem_size == 2) xo = 407;                             // sthx
        else                        xo = 151;                             // stwx
    }

    bool ok = true;
    ok &= emit(op_addi(REG_SCRATCH, in->ra, in->imm & 0xFFFF));
    ok &= emit(OP_MASK_ALIAS);
    ok &= emit(enc_x(31, in->rd, REG_WINDOW, REG_SCRATCH, xo));
    if (in->mem_update)
        ok &= emit(op_addi(in->ra, in->ra, in->imm & 0xFFFF));
    return ok;
}

static PpcXlateResult emit_block(const uint32_t *guest, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        PpcInst in;
        if (!ppc_decode(guest[i], &in))
            return PPC_XLATE_UNSUPPORTED;

        switch (in.class) {
        case PPC_CLASS_ALU:
            if (alu_uses_reserved_gpr(&in))
                return PPC_XLATE_UNSUPPORTED;
            if (!emit(guest[i]))
                return PPC_XLATE_ERROR;
            break;
        case PPC_CLASS_FP:
            if (!emit(guest[i]))   // FP operands index FPRs
                return PPC_XLATE_ERROR;
            break;
        case PPC_CLASS_LOAD:
        case PPC_CLASS_STORE:
            if (!emit_mem(&in))
                return PPC_XLATE_UNSUPPORTED;
            break;
        case PPC_CLASS_SYSTEM:
            if (!cache_noop(&in))
                return PPC_XLATE_UNSUPPORTED;
            break;
        default:
            return PPC_XLATE_UNSUPPORTED;   // branch / system / paired-single
        }
    }
    return PPC_XLATE_OK;
}

// Codegen drive

static void          *s_area;
static uint32_t       s_area_size;
static PpcXlateResult s_run_status;

// Copy `words` of translated code into the codegen area at `dst`.
static bool codegen_commit(void *dst, const uint32_t *code, uint32_t words) {
    uint32_t bytes = words * 4;
    if (!OSSwitchSecCodeGenMode(CODEGEN_RW_)) {
        WHBLogPrint("cpu_xlate: CODEGEN_RW_ failed");
        return false;
    }
    memcpy(dst, code, bytes);
    DCFlushRange(dst, bytes);
    if (!OSSwitchSecCodeGenMode(CODEGEN_R_X)) {
        WHBLogPrint("cpu_xlate: CODEGEN_R_X failed");
        return false;
    }
    ICInvalidateRange(dst, bytes);
    __asm__ volatile("isync");
    return true;
}

static int xlateThreadEntry(int argc, const char **argv) {
    (void)argc; (void)argv;
    if (!codegen_commit(s_area, s_code, s_len)) {
        s_run_status = PPC_XLATE_ERROR;
        return 0;
    }

    void (*fn)(void) = (void (*)(void))(uintptr_t)s_area;

    BOOL irq = OSDisableInterrupts();
    fn();
    OSRestoreInterrupts(irq);

    s_run_status = PPC_XLATE_OK;
    return 0;
}

static PpcXlateResult run_on_codegen_core(void) {
    OSGetCodegenVirtAddrRange(&s_area, &s_area_size);
    if (!s_area || s_area_size < s_len * 4)
        return PPC_XLATE_UNAVAILABLE;

    static OSThread thread;
    const uint32_t stackSize = 64u * 1024u;
    uint8_t *stack = memalign(16, stackSize);
    if (!stack)
        return PPC_XLATE_ERROR;

    OSThreadAttributes affinity = (OSThreadAttributes)(1u << OSGetCodegenCore());
    s_run_status = PPC_XLATE_ERROR;
    if (!OSCreateThread(&thread, xlateThreadEntry, 0, NULL,
                        stack + stackSize, stackSize, 16, affinity)) {
        free(stack);
        return PPC_XLATE_ERROR;
    }
    OSResumeThread(&thread);
    int ret = 0;
    OSJoinThread(&thread, &ret);
    free(stack);
    return s_run_status;
}

PpcXlateResult ppc_xlate_run_block(PpcContext *ctx, uint32_t guest_pc,
                                   const uint32_t *guest, uint32_t count) {
    (void)guest_pc;
    if (!wii_mem_fastmem_window())
        return PPC_XLATE_UNAVAILABLE;

    uint32_t ctx_addr = (uint32_t)(uintptr_t)ctx;
    s_len = 0;
    if (!emit_prologue(ctx_addr))
        return PPC_XLATE_ERROR;

    PpcXlateResult er = emit_block(guest, count);
    if (er != PPC_XLATE_OK)
        return er;

    if (!emit_epilogue(ctx_addr))
        return PPC_XLATE_ERROR;

    return run_on_codegen_core();
}

// Block cache + dispatcher
#define MAX_BLOCK_INSTS 256u

typedef enum {
    TERM_INTERP,       // branch / system / an op the emitter can't handle
    TERM_MMIO,         // a memory op whose static EA lands in hardware registers
    TERM_FALLTHROUGH,  // ran to the block length cap with no terminator
} TermKind;

typedef struct {
    bool     used;
    uint32_t start_pc;
    uint32_t code_off;      // byte offset of the body in the codegen area
    uint32_t guest_count;   // guest instructions emitted in the body
    uint32_t term_pc;       // guest PC of the terminator
    TermKind term_kind;
    PpcInst  term;          // decoded terminator
} XBlock;

#define XCACHE_CAP 4096u
static XBlock   s_cache[XCACHE_CAP];
static uint32_t s_codegen_bump;

// Session parameters and results
static PpcContext   *s_ctx;
static uint32_t      s_entry, s_stop, s_max_blocks;
static bool          s_run_to_gx;
static PpcXlateSession s_session;
static PpcXlateResult  s_session_status;

static bool take_external_interrupt(PpcContext *c, uint32_t *pc) {
    if (!(c->msr & PPC_MSR_EE) || !wii_mmio_irq_pending())
        return false;

    if (wii_read_u32(PPC_EXTERNAL_VECTOR) == 0)
        return false;

    c->srr0 = *pc;
    c->srr1 = c->msr;
    c->msr &= ~PPC_MSR_EE;
    c->pc = PPC_EXTERNAL_VECTOR;
    *pc = PPC_EXTERNAL_VECTOR;
    return true;
}

static bool take_decrementer_interrupt(PpcContext *c, uint32_t *pc) {
    if (!(c->msr & PPC_MSR_EE) || !ppc_decrementer_pending(c))
        return false;

    ppc_decrementer_acknowledge_exception(c);
    c->srr0 = *pc;
    c->srr1 = c->msr;
    c->msr &= ~PPC_MSR_EE;
    c->pc = PPC_DECREMENTER_VECTOR;
    *pc = PPC_DECREMENTER_VECTOR;
    return true;
}

static XBlock *cache_find(uint32_t pc) {
    uint32_t i = (pc >> 2) % XCACHE_CAP;
    for (uint32_t n = 0; n < XCACHE_CAP; ++n) {
        XBlock *b = &s_cache[i];
        if (!b->used) return NULL;
        if (b->start_pc == pc) return b;
        i = (i + 1) % XCACHE_CAP;
    }
    return NULL;
}

static XBlock *cache_alloc(uint32_t pc) {
    uint32_t i = (pc >> 2) % XCACHE_CAP;
    for (uint32_t n = 0; n < XCACHE_CAP; ++n) {
        XBlock *b = &s_cache[i];
        if (!b->used) {
            b->used = true;
            b->start_pc = pc;
            return b;
        }
        i = (i + 1) % XCACHE_CAP;
    }
    return NULL;   // cache full
}

// Enough to recognise a memory op whose base register was just loaded with an immediate (lis/li/addi/ori).
// A separate conservative bit catches common runtime indexed hardware register bases.
static void track_alu(uint32_t *cval, bool *known, bool *maybe_mmio, const PpcInst *in) {
    switch (in->primary) {
    case 14:  // addi / li
        if (in->ra == 0)            { cval[in->rd] = (uint32_t)in->imm; known[in->rd] = true; maybe_mmio[in->rd] = false; }
        else if (known[in->ra])     { cval[in->rd] = cval[in->ra] + (uint32_t)in->imm; known[in->rd] = true; maybe_mmio[in->rd] = false; }
        else                        { known[in->rd] = false; maybe_mmio[in->rd] = maybe_mmio[in->ra]; }
        return;
    case 15:  // addis / lis
        if (in->ra == 0)            { cval[in->rd] = (uint32_t)in->imm << 16; known[in->rd] = true; maybe_mmio[in->rd] = false; }
        else if (known[in->ra])     { cval[in->rd] = cval[in->ra] + ((uint32_t)in->imm << 16); known[in->rd] = true; maybe_mmio[in->rd] = false; }
        else                        { known[in->rd] = false; maybe_mmio[in->rd] = maybe_mmio[in->ra]; }
        if ((uint16_t)in->imm == 0xCC00u || (uint16_t)in->imm == 0xCD00u)
            maybe_mmio[in->rd] = true;
        return;
    case 24:  // ori
        if (known[in->rd])          { cval[in->ra] = cval[in->rd] | (uint16_t)in->raw; known[in->ra] = true; maybe_mmio[in->ra] = maybe_mmio[in->rd]; }
        else                        { known[in->ra] = false; maybe_mmio[in->ra] = maybe_mmio[in->rd]; }
        return;
    case 25:  // oris
        if (known[in->rd])          { cval[in->ra] = cval[in->rd] | ((uint32_t)(uint16_t)in->raw << 16); known[in->ra] = true; maybe_mmio[in->ra] = maybe_mmio[in->rd]; }
        else                        { known[in->ra] = false; maybe_mmio[in->ra] = maybe_mmio[in->rd]; }
        return;
    default:
        {
        bool any_mmio = false;
        for (uint32_t r = 0; r < 32; ++r)
            any_mmio |= maybe_mmio[r];
        known[in->rd] = false;
        known[in->ra] = false;
        maybe_mmio[in->rd] |= any_mmio;
        maybe_mmio[in->ra] |= any_mmio;
        return;
        }
    }
}

// Emit the straight-line body starting at `start_pc` into s_code.
static bool emit_body(uint32_t start_pc, uint32_t *guest_count,
                      TermKind *tk, uint32_t *term_pc, PpcInst *term) {
    uint32_t cval[32];
    bool     known[32];
    bool     maybe_mmio[32];
    memset(known, 0, sizeof known);
    for (uint32_t r = 0; r < 32; ++r)
        maybe_mmio[r] = wii_ea_is_mmio(s_ctx->gpr[r]);

    for (uint32_t i = 0;; ++i) {
        if (i >= MAX_BLOCK_INSTS) {
            *tk = TERM_FALLTHROUGH;
            *term_pc = start_pc + i * 4;
            *guest_count = i;
            return true;
        }
        uint32_t pc = start_pc + i * 4;
        uint32_t word = wii_read_u32(pc);
        PpcInst in;
        if (!ppc_decode(word, &in)) {
            *tk = TERM_INTERP; *term_pc = pc; *term = in; *guest_count = i; return true;
        }

        switch (in.class) {
        case PPC_CLASS_ALU:
            if (alu_uses_reserved_gpr(&in)) {
                *tk = TERM_INTERP; *term_pc = pc; *term = in; *guest_count = i; return true;
            }
            if (!emit(word)) return false;
            track_alu(cval, known, maybe_mmio, &in);
            break;
        case PPC_CLASS_FP:
            if (!emit(word)) return false;
            break;
        case PPC_CLASS_LOAD:
        case PPC_CLASS_STORE: {
            if (in.mem_size == 0) {
                *tk = TERM_INTERP; *term_pc = pc; *term = in; *guest_count = i; return true;
            }
            bool ea_known = false;
            uint32_t ea = 0;
            if (!in.mem_indexed) {
                if (in.ra == 0)          { ea_known = true; ea = (uint32_t)in.imm; }
                else if (known[in.ra])   { ea_known = true; ea = cval[in.ra] + (uint32_t)in.imm; }
            }
            if ((ea_known && wii_ea_is_mmio(ea)) || maybe_mmio[in.ra]) {
                *tk = TERM_MMIO; *term_pc = pc; *term = in; *guest_count = i; return true;
            }
            if (!emit_mem(&in)) {   // indexed or reserved register
                *tk = TERM_INTERP; *term_pc = pc; *term = in; *guest_count = i; return true;
            }
            if (in.class == PPC_CLASS_LOAD && !in.is_fp_mem) { known[in.rd] = false; maybe_mmio[in.rd] = false; }
            if (in.mem_update) { known[in.ra] = false; maybe_mmio[in.ra] = false; }
            break;
        }
        case PPC_CLASS_SYSTEM:
            if (!cache_noop(&in)) {
                *tk = TERM_INTERP; *term_pc = pc; *term = in; *guest_count = i; return true;
            }
            break;
        default:   // BRANCH / SYSTEM / PS
            *tk = TERM_INTERP; *term_pc = pc; *term = in; *guest_count = i; return true;
        }
    }
}

// Effective address of a memory op from the live context.
static uint32_t mem_ea(const PpcContext *c, const PpcInst *in) {
    uint32_t base = (in->ra == 0 && !in->mem_update) ? 0 : c->gpr[in->ra];
    return in->mem_indexed ? base + c->gpr[in->rb] : base + (uint32_t)in->imm;
}

// Service one hardware register access by routing WGP writes to the GX FIFO capture
// and IPC control writes to the IOS dispatcher.
static void service_mmio(PpcContext *c, const PpcInst *in) {
    uint32_t ea = mem_ea(c, in);
    uint32_t size = in->mem_size;

    if (in->class == PPC_CLASS_STORE) {
        uint64_t val;
        if (in->is_fp_mem) {
            if (size == 8) {
                val = c->fpr[in->rd].u;
            } else {
                float f = (float)c->fpr[in->rd].d;
                uint32_t bits; memcpy(&bits, &f, 4); val = bits;
            }
        } else {
            val = c->gpr[in->rd];
        }
        wii_mmio_write(ea, val, size);
        if (wii_ea_is_wgp(ea) && s_session.first_gx_write_ea == 0)
            s_session.first_gx_write_ea = ea;
    } else {
        uint32_t v = wii_mmio_read(ea, size);
        if (in->is_fp_mem) {
            if (size == 8) c->fpr[in->rd].u = (uint64_t)v << 32;
            else { float f; uint32_t bits = v; memcpy(&f, &bits, 4); c->fpr[in->rd].d = (double)f; }
        } else {
            if (size == 1) {
                v &= 0xFF;
            } else if (size == 2) {
                bool sign = in->primary == 42 || in->primary == 43 ||
                            in->xo == 343 || in->xo == 375;
                v = sign ? (uint32_t)(int32_t)(int16_t)v : v & 0xFFFFu;
            }
            c->gpr[in->rd] = v;
        }
    }
    if (in->mem_update)
        c->gpr[in->ra] = ea;
}

// From an MMIO terminator service that access and every immediately following
// memory op whose live EA is also MMIO.
static bool service_mmio_run(PpcContext *c) {
    for (;;) {
        PpcInst in;
        if (!ppc_decode(wii_read_u32(c->pc), &in))
            return false;
        if (in.class != PPC_CLASS_LOAD && in.class != PPC_CLASS_STORE)
            return false;
        if (!wii_ea_is_mmio(mem_ea(c, &in)))
            return false;
        service_mmio(c, &in);
        c->pc += 4;
        if (s_run_to_gx && s_session.first_gx_write_ea != 0)
            return true;
    }
}

// Ensure a block for `pc` exists in the cache, compiling and committing it if not.
static XBlock *get_block(uint32_t pc) {
    XBlock *b = cache_find(pc);
    if (b) { s_session.cache_hits++; return b; }
    s_session.cache_misses++;

    b = cache_alloc(pc);
    if (!b) { s_session_status = PPC_XLATE_ERROR; return NULL; }

    s_len = 0;
    if (!emit_prologue((uint32_t)(uintptr_t)s_ctx)) { s_session_status = PPC_XLATE_ERROR; return NULL; }
    TermKind tk; uint32_t tpc, gcount; PpcInst term;
    if (!emit_body(pc, &gcount, &tk, &tpc, &term))  { s_session_status = PPC_XLATE_ERROR; return NULL; }
    if (!emit_epilogue((uint32_t)(uintptr_t)s_ctx)) { s_session_status = PPC_XLATE_ERROR; return NULL; }

    uint32_t off = s_codegen_bump;
    if (off + s_len * 4 > s_area_size) { s_session_status = PPC_XLATE_UNAVAILABLE; return NULL; }
    if (!codegen_commit((uint8_t *)s_area + off, s_code, s_len)) {
        s_session_status = PPC_XLATE_ERROR; return NULL;
    }
    s_codegen_bump = (off + s_len * 4 + 31u) & ~31u;

    b->code_off = off;
    b->guest_count = gcount;
    b->term_kind = tk;
    b->term_pc = tpc;
    b->term = term;
    return b;
}

static void session_run(void) {
    OSGetCodegenVirtAddrRange(&s_area, &s_area_size);
    if (!s_area || !wii_mem_fastmem_window()) { s_session_status = PPC_XLATE_UNAVAILABLE; return; }

    memset(s_cache, 0, sizeof s_cache);
    memset(&s_session, 0, sizeof s_session);
    s_codegen_bump = 0;
    s_session.stop = PPC_XSTOP_BUDGET;
    s_session_status = PPC_XLATE_OK;
    PpcContext *c = s_ctx;
    uint32_t pc = s_entry;

    while (s_session.blocks_run < s_max_blocks) {
        if (pc == s_stop) {
            c->pc = pc;
            s_session.stop = PPC_XSTOP_STOP_PC;
            return;
        }

        if (s_session.blocks_run && (s_session.blocks_run % VI_FIELD_BLOCK_INTERVAL) == 0)
            wii_vi_tick_vblank();
        if (!take_external_interrupt(c, &pc))
            take_decrementer_interrupt(c, &pc);

        XBlock *b = get_block(pc);
        if (!b) return;

        void (*fn)(void) = (void (*)(void))(uintptr_t)((uint8_t *)s_area + b->code_off);
        BOOL irq = OSDisableInterrupts();
        fn();
        OSRestoreInterrupts(irq);
        s_session.blocks_run++;

        switch (b->term_kind) {
        case TERM_FALLTHROUGH:
            pc = b->term_pc;
            break;
        case TERM_MMIO:
            c->pc = b->term_pc;
            if (service_mmio_run(c)) { s_session.stop = PPC_XSTOP_GX_WRITE; return; }
            if (c->pc == b->term_pc) {
                uint32_t ex = 0;
                uint32_t nostop = b->term_pc + 2;
                if (ppc_interp_run(c, nostop, 1, &ex) == PPC_INTERP_ILLEGAL || ex == 0) {
                    PpcInst fi;
                    if (!ppc_decode(wii_read_u32(b->term_pc), &fi)) fi.class = PPC_CLASS_ILLEGAL;
                    s_session.stop = PPC_XSTOP_FAULT;
                    s_session.last_pc = b->term_pc;
                    s_session.last_word = wii_read_u32(b->term_pc);
                    s_session.last_class = (uint8_t)fi.class;
                    return;
                }
            }
            pc = c->pc;
            break;
        case TERM_INTERP: {
            c->pc = b->term_pc;
            uint32_t nostop = b->term_pc + 2;
            uint32_t ex = 0;
            if (ppc_interp_run(c, nostop, 1, &ex) == PPC_INTERP_ILLEGAL || ex == 0) {
                PpcInst fi;
                if (!ppc_decode(wii_read_u32(b->term_pc), &fi)) fi.class = PPC_CLASS_ILLEGAL;
                s_session.stop = PPC_XSTOP_FAULT;
                s_session.last_pc = b->term_pc;
                s_session.last_word = wii_read_u32(b->term_pc);
                s_session.last_class = (uint8_t)fi.class;
                return;
            }
            pc = c->pc;
            break;
        }
        }
    }
    c->pc = pc;
    s_session.last_pc = pc;
}

static int sessionThreadEntry(int argc, const char **argv) {
    (void)argc; (void)argv;
    session_run();
    return 0;
}

PpcXlateResult ppc_xlate_run(PpcContext *ctx, uint32_t entry_pc, uint32_t stop_pc,
                             uint32_t max_blocks, PpcXlateSession *out) {
    if (!wii_mem_fastmem_window())
        return PPC_XLATE_UNAVAILABLE;

    s_ctx = ctx;
    s_entry = entry_pc;
    s_stop = stop_pc;
    s_max_blocks = max_blocks;
    s_run_to_gx = (stop_pc == PPC_XLATE_RUN_TO_GX);

    static OSThread thread;
    const uint32_t stackSize = 128u * 1024u;
    uint8_t *stack = memalign(16, stackSize);
    if (!stack)
        return PPC_XLATE_ERROR;

    OSThreadAttributes affinity = (OSThreadAttributes)(1u << OSGetCodegenCore());
    s_session_status = PPC_XLATE_ERROR;
    if (!OSCreateThread(&thread, sessionThreadEntry, 0, NULL,
                        stack + stackSize, stackSize, 16, affinity)) {
        free(stack);
        return PPC_XLATE_ERROR;
    }
    OSResumeThread(&thread);
    int ret = 0;
    OSJoinThread(&thread, &ret);
    free(stack);

    if (out)
        *out = s_session;
    return s_session_status;
}

// Self test
static bool ctx_equal(const PpcContext *a, const PpcContext *b) {
    for (int i = 0; i < 32; ++i)
        if (a->gpr[i] != b->gpr[i]) return false;
    for (int i = 0; i < 32; ++i)
        if (a->fpr[i].u != b->fpr[i].u) return false;
    return a->cr == b->cr && a->xer == b->xer && a->lr == b->lr && a->ctr == b->ctr;
}

static void seed_common(PpcContext *ctx) {
    memset(ctx, 0, sizeof *ctx);
    ctx->fpr[1].d = 2.5;
    ctx->fpr[2].d = 4.0;
}

bool ppc_xlate_identity_selftest(void) {
    // li r3,100 | li r4,7 | mullw r5,r3,r4 | addi r5,r5,5 |
    // addic r10,r3,-1 | subfe r3,r10,r3 | addze/addme/subfze/subfme | fmul f3,f1,f2
    static const uint32_t block[] = {
        0x38600064, 0x38800007, 0x7CA321D6, 0x38A50005,
        0x3143FFFF, 0x7C6A1910, 0x7C830194, 0x7CA301D4,
        0x7CC30190, 0x7CE301D0, 0xFC6100B2,
    };
    const uint32_t pc = 0x80004000;

    PpcContext want; seed_common(&want);
    want.pc = pc;
    for (size_t i = 0; i < sizeof block / sizeof block[0]; ++i)
        wii_write_u32(pc + (uint32_t)i * 4, block[i]);
    uint32_t executed = 0;
    if (ppc_interp_run(&want, pc + sizeof block, 32, &executed) != PPC_INTERP_STOP) {
        WHBLogPrint("cpu_xlate: identity oracle did not reach block end");
        return false;
    }

    PpcContext got; seed_common(&got);
    PpcXlateResult r = ppc_xlate_run_block(&got, pc, block, sizeof block / sizeof block[0]);
    if (r != PPC_XLATE_OK) {
        WHBLogPrintf("cpu_xlate: identity block build/run failed (%d)", r);
        return false;
    }

    WHBLogPrintf("cpu_xlate: identity block core=%u built=%uB ran OK",
                 OSGetCodegenCore(), s_len * 4);
    if (!ctx_equal(&want, &got) || want.gpr[3] != 1 || want.gpr[4] != 2 ||
        want.gpr[5] != 0 || want.gpr[6] != 0xFFFFFFFFu || want.gpr[7] != 0xFFFFFFFDu ||
        (want.xer & 0x20000000u) == 0) {
        WHBLogPrintf("cpu_xlate: identity r5 got=%u want=%u f3 got=%d/1000 want=%d/1000 MISMATCH",
                     got.gpr[5], want.gpr[5],
                     (int)(got.fpr[3].d * 1000.0), (int)(want.fpr[3].d * 1000.0));
        return false;
    }

    static const uint32_t reserved_source[] = {0x7CA3F214};  // add r5,r3,r30
    if (emit_block(reserved_source, 1) != PPC_XLATE_UNSUPPORTED) {
        WHBLogPrint("cpu_xlate: reserved rB was emitted");
        return false;
    }
    return true;
}

bool ppc_xlate_memblock_selftest(void) {
    if (!wii_mem_fastmem_window()) {
        WHBLogPrint("cpu_xlate: memblock scheme=a window UNAVAILABLE");
        return false;
    }

    // Seed source data at a cached MEM2 EA and an uncached MEM1 EA.
    const uint32_t src2 = 0x90002000;   // MEM2
    wii_write_u32(src2 + 0, 0xDEADBEEFu);
    union { uint64_t u; double d; } dv = {.d = 3.5};
    wii_write_u64(src2 + 8, dv.u);
    wii_write_u32(0xC0001000u, 0);      // clear the MEM1 store target

    // lis r3,0x9000 | ori r3,r3,0x2000 -> r3 = 0x90002000
    // lwz r4,0(r3)          load word from MEM2 cached
    // lfd f1,8(r3)          load double from MEM2 cached
    // lis r5,0xC000 | ori r5,r5,0x1000 -> r5 = 0xC0001000  (MEM1 uncached alias)
    // stw r4,0(r5)          store word to MEM1 uncached
    // stfd f1,16(r5)        store double to MEM1 uncached
    static const uint32_t block[] = {
        0x3C609000, 0x60632000, 0x80830000, 0xC8230008,
        0x3CA0C000, 0x60A51000, 0x90A50000, 0xD82A0010,
    };
    const uint32_t pc = 0x80005000;

    PpcContext want; memset(&want, 0, sizeof want);
    want.pc = pc;
    for (size_t i = 0; i < sizeof block / sizeof block[0]; ++i)
        wii_write_u32(pc + (uint32_t)i * 4, block[i]);
    uint32_t executed = 0;
    if (ppc_interp_run(&want, pc + sizeof block, 32, &executed) != PPC_INTERP_STOP) {
        WHBLogPrint("cpu_xlate: memblock oracle did not reach block end");
        return false;
    }
    // Snapshot the memory the oracle wrote then reset those cells so the translated
    // run must reproduce them.
    uint32_t want_w = wii_read_u32(0xC0001000u);
    uint64_t want_d = wii_read_u64(0xC0001010u);
    wii_write_u32(0xC0001000u, 0);
    wii_write_u64(0xC0001010u, 0);

    PpcContext got; memset(&got, 0, sizeof got);
    PpcXlateResult r = ppc_xlate_run_block(&got, pc, block, sizeof block / sizeof block[0]);
    if (r != PPC_XLATE_OK) {
        WHBLogPrintf("cpu_xlate: memblock build/run failed (%d)", r);
        return false;
    }
    WHBLogPrint("cpu_xlate: memblock scheme=a ok");

    uint32_t got_w = wii_read_u32(0xC0001000u);
    uint64_t got_d = wii_read_u64(0xC0001010u);
    if (!ctx_equal(&want, &got) || got_w != want_w || got_d != want_d) {
        WHBLogPrintf("cpu_xlate: memblock r4 got=0x%08X want=0x%08X mem@0xC0001000 got=0x%08X want=0x%08X MISMATCH",
                     got.gpr[4], want.gpr[4], got_w, want_w);
        return false;
    }

    static const uint32_t update_block[] = {
        0x3C60C000, 0x60631000, 0x7C0018AC, 0x94630004, 0x84A30004,
    };
    const uint32_t update_pc = 0x80005100u;
    wii_write_u32(0xC0001004u, 0);
    wii_write_u32(0xC0001008u, 0x11223344u);
    PpcContext update_want; memset(&update_want, 0, sizeof update_want);
    update_want.pc = update_pc;
    for (size_t i = 0; i < sizeof update_block / sizeof update_block[0]; ++i)
        wii_write_u32(update_pc + (uint32_t)i * 4, update_block[i]);
    if (ppc_interp_run(&update_want, update_pc + sizeof update_block, 32, &executed) !=
        PPC_INTERP_STOP) {
        WHBLogPrint("cpu_xlate: update form oracle did not reach block end");
        return false;
    }
    uint32_t update_store = wii_read_u32(0xC0001004u);
    wii_write_u32(0xC0001004u, 0);

    PpcContext update_got; memset(&update_got, 0, sizeof update_got);
    r = ppc_xlate_run_block(&update_got, update_pc, update_block,
                            sizeof update_block / sizeof update_block[0]);
    if (r != PPC_XLATE_OK || !ctx_equal(&update_want, &update_got) ||
        wii_read_u32(0xC0001004u) != update_store) {
        WHBLogPrintf("cpu_xlate: update form translation failed r=%d r3=0x%08X r5=0x%08X",
                     r, update_got.gpr[3], update_got.gpr[5]);
        return false;
    }
    return true;
}

bool ppc_xlate_branch_selftest(void) {
    // A counted bdnz loop (acc = 1+..+5), a bl/blr call that doubles it, and a
    // computed bctr into a final block.
    static const uint32_t prog[] = {
        0x38800000, 0x38a00005, 0x7ca903a6, 0x38c00000, 0x38c60001, 0x7c843214,
        0x4200fff8, 0x48000011, 0x38ea0034, 0x7ce903a6, 0x4e800420, 0x7c842214,
        0x4e800020, 0x7c642214, 0x48000004, 0x60000000,
    };
    const uint32_t base = 0x80006000u;
    const uint32_t stop = base + 0x3c;   // `theend`
    for (size_t i = 0; i < sizeof prog / sizeof prog[0]; ++i)
        wii_write_u32(base + (uint32_t)i * 4, prog[i]);

    PpcContext want; memset(&want, 0, sizeof want);
    want.pc = base; want.gpr[10] = base;
    uint32_t ex = 0;
    if (ppc_interp_run(&want, stop, 256, &ex) != PPC_INTERP_STOP) {
        WHBLogPrint("cpu_xlate: branch oracle did not reach stop");
        return false;
    }

    PpcContext got; memset(&got, 0, sizeof got);
    got.pc = base; got.gpr[10] = base;
    PpcXlateSession s;
    PpcXlateResult r = ppc_xlate_run(&got, base, stop, 256, &s);
    if (r != PPC_XLATE_OK || s.stop != PPC_XSTOP_STOP_PC) {
        WHBLogPrintf("cpu_xlate: branch run failed (r=%d stop=%d)", r, (int)s.stop);
        return false;
    }
    WHBLogPrintf("cpu_xlate: block-cache hits=%u misses=%u", s.cache_hits, s.cache_misses);
    if (s.cache_hits == 0) {
        WHBLogPrint("cpu_xlate: branch loop never re-hit a cached block");
        return false;
    }
    if (!ctx_equal(&want, &got)) {
        WHBLogPrintf("cpu_xlate: branches r3 got=%u want=%u r4 got=%u want=%u ctr got=%u want=%u MISMATCH",
                     got.gpr[3], want.gpr[3], got.gpr[4], want.gpr[4], got.ctr, want.ctr);
        return false;
    }
    return true;
}

bool ppc_xlate_interrupt_selftest(void) {
    wii_mmio_reset();
    wii_write_u32(PPC_EXTERNAL_VECTOR, 0x4E800020u);
    uint32_t di0 = (1u << 28) | (240u << 16) | 430u;
    wii_mmio_write(WII_VI_BASE + WII_VI_DISPLAY_INT_0, di0, 4);
    wii_mmio_write(WII_MMIO_PI_INTMR, WII_MMIO_PI_VI, 4);
    wii_vi_tick_vblank();

    PpcContext c;
    memset(&c, 0, sizeof c);
    c.msr = PPC_MSR_EE | 0x00002000u;
    uint32_t pc = 0x80001234u;
    bool took = take_external_interrupt(&c, &pc);
    bool ok = took && pc == PPC_EXTERNAL_VECTOR && c.pc == PPC_EXTERNAL_VECTOR &&
              c.srr0 == 0x80001234u && c.srr1 == (PPC_MSR_EE | 0x00002000u) &&
              !(c.msr & PPC_MSR_EE);
    if (!ok)
        WHBLogPrintf("cpu_xlate: external interrupt delivery failed took=%d pc=%08X srr0=%08X",
                     took, pc, c.srr0);

    memset(&c, 0, sizeof c);
    c.msr = PPC_MSR_EE | 0x00002000u;
    c.decrementer = -1;
    c.decrementer_exception_pending = true;
    pc = 0x80005678u;
    took = take_decrementer_interrupt(&c, &pc);
    ok &= took && pc == PPC_DECREMENTER_VECTOR && c.pc == PPC_DECREMENTER_VECTOR &&
          c.srr0 == 0x80005678u && c.srr1 == (PPC_MSR_EE | 0x00002000u) &&
          !(c.msr & PPC_MSR_EE) && !ppc_decrementer_pending(&c);
    if (!ok)
        WHBLogPrintf("cpu_xlate: decrementer interrupt delivery failed took=%d pc=%08X srr0=%08X",
                     took, pc, c.srr0);

    wii_write_u32(PPC_EXTERNAL_VECTOR, 0);
    wii_mmio_reset();
    return ok;
}

// MMIO/lowmem trap routing
static int      s_bp_calls;
static uint8_t  s_bp_cmd;
static void mmio_on_bp(void *u, uint8_t cmd, uint32_t val) {
    (void)u; (void)val; s_bp_calls++; s_bp_cmd = cmd;
}

bool ppc_xlate_mmio_selftest(void) {
    // Translated block bursts a GX command stream (NOP, BP-load, NOP)
    // into the write-gather pipe
    wii_mmio_reset();
    static const uint32_t wgp[] = {
        0x3c60cc00, 0x60638000, 0x38800000, 0x38a00061, 0x38c00041,
        0x98830000, 0x98a30000, 0x98c30000, 0x98830000, 0x98830000, 0x98830000, 0x98830000,
        0x4e800020,
    };
    const uint32_t base = 0x80007000u;
    const uint32_t stop = base + (uint32_t)sizeof wgp;   // blr returns to the sentinel
    for (size_t i = 0; i < sizeof wgp / sizeof wgp[0]; ++i)
        wii_write_u32(base + (uint32_t)i * 4, wgp[i]);

    PpcContext c; memset(&c, 0, sizeof c);
    c.pc = base; c.lr = stop;
    PpcXlateSession s;
    PpcXlateResult r = ppc_xlate_run(&c, base, stop, 64, &s);
    if (r != PPC_XLATE_OK) {
        WHBLogPrintf("cpu_xlate: mmio wgp run failed (%d)", r);
        return false;
    }

    uint32_t wlen = 0;
    const uint8_t *wdata = wii_mmio_wgp_data(&wlen);
    if (wlen != 7) {
        WHBLogPrintf("cpu_xlate: mmio wgp captured %u bytes (want 7)", wlen);
        return false;
    }
    s_bp_calls = 0; s_bp_cmd = 0;
    GXFifoState st; gx_fifo_state_reset(&st);
    GXFifoSink sink; memset(&sink, 0, sizeof sink);
    sink.on_bp = mmio_on_bp;
    gx_fifo_run(&st, wdata, wlen, &sink, NULL);
    if (s_bp_calls != 1 || s_bp_cmd != 0x41) {
        WHBLogPrintf("cpu_xlate: mmio gx_fifo bp_calls=%d cmd=0x%02X (want 1 / 0x41)",
                     s_bp_calls, s_bp_cmd);
        return false;
    }
    WHBLogPrint("cpu_xlate: mmio scheme=wgp+ipc ok");

    ios_ipc_init();
    const uint32_t blk  = 0x90003000u;   // command block in MEM2
    const uint32_t path = 0x90003100u;
    wii_mem_write(path, "/dev/es", 8);
    wii_write_u32(blk + 0x00, 1);            // IOS_CMD_OPEN
    wii_write_u32(blk + 0x04, 0xFFFFFFFFu);  // poison result; dispatch overwrites it
    wii_write_u32(blk + 0x0C, path);         // arg0: path pointer
    wii_write_u32(blk + 0x10, 0);            // arg1: mode

    static const uint32_t ipc[] = {
        0x3c60cd00, 0x3c809000, 0x60843000, 0x38a00001, 0x90830000, 0x90a30004, 0x4e800020,
    };
    const uint32_t ibase = 0x80007100u;
    const uint32_t istop = ibase + (uint32_t)sizeof ipc;
    for (size_t i = 0; i < sizeof ipc / sizeof ipc[0]; ++i)
        wii_write_u32(ibase + (uint32_t)i * 4, ipc[i]);

    PpcContext ic; memset(&ic, 0, sizeof ic);
    ic.pc = ibase; ic.lr = istop;
    r = ppc_xlate_run(&ic, ibase, istop, 64, &s);
    if (r != PPC_XLATE_OK) {
        WHBLogPrintf("cpu_xlate: mmio ipc run failed (%d)", r);
        return false;
    }
    int32_t result = (int32_t)wii_read_u32(blk + 0x04);
    if (result < 0) {
        WHBLogPrintf("cpu_xlate: mmio ipc dispatch result=%d (want fd>=0)", result);
        return false;
    }

    static const uint32_t dynamic_mmio[] = {
        0x38600000, 0x1CA30014, 0x3D45CD00, 0x394A680C, 0x812A0000, 0x4E800020,
    };
    const uint32_t dbase = 0x80007200u;
    const uint32_t dstop = dbase + (uint32_t)sizeof dynamic_mmio;
    for (size_t i = 0; i < sizeof dynamic_mmio / sizeof dynamic_mmio[0]; ++i)
        wii_write_u32(dbase + (uint32_t)i * 4, dynamic_mmio[i]);
    uint32_t *padding = (uint32_t *)((uint8_t *)wii_mem_fastmem_window() +
                                     (0xCD00680Cu & WII_FASTMEM_MASK));
    *padding = 1;
    PpcContext dc; memset(&dc, 0, sizeof dc);
    dc.pc = dbase; dc.lr = dstop;
    r = ppc_xlate_run(&dc, dbase, dstop, 64, &s);
    *padding = 0;
    if (r != PPC_XLATE_OK || s.stop != PPC_XSTOP_STOP_PC || dc.gpr[9] != 0) {
        WHBLogPrintf("cpu_xlate: dynamic mmio routing failed r=%d stop=%d r9=0x%08X",
                     r, (int)s.stop, dc.gpr[9]);
        return false;
    }

    wii_mmio_reset();
    wii_mmio_write(WII_DSP_ARAM_DMA_CNT_LO, 0x20, 2);
    static const uint32_t signed_mmio[] = {
        0x3C60CC00, 0x60635004, 0xA9230000, 0x4E800020,
    };
    const uint32_t sbase = 0x80007300u;
    const uint32_t sstop = sbase + (uint32_t)sizeof signed_mmio;
    for (size_t i = 0; i < sizeof signed_mmio / sizeof signed_mmio[0]; ++i)
        wii_write_u32(sbase + (uint32_t)i * 4, signed_mmio[i]);
    PpcContext sc; memset(&sc, 0, sizeof sc);
    sc.pc = sbase; sc.lr = sstop;
    r = ppc_xlate_run(&sc, sbase, sstop, 64, &s);
    if (r != PPC_XLATE_OK || s.stop != PPC_XSTOP_STOP_PC || sc.gpr[9] != 0xFFFF8000u) {
        WHBLogPrintf("cpu_xlate: signed mmio load failed r=%d stop=%d r9=0x%08X",
                     r, (int)s.stop, sc.gpr[9]);
        return false;
    }
    return true;
}

//Self test
bool ppc_xlate_entry_selftest(void) {
    wii_mmio_reset();
    // A counted loop (r3 -> 10) stored to RAM, then a first write-gather-pipe write.
    static const uint32_t prog[] = {
        0x38600000, 0x3880000a, 0x7c8903a6, 0x38630001, 0x4200fffc,
        0x3ca09000, 0x60a55000, 0x90650000, 0x3cc0cc00, 0x60c68000, 0x38e00000, 0x98e60000,
        0x4e800020,
    };
    const uint32_t base = 0x80004000u;   // a DOL-style entry EA
    for (size_t i = 0; i < sizeof prog / sizeof prog[0]; ++i)
        wii_write_u32(base + (uint32_t)i * 4, prog[i]);
    wii_write_u32(0x90005000u, 0);

    PpcContext c; memset(&c, 0, sizeof c);
    c.pc = base; c.lr = base + (uint32_t)sizeof prog;
    PpcXlateSession s;
    PpcXlateResult r = ppc_xlate_run(&c, base, PPC_XLATE_RUN_TO_GX, 256, &s);
    if (r != PPC_XLATE_OK) {
        WHBLogPrintf("cpu_xlate: entry run failed (%d)", r);
        return false;
    }
    if (s.stop != PPC_XSTOP_GX_WRITE) {
        if (s.stop == PPC_XSTOP_FAULT)
            WHBLogPrintf("cpu_xlate: entry faulted @0x%08X class=%u", s.last_pc, s.last_class);
        else
            WHBLogPrintf("cpu_xlate: entry stopped=%d without a GX write", (int)s.stop);
        return false;
    }
    WHBLogPrintf("cpu_xlate: entry=0x%08X ran %u blocks, first GX write @0x%08X",
                 base, s.blocks_run, s.first_gx_write_ea);
    if (c.gpr[3] != 10 || wii_read_u32(0x90005000u) != 10) {
        WHBLogPrintf("cpu_xlate: entry r3=%u mem=%u (want 10) MISMATCH",
                     c.gpr[3], wii_read_u32(0x90005000u));
        return false;
    }
    return true;
}
