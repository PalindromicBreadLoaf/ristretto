// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu/xfb_present.h"

#include <stddef.h>

// BT.601 YCbCr -> RGB, fixed point in 8.8. Coefficients are the standard
// full-range conversion scaled by 256.
#define K_CR_R 351   // 1.371 * 256
#define K_CB_G 86    // 0.336 * 256
#define K_CR_G 179   // 0.698 * 256
#define K_CB_B 443   // 1.732 * 256

static inline uint8_t clamp8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static inline void yuv_to_rgb(int y, int cb2, int cr2, uint8_t *out) {
    out[0] = clamp8(y + ((K_CR_R * cr2) >> 8));
    out[1] = clamp8(y - ((K_CB_G * cb2) >> 8) - ((K_CR_G * cr2) >> 8));
    out[2] = clamp8(y + ((K_CB_B * cb2) >> 8));
    out[3] = 0xFF;
}

void xfb_yuv422_to_rgba(const uint8_t *yuv, uint32_t width, uint32_t height,
                        uint32_t src_pitch, uint8_t *rgba, uint32_t dst_pitch) {
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t *s = yuv + (size_t)y * src_pitch;
        uint8_t *d = rgba + (size_t)y * dst_pitch;
        for (uint32_t x = 0; x < width; x += 2) {
            int y0 = s[0];
            int cb2 = (int)s[1] - 128;
            int y1 = s[2];
            int cr2 = (int)s[3] - 128;
            s += 4;

            yuv_to_rgb(y0, cb2, cr2, d);
            if (x + 1 < width)
                yuv_to_rgb(y1, cb2, cr2, d + 4);
            d += 8;
        }
    }
}

bool xfb_present_selftest(void) {
    bool ok = true;

    // Neutral chroma is greyscale.
    const uint8_t grey[4] = {200, 128, 200, 128};
    uint8_t out[8];
    xfb_yuv422_to_rgba(grey, 2, 1, 4, out, 8);
    if (out[0] != 200 || out[1] != 200 || out[2] != 200 || out[3] != 0xFF ||
        out[4] != 200 || out[5] != 200 || out[6] != 200 || out[7] != 0xFF) {
        ok = false;
    }

    // Strong blue chroma
    const uint8_t blue[4] = {128, 255, 128, 128};
    xfb_yuv422_to_rgba(blue, 2, 1, 4, out, 8);
    if (out[0] != 128 || out[2] != 255 || out[3] != 0xFF) {
        ok = false;  // R should hold at Y, B should clamp to 255
    }
    if (out[1] >= out[0]) {
        ok = false;  // G is pulled below Y by the +Cb term
    }

    return ok;
}
