// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "mem/wii_memory.h"

#include <malloc.h>
#include <string.h>

#include <whb/log.h>

static uint8_t *sMem1;
static uint8_t *sMem2;

// Resolve a guest EA to its backing bank, reporting the offset within that bank
// and the bytes remaining to the bank's end.
static uint8_t *bank_of(uint32_t ea, uint32_t *offset, uint32_t *remaining) {
    uint32_t off;

    if ((off = ea - WII_MEM1_EA_CACHED) < WII_MEM1_SIZE ||
        (off = ea - WII_MEM1_EA_UNCACHED) < WII_MEM1_SIZE ||
        (off = ea - WII_MEM1_PHYS) < WII_MEM1_SIZE) {
        if (offset) *offset = off;
        if (remaining) *remaining = WII_MEM1_SIZE - off;
        return sMem1;
    }

    if ((off = ea - WII_MEM2_EA_CACHED) < WII_MEM2_SIZE ||
        (off = ea - WII_MEM2_EA_UNCACHED) < WII_MEM2_SIZE ||
        (off = ea - WII_MEM2_PHYS) < WII_MEM2_SIZE) {
        if (offset) *offset = off;
        if (remaining) *remaining = WII_MEM2_SIZE - off;
        return sMem2;
    }

    return NULL;
}

bool wii_mem_init(void) {
    if (sMem1 && sMem2)
        return true;

    sMem1 = memalign(0x10000, WII_MEM1_SIZE);
    sMem2 = memalign(0x10000, WII_MEM2_SIZE);
    if (!sMem1 || !sMem2) {
        WHBLogPrintf("wii_mem: allocation failed (mem1=%p mem2=%p)", sMem1, sMem2);
        wii_mem_shutdown();
        return false;
    }

    memset(sMem1, 0, WII_MEM1_SIZE);
    memset(sMem2, 0, WII_MEM2_SIZE);
    return true;
}

void wii_mem_shutdown(void) {
    free(sMem1);
    free(sMem2);
    sMem1 = NULL;
    sMem2 = NULL;
}

void *wii_mem_ptr(uint32_t ea) {
    uint32_t off;
    uint8_t *bank = bank_of(ea, &off, NULL);
    return bank ? bank + off : NULL;
}

void *wii_mem_range(uint32_t ea, uint32_t size) {
    uint32_t off, remaining;
    uint8_t *bank = bank_of(ea, &off, &remaining);
    if (!bank || size > remaining)
        return NULL;
    return bank + off;
}

uint8_t wii_read_u8(uint32_t ea) {
    const uint8_t *p = wii_mem_range(ea, sizeof(uint8_t));
    return p ? *p : 0;
}

uint16_t wii_read_u16(uint32_t ea) {
    const void *p = wii_mem_range(ea, sizeof(uint16_t));
    uint16_t v = 0;
    if (p) memcpy(&v, p, sizeof(v));
    return v;
}

uint32_t wii_read_u32(uint32_t ea) {
    const void *p = wii_mem_range(ea, sizeof(uint32_t));
    uint32_t v = 0;
    if (p) memcpy(&v, p, sizeof(v));
    return v;
}

uint64_t wii_read_u64(uint32_t ea) {
    const void *p = wii_mem_range(ea, sizeof(uint64_t));
    uint64_t v = 0;
    if (p) memcpy(&v, p, sizeof(v));
    return v;
}

void wii_write_u8(uint32_t ea, uint8_t value) {
    uint8_t *p = wii_mem_range(ea, sizeof(value));
    if (p) *p = value;
}

void wii_write_u16(uint32_t ea, uint16_t value) {
    void *p = wii_mem_range(ea, sizeof(value));
    if (p) memcpy(p, &value, sizeof(value));
}

void wii_write_u32(uint32_t ea, uint32_t value) {
    void *p = wii_mem_range(ea, sizeof(value));
    if (p) memcpy(p, &value, sizeof(value));
}

void wii_write_u64(uint32_t ea, uint64_t value) {
    void *p = wii_mem_range(ea, sizeof(value));
    if (p) memcpy(p, &value, sizeof(value));
}

