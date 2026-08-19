// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "boot/boot.h"

#include <whb/log.h>

#include "mem/wii_memory.h"

// __OSBootInfo lives at the base of MEM1.
// The first 0x20 bytes are the DVDDiskID
// arenaLo/Hi at 0x30/0x34 bound the free heap the runtime hands out.
#define OSBOOTINFO_DISK_ID   0x00000000u
#define OSBOOTINFO_ARENA_LO  0x00000030u
#define OSBOOTINFO_ARENA_HI  0x00000034u

static uint32_t align_up(uint32_t v, uint32_t a) {
    return (v + (a - 1)) & ~(a - 1);
}

void boot_apply_params(const DolLoadResult *dol, const char *disc_id6) {
    // DVDDiskID is a 6-byte game code + 2-byte maker
    for (uint32_t i = 0; i < 6; ++i)
        wii_write_u8(OSBOOTINFO_DISK_ID + i, disc_id6 ? (uint8_t)disc_id6[i] : 0);

    const uint32_t arena_lo = align_up(dol->image_hi, 0x20);
    wii_write_u32(OSBOOTINFO_ARENA_LO, arena_lo);

    // FST start/size (0x38/0x3C) stay zero until the disc/apploader path fills
    // them.
    // TODO: populate once the DI device can read the disc FST.

    WHBLogPrintf("boot: entry=0x%08X arenaLo=0x%08X arenaHi=0x%08X diskID=%.6s",
                 dol->entry_point, wii_read_u32(0x80000030), wii_read_u32(0x80000034),
                 disc_id6 ? disc_id6 : "<none>");
}

bool boot_dol_from_buffer(const void *buf, uint32_t size, const char *disc_id6,
                          DolLoadResult *out) {
    if (!dol_load(buf, size, out))
        return false;
    boot_apply_params(out, disc_id6);
    return true;
}
