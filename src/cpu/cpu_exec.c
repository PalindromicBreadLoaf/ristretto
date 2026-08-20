// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "cpu/cpu_exec.h"

#include <coreinit/cache.h>
#include <coreinit/codegen.h>
#include <coreinit/context.h>
#include <coreinit/core.h>
#include <coreinit/exception.h>
#include <coreinit/thread.h>
#include <whb/log.h>

#include <malloc.h>
#include <string.h>

//   int int_fn(int a, int b) { return a * b + 7; }
static const uint32_t kIntFn[] = {
    0x7C6321D6,  // mullw r3, r3, r4
    0x38630007,  // addi  r3, r3, 7
    0x4E800020,  // blr
};
//   float flt_fn(float a, float b) { return a * b; }
static const uint32_t kFltFn[] = {
    0xEC2100B2,  // fmuls f1, f1, f2
    0x4E800020,  // blr
};

#define INT_FN_OFFSET 0u
#define FLT_FN_OFFSET 32u   // keep the two routines on separate cache safe slots

typedef int   (*IntFn)(int, int);
typedef float (*FltFn)(float, float);

static void          *s_area;
static CpuExecStatus  s_status;

void cpu_exec_probe(CpuCodegenInfo *out) {
    void *addr = NULL;
    uint32_t size = 0;
    OSGetCodegenVirtAddrRange(&addr, &size);

    out->area      = addr;
    out->size      = size;
    out->available = (addr != NULL && size > 0);
    out->core      = OSGetCodegenCore();
    out->mode      = OSGetCodegenMode();

    WHBLogPrintf("cpu_exec: codegen area=%p size=%u core=%u mode=%u %s",
                 addr, size, out->core, out->mode,
                 out->available ? "(available)" : "(UNAVAILABLE)");
}

static int codegenThreadEntry(int argc, const char **argv) {
    (void)argc;
    (void)argv;

    uint8_t *intAddr = (uint8_t *)s_area + INT_FN_OFFSET;
    uint8_t *fltAddr = (uint8_t *)s_area + FLT_FN_OFFSET;

    // Grant ourselves codegen access
    if (!OSSwitchSecCodeGenMode(CODEGEN_RW_)) {
        WHBLogPrint("cpu_exec: switch to CODEGEN_RW_ failed");
        s_status = CPU_EXEC_FAIL;
        return 0;
    }
    memcpy(intAddr, (const void *)kIntFn, sizeof kIntFn);
    memcpy(fltAddr, (const void *)kFltFn, sizeof kFltFn);
    DCFlushRange(intAddr, sizeof kIntFn);
    DCFlushRange(fltAddr, sizeof kFltFn);

    if (!OSSwitchSecCodeGenMode(CODEGEN_R_X)) {
        WHBLogPrint("cpu_exec: switch to CODEGEN_R_X failed");
        s_status = CPU_EXEC_FAIL;
        return 0;
    }
    ICInvalidateRange(intAddr, sizeof kIntFn);
    ICInvalidateRange(fltAddr, sizeof kFltFn);

    // isync before the first call so the core drops any instructions it prefetched.
    __asm__ volatile ("isync");

    IntFn ifn = (IntFn)(uintptr_t)intAddr;
    FltFn ffn = (FltFn)(uintptr_t)fltAddr;

    int   ir  = ifn(6, 7);
    float fr  = ffn(2.5f, 4.0f);
    int   frx = (int)(fr * 1000.0f);

    bool ok = (ir == 49) && (frx == 10000);
    WHBLogPrintf("cpu_exec: core=%u int_fn(6,7)=%d(want 49) flt_fn(2.5,4)=%d/1000(want 10000) %s",
                 OSGetCoreId(), ir, frx, ok ? "OK" : "WRONG");

    s_status = ok ? CPU_EXEC_PASS : CPU_EXEC_FAIL;
    return 0;
}

