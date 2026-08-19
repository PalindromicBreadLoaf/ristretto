// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "cpu/cpu_exec.h"

#include <coreinit/cache.h>
#include <coreinit/codegen.h>
#include <coreinit/core.h>
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
