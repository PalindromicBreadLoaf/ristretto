// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "cpu/ppc_xlate.h"

#include "cpu/ppc_decode.h"
#include "cpu/ppc_interp.h"
#include "mem/wii_memory.h"

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

// Rewrite one D-form load/store so its EA resolves onto the fastmem window
static bool emit_mem(const PpcInst *in) {
    if (in->mem_indexed || in->mem_update)
        return false;   // TODO: indexed and update forms
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
    return ok;
}

static PpcXlateResult emit_block(const uint32_t *guest, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        PpcInst in;
        if (!ppc_decode(guest[i], &in))
            return PPC_XLATE_UNSUPPORTED;

        switch (in.class) {
        case PPC_CLASS_ALU:
            if (gpr_reserved(in.rd) || gpr_reserved(in.ra))
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

static int xlateThreadEntry(int argc, const char **argv) {
    (void)argc; (void)argv;
    uint8_t *dst = s_area;
    uint32_t bytes = s_len * 4;

    if (!OSSwitchSecCodeGenMode(CODEGEN_RW_)) {
        WHBLogPrint("cpu_xlate: CODEGEN_RW_ failed");
        s_run_status = PPC_XLATE_ERROR;
        return 0;
    }
    memcpy(dst, s_code, bytes);
    DCFlushRange(dst, bytes);
    if (!OSSwitchSecCodeGenMode(CODEGEN_R_X)) {
        WHBLogPrint("cpu_xlate: CODEGEN_R_X failed");
        s_run_status = PPC_XLATE_ERROR;
        return 0;
    }
    ICInvalidateRange(dst, bytes);
    __asm__ volatile("isync");

    void (*fn)(void) = (void (*)(void))(uintptr_t)dst;

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
    // li r3,100 | li r4,7 | mullw r5,r3,r4 | addi r5,r5,5 | fmul f3,f1,f2
    static const uint32_t block[] = {
        0x38600064, 0x38800007, 0x7CA321D6, 0x38A50005, 0xFC6100B2,
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
    if (!ctx_equal(&want, &got)) {
        WHBLogPrintf("cpu_xlate: identity r5 got=%u want=%u f3 got=%d/1000 want=%d/1000 MISMATCH",
                     got.gpr[5], want.gpr[5],
                     (int)(got.fpr[3].d * 1000.0), (int)(want.fpr[3].d * 1000.0));
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
    return true;
}