CpuExecStatus cpu_exec_selftest(void) {
    CpuCodegenInfo info;
    cpu_exec_probe(&info);
    if (!info.available) {
        return CPU_EXEC_UNAVAILABLE;
    }
    if (info.size < FLT_FN_OFFSET + sizeof kFltFn) {
        WHBLogPrint("cpu_exec: codegen area too small for the test routines");
        return CPU_EXEC_FAIL;
    }

    s_area   = info.area;
    s_status = CPU_EXEC_FAIL;

    // Codegen is usable only from OSGetCodegenCore()
    static OSThread thread;
    const uint32_t stackSize = 64u * 1024u;
    uint8_t *stack = memalign(16, stackSize);
    if (!stack) {
        WHBLogPrint("cpu_exec: failed to allocate codegen thread stack");
        return CPU_EXEC_FAIL;
    }

    OSThreadAttributes affinity = (OSThreadAttributes)(1u << info.core);
    if (!OSCreateThread(&thread, codegenThreadEntry, 0, NULL,
                        stack + stackSize, stackSize, 16, affinity)) {
        WHBLogPrint("cpu_exec: OSCreateThread failed");
        free(stack);
        return CPU_EXEC_FAIL;
    }

    OSResumeThread(&thread);
    int ret = 0;
    OSJoinThread(&thread, &ret);
    free(stack);

    return s_status;
}

// One paired-single op as a raw opcode word for testing.
__asm__(
    ".globl ps_probe_asm\n"
    ".type ps_probe_asm, @function\n"
    "ps_probe_asm:\n"
    "  .long 0x10210072\n"   // ps_mul f1, f1, f1
    "  blr\n"
    ".size ps_probe_asm, .-ps_probe_asm\n"
);
extern float ps_probe_asm(float a);

static volatile bool s_ps_illegal;

// Program exception fires when ps_mul decodes as illegal.
static BOOL psProgramHandler(OSContext *ctx) {
    s_ps_illegal = true;
    ctx->srr0 += 4;
    return TRUE;
}

static CpuPsStatus  s_ps_status;
static float        s_ps_result;

static int psThreadEntry(int argc, const char **argv) {
    (void)argc;
    (void)argv;

    s_ps_illegal = false;
    OSExceptionCallbackFn prev =
        OSSetExceptionCallback(OS_EXCEPTION_TYPE_PROGRAM, psProgramHandler);

    s_ps_result = ps_probe_asm(3.0f);

    OSSetExceptionCallback(OS_EXCEPTION_TYPE_PROGRAM, prev);
    s_ps_status = s_ps_illegal ? CPU_PS_ILLEGAL : CPU_PS_ENABLED;
    return 0;
}

CpuPsStatus cpu_ps_probe_core(uint32_t core) {
    s_ps_status = CPU_PS_ERROR;

    static OSThread thread;
    const uint32_t stackSize = 64u * 1024u;
    uint8_t *stack = memalign(16, stackSize);
    if (!stack) {
        WHBLogPrint("cpu_exec: failed to allocate ps-probe thread stack");
        return CPU_PS_ERROR;
    }

    OSThreadAttributes affinity = (OSThreadAttributes)(1u << core);
    if (!OSCreateThread(&thread, psThreadEntry, 0, NULL,
                        stack + stackSize, stackSize, 16, affinity)) {
        WHBLogPrint("cpu_exec: OSCreateThread failed for ps probe");
        free(stack);
        return CPU_PS_ERROR;
    }

    OSResumeThread(&thread);
    int ret = 0;
    OSJoinThread(&thread, &ret);
    free(stack);

    return s_ps_status;
}