void wii_mem_write(uint32_t ea, const void *src, uint32_t size) {
    const uint8_t *in = src;
    while (size) {
        uint32_t off, remaining;
        uint8_t *bank = bank_of(ea, &off, &remaining);
        if (!bank)
            return;
        uint32_t n = size < remaining ? size : remaining;
        memcpy(bank + off, in, n);
        ea += n;
        in += n;
        size -= n;
    }
}

void wii_mem_read(void *dst, uint32_t ea, uint32_t size) {
    uint8_t *out = dst;
    while (size) {
        uint32_t off, remaining;
        uint8_t *bank = bank_of(ea, &off, &remaining);
        if (!bank) {
            memset(out, 0, size);
            return;
        }
        uint32_t n = size < remaining ? size : remaining;
        memcpy(out, bank + off, n);
        ea += n;
        out += n;
        size -= n;
    }
}

void wii_mem_setup_lowmem(void) {
    // Globals the retail IPL/BS2 leaves in low MEM1 before handing off to the
    // apploader.
    // Sourced from Dolphin's SetupWiiMemory.
    wii_write_u32(0x00000020, 0x0D15EA5E);      // boot magic
    wii_write_u32(0x00000024, 0x00000001);      // version
    wii_write_u32(0x00000028, WII_MEM1_SIZE);   // physical MEM1 size (24 MiB)
    wii_write_u32(0x0000002C, 0x00000023);      // board model: RVL_Retail3
    wii_write_u32(0x00000030, 0x00000000);      // arena low
    wii_write_u32(0x00000034, 0x817FEC60);      // arena high
    // 0x38/0x3C (FST start/size) filled by the apploader HLE.
    wii_write_u32(0x000000CC, 0x00000000);      // VI region
    wii_write_u32(0x000000E4, 0x8008F7B8);      // __OSThreadInit thunk
    wii_write_u32(0x000000F0, WII_MEM1_SIZE);   // simulated memory size
    wii_write_u32(0x000000F4, 0x8179B500);      // __start
    wii_write_u32(0x000000F8, 0x0E7BE2C0);      // bus clock speed
    wii_write_u32(0x000000FC, 0x2B73A840);      // CPU clock speed

    wii_write_u32(0x000030C0, 0x00000000);      // EXI
    wii_write_u32(0x000030C4, 0x00000000);      // EXI
    wii_write_u32(0x000030DC, 0x00000000);      // time
    wii_write_u32(0x000030D8, 0xFFFFFFFF);      // set by every official NAND title
    wii_write_u16(0x000030E0, 0x0000);          // PADInit
    wii_write_u16(0x000030E6, 0x8201);          // dev/debug-capable console flag
    wii_write_u32(0x000030F0, 0x00000000);      // apploader

    wii_write_u16(0x0000315E, 0x0113);          // devkit boot program version (v1.13)
    wii_write_u8(0x0000315C, 0x80);             // OSInit state
    wii_write_u32(0x00003184, 0x80000000);      // GameID address

    for (uint32_t ea = 0x00003000; ea <= 0x00003038; ea += 4)
        wii_write_u32(ea, 0x00000000);          // clear exception-handler slots
}

void wii_mem_log_layout(void) {
    WHBLogPrintf("wii_mem: MEM1 host=%p size=%u KiB  EA 0x%08X(cached)/0x%08X(uncached)",
                 sMem1, WII_MEM1_SIZE >> 10, WII_MEM1_EA_CACHED, WII_MEM1_EA_UNCACHED);
    WHBLogPrintf("wii_mem: MEM2 host=%p size=%u KiB  EA 0x%08X(cached)/0x%08X(uncached)",
                 sMem2, WII_MEM2_SIZE >> 10, WII_MEM2_EA_CACHED, WII_MEM2_EA_UNCACHED);
    WHBLogPrintf("wii_mem: lowmem magic@0x80000020=0x%08X ramsize@0x80000028=0x%08X",
                 wii_read_u32(0x80000020), wii_read_u32(0x80000028));
}
