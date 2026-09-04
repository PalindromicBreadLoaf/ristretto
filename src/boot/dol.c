// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "boot/dol.h"

#include <string.h>

#include <whb/log.h>

#include "mem/wii_memory.h"

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

typedef struct {
    uint32_t offset[DOL_NUM_TEXT + DOL_NUM_DATA];   // byte offset within the DOL
    uint32_t address[DOL_NUM_TEXT + DOL_NUM_DATA];  // destination guest EA
    uint32_t size[DOL_NUM_TEXT + DOL_NUM_DATA];
    uint32_t bss_address;
    uint32_t bss_size;
    uint32_t entry_point;
} DolHeader;

#define DOL_NUM_SECTIONS (DOL_NUM_TEXT + DOL_NUM_DATA)

static void parse_header(const uint8_t *buf, DolHeader *h) {
    const uint8_t *p = buf;
    for (int i = 0; i < DOL_NUM_SECTIONS; ++i, p += 4)
        h->offset[i] = read_be32(p);
    for (int i = 0; i < DOL_NUM_SECTIONS; ++i, p += 4)
        h->address[i] = read_be32(p);
    for (int i = 0; i < DOL_NUM_SECTIONS; ++i, p += 4)
        h->size[i] = read_be32(p);
    h->bss_address = read_be32(p); p += 4;
    h->bss_size    = read_be32(p); p += 4;
    h->entry_point = read_be32(p);
}

// Set Wii mode for extra RAM
#define HID4_PATTERN 0x7C13FBA6u
#define HID4_MASK    0xFC1FFFFFu

static bool section_is_wii(const uint8_t *sect, uint32_t size) {
    for (uint32_t off = 0; off + 4 <= size; off += 4) {
        if ((read_be32(sect + off) & HID4_MASK) == HID4_PATTERN)
            return true;
    }
    return false;
}

bool dol_load(const void *buf, uint32_t size, DolLoadResult *out) {
    if (!buf || !out || size < DOL_HEADER_SIZE) {
        WHBLogPrintf("dol: image too small (%u bytes)", size);
        return false;
    }

    const uint8_t *bytes = buf;
    DolHeader h;
    parse_header(bytes, &h);

    DolLoadResult r = {0};
    r.entry_point = h.entry_point;
    r.bss_address = h.bss_size ? h.bss_address : 0;
    r.bss_size    = h.bss_size;
    r.image_lo    = 0xFFFFFFFFu;
    r.image_hi    = 0;

    if (h.bss_size) {
        void *bss = wii_mem_range(h.bss_address, h.bss_size);
        if (!bss) {
            WHBLogPrintf("dol: BSS 0x%08X+0x%X not in guest memory",
                         h.bss_address, h.bss_size);
            return false;
        }
        memset(bss, 0, h.bss_size);
    }

    for (int i = 0; i < DOL_NUM_SECTIONS; ++i) {
        if (h.size[i] == 0)
            continue;

        const bool is_text = i < DOL_NUM_TEXT;

        // GC/Wii sections are always 32-byte aligned.
        if ((h.address[i] & 31) || (h.size[i] & 31)) {
            WHBLogPrintf("dol: section %d misaligned addr=0x%08X size=0x%X",
                         i, h.address[i], h.size[i]);
            return false;
        }
        // Guard against an offset+size that runs past the end of the image.
        if (h.offset[i] > size || h.size[i] > size - h.offset[i]) {
            WHBLogPrintf("dol: section %d runs past image end (off=0x%X size=0x%X buf=0x%X)",
                         i, h.offset[i], h.size[i], size);
            return false;
        }
        // The destination must sit inside a mappable guest bank.
        if (!wii_mem_range(h.address[i], h.size[i])) {
            WHBLogPrintf("dol: section %d dest 0x%08X+0x%X not in guest memory",
                         i, h.address[i], h.size[i]);
            return false;
        }

        const uint8_t *sect = bytes + h.offset[i];
        wii_mem_write(h.address[i], sect, h.size[i]);
        if (is_text && !r.is_wii)
            r.is_wii = section_is_wii(sect, h.size[i]);

        if (h.address[i] < r.image_lo)
            r.image_lo = h.address[i];
        if (h.address[i] + h.size[i] > r.image_hi)
            r.image_hi = h.address[i] + h.size[i];
        r.section_count++;
    }

    if (r.section_count == 0) {
        WHBLogPrint("dol: no sections to load");
        return false;
    }

    if (h.bss_size) {
        if (h.bss_address + h.bss_size > r.image_hi)
            r.image_hi = h.bss_address + h.bss_size;
    }

    *out = r;
    WHBLogPrintf("dol: loaded %u sections, entry=0x%08X image=0x%08X..0x%08X bss=0x%08X+0x%X %s",
                 r.section_count, r.entry_point, r.image_lo, r.image_hi,
                 r.bss_address, r.bss_size, r.is_wii ? "(Wii)" : "(GC)");
    return true;
}