void cpu_ps_probe_all(void) {
    int enabled = 0;
    for (uint32_t core = 0; core < 3; ++core) {
        CpuPsStatus st = cpu_ps_probe_core(core);
        const char *label = st == CPU_PS_ENABLED ? "enabled"
                          : st == CPU_PS_ILLEGAL ? "illegal"
                                                 : "probe error";
        if (st == CPU_PS_ENABLED) {
            ++enabled;
            WHBLogPrintf("cpu_exec: ps core=%u %s ps_mul(3,3)=%d/1000(want 9000)",
                         core, label, (int)(s_ps_result * 1000.0f));
        } else {
            WHBLogPrintf("cpu_exec: ps core=%u %s", core, label);
        }
    }
    WHBLogPrintf("cpu_exec: paired-singles %s (%d/3 cores)",
                 enabled == 3 ? "AVAILABLE" : enabled > 0 ? "PARTIAL" : "UNAVAILABLE",
                 enabled);
}

// A read-only supervisor SPR read (mfspr DBAT0U, SPR 536) as a raw opcode word.
__asm__(
    ".globl priv_probe_asm\n"
    ".type priv_probe_asm, @function\n"
    "priv_probe_asm:\n"
    "  .long 0x7C7882A6\n"   // mfspr r3, DBAT0U (SPR 536)
    "  blr\n"
    ".size priv_probe_asm, .-priv_probe_asm\n"
);
extern uint32_t priv_probe_asm(void);

static volatile bool     s_priv_trapped;
static volatile uint32_t s_priv_srr1;

// Program exception fires when the DBAT0U read is denied in user mode.
static BOOL privProgramHandler(OSContext *ctx) {
    s_priv_trapped = true;
    s_priv_srr1    = ctx->srr1;
    ctx->srr0 += 4;
    return TRUE;
}

static CpuPrivStatus s_priv_status;
static uint32_t      s_priv_value;

static int privThreadEntry(int argc, const char **argv) {
    (void)argc;
    (void)argv;

    s_priv_trapped = false;
    OSExceptionCallbackFn prev =
        OSSetExceptionCallback(OS_EXCEPTION_TYPE_PROGRAM, privProgramHandler);

    s_priv_value = priv_probe_asm();

    OSSetExceptionCallback(OS_EXCEPTION_TYPE_PROGRAM, prev);
    s_priv_status = s_priv_trapped ? CPU_PRIV_USER : CPU_PRIV_SUPERVISOR;
    return 0;
}

CpuPrivStatus cpu_privilege_probe_core(uint32_t core) {
    s_priv_status = CPU_PRIV_ERROR;

    static OSThread thread;
    const uint32_t stackSize = 64u * 1024u;
    uint8_t *stack = memalign(16, stackSize);
    if (!stack) {
        WHBLogPrint("cpu_exec: failed to allocate privilege-probe thread stack");
        return CPU_PRIV_ERROR;
    }

    OSThreadAttributes affinity = (OSThreadAttributes)(1u << core);
    if (!OSCreateThread(&thread, privThreadEntry, 0, NULL,
                        stack + stackSize, stackSize, 16, affinity)) {
        WHBLogPrint("cpu_exec: OSCreateThread failed for privilege probe");
        free(stack);
        return CPU_PRIV_ERROR;
    }

    OSResumeThread(&thread);
    int ret = 0;
    OSJoinThread(&thread, &ret);
    free(stack);

    return s_priv_status;
}

void cpu_privilege_probe_all(void) {
    int user = 0, super = 0;
    for (uint32_t core = 0; core < 3; ++core) {
        CpuPrivStatus st = cpu_privilege_probe_core(core);
        if (st == CPU_PRIV_USER) {
            ++user;
            WHBLogPrintf("cpu_exec: privilege core=%u user mode",
                         core, s_priv_srr1);
        } else if (st == CPU_PRIV_SUPERVISOR) {
            ++super;
            WHBLogPrintf("cpu_exec: privilege core=%u DBAT0U=0x%08X",
                         core, s_priv_value);
        } else {
            WHBLogPrintf("cpu_exec: privilege core=%u probe error", core);
        }
    }
    const char *verdict = (user == 3) ? "UNAVAILABLE"
                        : (super > 0)  ? "AVAILABLE"
                                       : "INCONCLUSIVE";
    WHBLogPrintf("cpu_exec: Espresso supervisor access %s (%d/3 user-mode cores)",
                 verdict, user);
}
