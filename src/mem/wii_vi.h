// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef RISTRETTO_MEM_WII_VI_H
#define RISTRETTO_MEM_WII_VI_H

#include <stdbool.h>
#include <stdint.h>

// Software model of the Wii Video Interface.

#define WII_VI_BASE 0xCC002000u
#define WII_VI_SIZE 0x100u

// 16-bit register offsets from the VI base
#define WII_VI_VERTICAL_TIMING   0x00u
#define WII_VI_CONTROL           0x02u
#define WII_VI_FB_LEFT_TOP_HI    0x1Cu   // XFB top field
#define WII_VI_FB_LEFT_BOTTOM_HI 0x24u   // XFB bottom field
#define WII_VI_VERTICAL_BEAM_POS 0x2Cu
#define WII_VI_HORIZONTAL_BEAM   0x2Eu
#define WII_VI_DISPLAY_INT_0     0x30u   // four 32-bit display interrupt registers
#define WII_VI_DISPLAY_INT_1     0x34u
#define WII_VI_DISPLAY_INT_2     0x38u
#define WII_VI_DISPLAY_INT_3     0x3Cu

#define WII_VI_NUM_DISPLAY_INT   4u

static inline bool wii_ea_is_vi(uint32_t ea) {
    return ea >= WII_VI_BASE && ea < (WII_VI_BASE + WII_VI_SIZE);
}

void wii_vi_reset(void);

void     wii_vi_write(uint32_t offset, uint32_t value, uint32_t size);
uint32_t wii_vi_read(uint32_t offset, uint32_t size);

// Physical XFB address of the field currently being scanned out.
uint32_t wii_vi_current_xfb(void);
uint32_t wii_vi_xfb_top(void);
uint32_t wii_vi_xfb_bottom(void);

// Advance one field/retrace.
bool wii_vi_tick_vblank(void);

// True while any display interrupt is asserted and unmasked.
bool wii_vi_irq_pending(void);

// Fields completed since reset, and the parity of the field about to scan out.
uint64_t wii_vi_field_count(void);
bool     wii_vi_field_is_top(void);

bool wii_vi_selftest(void);

#endif  // RISTRETTO_MEM_WII_VI_H
