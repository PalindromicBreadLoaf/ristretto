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

static inline uint8_t rgb_to_y(uint8_t r, uint8_t g, uint8_t b) {
    return (uint8_t)((66u * r + 129u * g + 25u * b + 4224u) >> 8);
}

static inline uint8_t rgb_pair_to_cb(const uint8_t *a, const uint8_t *b) {
    const int sum = -38 * ((int)a[0] + b[0]) - 74 * ((int)a[1] + b[1]) +
                    112 * ((int)a[2] + b[2]);
    return clamp8((sum + 65792) >> 9);
}

static inline uint8_t rgb_pair_to_cr(const uint8_t *a, const uint8_t *b) {
    const int sum = 112 * ((int)a[0] + b[0]) - 94 * ((int)a[1] + b[1]) -
                    18 * ((int)a[2] + b[2]);
    return clamp8((sum + 65792) >> 9);
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

bool xfb_rgba_to_yuv422(const uint8_t *rgba, uint32_t width, uint32_t height,
                        uint32_t src_pitch, uint8_t *yuv, uint32_t dst_pitch) {
    const uint32_t pair_bytes = ((width + 1u) >> 1) * 4u;
    if (!rgba || !yuv || width == 0 || height == 0 || src_pitch < width * 4u ||
        dst_pitch < pair_bytes)
        return false;

    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t *s = rgba + (size_t)y * src_pitch;
        uint8_t *d = yuv + (size_t)y * dst_pitch;
        for (uint32_t x = 0; x < width; x += 2) {
            const uint8_t *a = s + (size_t)x * 4u;
            const uint8_t *b = s + (size_t)(x + 1u < width ? x + 1u : x) * 4u;
            d[0] = rgb_to_y(a[0], a[1], a[2]);
            d[1] = rgb_pair_to_cb(a, b);
            d[2] = rgb_to_y(b[0], b[1], b[2]);
            d[3] = rgb_pair_to_cr(a, b);
            d += 4;
        }
    }
    return true;
}

bool xfb_rgba8_box_filter_2x(const uint8_t *src, uint32_t width, uint32_t height,
                             uint32_t src_pitch, uint8_t *dst, uint32_t dst_pitch) {
    const uint32_t dst_width = (width + 1u) >> 1;
    const uint32_t dst_height = (height + 1u) >> 1;
    if (!src || !dst || width == 0 || height == 0 || src_pitch < width * 4u ||
        dst_pitch < dst_width * 4u)
        return false;

    for (uint32_t y = 0; y < dst_height; ++y) {
        const uint32_t y0 = y * 2u;
        const uint32_t y1 = y0 + 1u < height ? y0 + 1u : y0;
        uint8_t *d = dst + (size_t)y * dst_pitch;
        for (uint32_t x = 0; x < dst_width; ++x) {
            const uint32_t x0 = x * 2u;
            const uint32_t x1 = x0 + 1u < width ? x0 + 1u : x0;
            const uint8_t *p00 = src + (size_t)y0 * src_pitch + (size_t)x0 * 4u;
            const uint8_t *p01 = src + (size_t)y0 * src_pitch + (size_t)x1 * 4u;
            const uint8_t *p10 = src + (size_t)y1 * src_pitch + (size_t)x0 * 4u;
            const uint8_t *p11 = src + (size_t)y1 * src_pitch + (size_t)x1 * 4u;
            for (uint32_t c = 0; c < 4; ++c)
                d[x * 4u + c] = (uint8_t)((p00[c] + p01[c] + p10[c] + p11[c] + 2u) >> 2);
        }
    }
    return true;
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

    const uint8_t white_pair[8] = {255, 255, 255, 255, 255, 255, 255, 255};
    uint8_t encoded[4] = {0};
    if (!xfb_rgba_to_yuv422(white_pair, 2, 1, 8, encoded, 4) ||
        encoded[0] != 235 || encoded[1] != 128 || encoded[2] != 235 || encoded[3] != 128) {
        ok = false;
    }

    const uint8_t source[3 * 3 * 4] = {
        0, 0, 0, 0,    4, 4, 4, 4,    8, 8, 8, 8,
        12, 12, 12, 12, 16, 16, 16, 16, 20, 20, 20, 20,
        24, 24, 24, 24, 28, 28, 28, 28, 32, 32, 32, 32,
    };
    uint8_t filtered[2 * 2 * 4] = {0};
    if (!xfb_rgba8_box_filter_2x(source, 3, 3, 12, filtered, 8) ||
        filtered[0] != 8 || filtered[4] != 14 || filtered[8] != 26 || filtered[12] != 32) {
        ok = false;
    }

    return ok;
}
