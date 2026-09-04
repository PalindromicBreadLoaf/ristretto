// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "boot/boot.h"

#include <stdlib.h>

#include <whb/log.h>

#include "mem/wii_memory.h"

// __OSBootInfo lives at the base of MEM1.
// The first 0x20 bytes are the DVDDiskID
#define OSBOOTINFO_DISK_ID   0x00000000u
#define OSBOOTINFO_ARENA_LO  0x00000030u
#define OSBOOTINFO_ARENA_HI  0x00000034u
#define OSBOOTINFO_FST_ADDR  0x00000038u
#define OSBOOTINFO_FST_SIZE  0x0000003Cu

#define DISC_BOOT_DOL_OFFSET 0x420u
#define DISC_BOOT_FST_OFFSET 0x424u
#define DISC_BOOT_FST_SIZE   0x428u

static uint32_t align_up(uint32_t v, uint32_t a) {
    return (v + (a - 1)) & ~(a - 1);
}

void boot_apply_params(const DolLoadResult *dol, const char *disc_id6) {
    // DVDDiskID is a 6-byte game code + 2-byte maker
    for (uint32_t i = 0; i < 6; ++i)
        wii_write_u8(OSBOOTINFO_DISK_ID + i, disc_id6 ? (uint8_t)disc_id6[i] : 0);

    const uint32_t arena_lo = align_up(dol->image_hi, 0x20);
    wii_write_u32(OSBOOTINFO_ARENA_LO, arena_lo);
    wii_write_u32(OSBOOTINFO_FST_ADDR, 0);
    wii_write_u32(OSBOOTINFO_FST_SIZE, 0);

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

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static bool boot_load_fst(Disc *disc, const uint8_t *boot, const DolLoadResult *dol) {
    const uint64_t fst_offset = (uint64_t)read_be32(boot + DISC_BOOT_FST_OFFSET) << 2;
    const uint64_t fst_size = (uint64_t)read_be32(boot + DISC_BOOT_FST_SIZE) << 2;
    if (fst_size == 0 || fst_size > UINT32_MAX) {
        WHBLogPrintf("boot: invalid disc FST size 0x%llX", (unsigned long long)fst_size);
        return false;
    }
    if (dol->image_hi > UINT32_MAX - 0x1Fu) {
        WHBLogPrint("boot: DOL image leaves no aligned FST address");
        return false;
    }

    const uint32_t fst_ea = align_up(dol->image_hi, 0x20);
    const uint32_t arena_hi = wii_read_u32(OSBOOTINFO_ARENA_HI);
    if (fst_ea > arena_hi || fst_size > (uint64_t)arena_hi - fst_ea ||
        !wii_mem_range(fst_ea, (uint32_t)fst_size)) {
        WHBLogPrintf("boot: FST 0x%llX bytes does not fit at 0x%08X",
                     (unsigned long long)fst_size, fst_ea);
        return false;
    }
    if (!disc_read_partition(disc, fst_offset, wii_mem_ptr(fst_ea), (uint32_t)fst_size)) {
        WHBLogPrintf("boot: could not read FST @0x%llX", (unsigned long long)fst_offset);
        return false;
    }

    const uint32_t arena_lo = align_up(fst_ea + (uint32_t)fst_size, 0x20);
    if (arena_lo < fst_ea || arena_lo > arena_hi) {
        WHBLogPrint("boot: FST alignment exceeds the guest arena");
        return false;
    }
    wii_write_u32(OSBOOTINFO_FST_ADDR, fst_ea);
    wii_write_u32(OSBOOTINFO_FST_SIZE, (uint32_t)fst_size);
    wii_write_u32(OSBOOTINFO_ARENA_LO, arena_lo);
    WHBLogPrintf("boot: FST @0x%08X size=0x%X arenaLo=0x%08X",
                 fst_ea, (uint32_t)fst_size, arena_lo);
    return true;
}

bool boot_dol_from_disc(Disc *disc, DolLoadResult *out) {
    if (!disc || !out || !disc->part_open) return false;

    uint8_t boot[0x440];
    if (!disc_read_partition(disc, 0, boot, sizeof(boot))) return false;
    const uint64_t dol_offset = (uint64_t)read_be32(boot + DISC_BOOT_DOL_OFFSET) << 2;
    if (dol_offset == 0) {
        WHBLogPrint("boot: disc boot header has no main DOL offset");
        return false;
    }

    uint8_t header[DOL_HEADER_SIZE];
    if (!disc_read_partition(disc, dol_offset, header, sizeof(header))) return false;

    uint32_t dol_size = DOL_HEADER_SIZE;
    for (uint32_t i = 0; i < DOL_NUM_TEXT + DOL_NUM_DATA; ++i) {
        const uint32_t section_offset = read_be32(header + i * 4);
        const uint32_t section_size = read_be32(header + 0x90 + i * 4);
        if (section_size > UINT32_MAX - section_offset) {
            WHBLogPrintf("boot: disc DOL section %u overflows its file offset", i);
            return false;
        }
        if (section_offset + section_size > dol_size)
            dol_size = section_offset + section_size;
    }

    uint8_t *dol = malloc(dol_size);
    if (!dol) {
        WHBLogPrintf("boot: cannot allocate %u-byte disc DOL", dol_size);
        return false;
    }
    const bool read_ok = disc_read_partition(disc, dol_offset, dol, dol_size);
    const bool load_ok = read_ok && boot_dol_from_buffer(dol, dol_size, disc->game_id, out) &&
                         boot_load_fst(disc, boot, out);
    free(dol);
    return load_ok;
}
