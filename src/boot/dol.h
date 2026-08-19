// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef RISTRETTO_BOOT_DOL_H
#define RISTRETTO_BOOT_DOL_H

#include <stdbool.h>
#include <stdint.h>

// Loader for the Nintendo DOL executable format.

#define DOL_NUM_TEXT 7
#define DOL_NUM_DATA 11
#define DOL_HEADER_SIZE 0xE4u

typedef struct {
    uint32_t entry_point;   // guest EA the game starts executing at
    uint32_t bss_address;   // guest EA of the zero-init region
    uint32_t bss_size;
    uint32_t image_lo;      // lowest guest EA touched by the load
    uint32_t image_hi;      // one past the highest guest EA touched (incl. BSS)
    uint32_t section_count; // non-empty text+data sections loaded
    bool     is_wii;        // true if a text section pokes HID4
} DolLoadResult;

// Parse and load a DOL image into the guest memory map.
bool dol_load(const void *buf, uint32_t size, DolLoadResult *out);

#endif  // RISTRETTO_BOOT_DOL_H
