// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef RISTRETTO_BOOT_BOOT_H
#define RISTRETTO_BOOT_BOOT_H

#include <stdbool.h>
#include <stdint.h>

#include "boot/dol.h"

// Finalises the low-MEM1 boot parameters a game reads at startup.

// disc_id6 is the 6-char game code
void boot_apply_params(const DolLoadResult *dol, const char *disc_id6);

bool boot_dol_from_buffer(const void *buf, uint32_t size, const char *disc_id6,
                          DolLoadResult *out);

#endif  // RISTRETTO_BOOT_BOOT_H
