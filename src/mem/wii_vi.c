// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "mem/wii_vi.h"

#include "mem/wii_mmio.h"

#include <string.h>

static uint8_t s_regs[WII_VI_SIZE];

static bool     s_field_top;     // parity of the field about to scan out
static uint64_t s_field_count;

static uint16_t rd16(uint32_t off) {
    return (uint16_t)((s_regs[off] << 8) | s_regs[off + 1]);
}

static void wr16(uint32_t off, uint16_t v) {
    s_regs[off]     = (uint8_t)(v >> 8);
    s_regs[off + 1] = (uint8_t)v;
}

static uint32_t rd32(uint32_t off) {
    return ((uint32_t)rd16(off) << 16) | rd16(off + 2);
}

static void wr32(uint32_t off, uint32_t v) {
    wr16(off, (uint16_t)(v >> 16));
    wr16(off + 2, (uint16_t)v);
}

// FB-info register
static uint32_t fb_xfb_addr(uint32_t off) {
    uint32_t v   = rd32(off);
    uint32_t fbb = v & 0x00FFFFFFu;
    bool     poff = (rd32(WII_VI_FB_LEFT_TOP_HI) >> 28) & 1u;
    return poff ? (fbb << 5) : fbb;
}

static void fb_apply_clrpoff(uint32_t off) {
    uint32_t v = rd32(off);
    if (off == WII_VI_FB_LEFT_TOP_HI && (v & 0x80000000u))
        wr32(WII_VI_FB_LEFT_TOP_HI, v & ~(1u << 28));
}

// Display interrupt register
static uint32_t di_off(uint32_t i)     { return WII_VI_DISPLAY_INT_0 + i * 4u; }
static uint32_t di_vct(uint32_t v)     { return (v >> 16) & 0x7FFu; }
static bool     di_mask(uint32_t v)    { return (v >> 28) & 1u; }
static bool     di_int(uint32_t v)     { return (v >> 31) & 1u; }

void wii_vi_reset(void) {
    memset(s_regs, 0, sizeof(s_regs));
    s_field_top   = true;
    s_field_count = 0;
    wr16(WII_VI_VERTICAL_TIMING, 6u);
    wr16(WII_VI_CONTROL, 1u);
    wr32(WII_VI_DISPLAY_INT_0, (1u << 28) | (263u << 16) | 430u);
}

void wii_vi_write(uint32_t offset, uint32_t value, uint32_t size) {
    if (offset >= WII_VI_SIZE)
        return;
    switch (size) {
    case 1:
        s_regs[offset] = (uint8_t)value;
        break;
    case 2:
        wr16(offset, (uint16_t)value);
        break;
    case 4:
    default:
        wr32(offset, value);
        break;
    }

    if (offset == WII_VI_FB_LEFT_TOP_HI)
        fb_apply_clrpoff(WII_VI_FB_LEFT_TOP_HI);
}

uint32_t wii_vi_read(uint32_t offset, uint32_t size) {
    if (offset >= WII_VI_SIZE)
        return 0;
    switch (size) {
    case 1:  return s_regs[offset];
    case 2:  return rd16(offset);
    case 4:
    default: return rd32(offset);
    }
}

uint32_t wii_vi_xfb_top(void)    { return fb_xfb_addr(WII_VI_FB_LEFT_TOP_HI); }
uint32_t wii_vi_xfb_bottom(void) { return fb_xfb_addr(WII_VI_FB_LEFT_BOTTOM_HI); }

uint32_t wii_vi_current_xfb(void) {
    if (!s_field_top) {
        uint32_t bottom = wii_vi_xfb_bottom();
        if (bottom)
            return bottom;
    }
    return wii_vi_xfb_top();
}

// Active lines per field.
// PAL scans 313 lines/field. NTSC is 263.
static uint32_t lines_per_field(void) {
    uint32_t acv = (rd16(WII_VI_VERTICAL_TIMING) >> 4) & 0x3FFu;
    if (acv)
        return acv;
    uint32_t fmt = (rd16(WII_VI_CONTROL) >> 8) & 0x3u;
    return (fmt == 1u) ? 313u : 263u;  // 1 == PAL
}

bool wii_vi_tick_vblank(void) {
    uint32_t lines = lines_per_field();

    // Arm every display interrupt whose target line lands within this field.
    for (uint32_t i = 0; i < WII_VI_NUM_DISPLAY_INT; ++i) {
        uint32_t off = di_off(i);
        uint32_t v   = rd32(off);
        uint32_t vct = di_vct(v);
        if (di_mask(v) && vct <= lines)
            wr32(off, v | (1u << 31));  // set IR_INT
    }

    wr16(WII_VI_VERTICAL_BEAM_POS, (uint16_t)lines);

    s_field_top = !s_field_top;
    ++s_field_count;

    return wii_vi_irq_pending();
}

bool wii_vi_irq_pending(void) {
    for (uint32_t i = 0; i < WII_VI_NUM_DISPLAY_INT; ++i) {
        uint32_t v = rd32(di_off(i));
        if (di_int(v) && di_mask(v))
            return true;
    }
    return false;
}

uint64_t wii_vi_field_count(void) { return s_field_count; }
bool     wii_vi_field_is_top(void) { return s_field_top; }

bool wii_vi_selftest(void) {
    bool ok = true;

    // Drive through the MMIO router the way translated guest stores will.
    wii_mmio_reset();

    // NTSC 640x480 XFB at physical 0x00300000
    uint32_t fb_top = (0x00300000u >> 5) | (1u << 28);
    wii_mmio_write(WII_VI_BASE + WII_VI_FB_LEFT_TOP_HI, fb_top, 4);
    if (wii_vi_xfb_top() != 0x00300000u) {
        ok = false;  // POFF address derivation wrong
    }
    if (wii_vi_current_xfb() != 0x00300000u) {
        ok = false;  // top field should scan the top XFB
    }

    wii_vi_write(WII_VI_FB_LEFT_BOTTOM_HI, 0x00301E00u >> 5, 4);
    if (wii_vi_xfb_bottom() != 0x00301E00u) {
        ok = false;
    }

    // Arm DI0 at a retrace line with its mask set.
    uint32_t di0 = (0u << 31) | (1u << 28) | (240u << 16) | 430u;  // VCT=240
    wii_mmio_write(WII_VI_BASE + WII_VI_DISPLAY_INT_0, di0, 4);
    if (wii_vi_irq_pending()) {
        ok = false;  // nothing should be pending before a field elapses
    }

    // A field retrace should raise DI0 (VCT 240 < 263 lines) but leave DI2 clear.
    bool pending = wii_vi_tick_vblank();
    if (!pending || !wii_vi_irq_pending()) {
        ok = false;
    }
    uint32_t di0_after = wii_vi_read(WII_VI_DISPLAY_INT_0, 4);
    if (!((di0_after >> 31) & 1u)) {
        ok = false;  // IR_INT not latched
    }
    if ((wii_vi_read(WII_VI_DISPLAY_INT_2, 4) >> 31) & 1u) {
        ok = false;  // disabled interrupt fired
    }
    if (wii_vi_field_count() != 1 || wii_vi_field_is_top()) {
        ok = false;  // one field elapsed
    }

    // The bottom field scans the bottom XFB.
    if (wii_vi_current_xfb() != 0x00301E00u) {
        ok = false;
    }

    // Guest acks by writing IR_INT back to 0.
    wii_mmio_write(WII_VI_BASE + WII_VI_DISPLAY_INT_0, di0_after & ~(1u << 31), 4);
    if (wii_vi_irq_pending()) {
        ok = false;
    }

    wii_mmio_reset();
    return ok;
}
