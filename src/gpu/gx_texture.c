// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu/gx_texture.h"

#include <string.h>

// Multi-byte fields inside encoded texture data are read explicitly rather than
// through a u16 cast so the decode is identical on any host endianness.
// Heavily based on Dolphin's VideoCommon/TextureDecoder_Generic.cpp.
static inline uint16_t rd_le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint16_t rd_be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

// Expand an N-bit channel to 8 bits by bit replication (Dolphin LookUpTables.h).
static inline uint8_t conv3to8(uint8_t v) { return (uint8_t)((v << 5) | (v << 2) | (v >> 1)); }
static inline uint8_t conv4to8(uint8_t v) { return (uint8_t)((v << 4) | v); }
static inline uint8_t conv5to8(uint8_t v) { return (uint8_t)((v << 3) | (v >> 2)); }
static inline uint8_t conv6to8(uint8_t v) { return (uint8_t)((v << 2) | (v >> 4)); }

static inline void put(uint8_t *px, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    px[0] = r; px[1] = g; px[2] = b; px[3] = a;
}

static inline void decode_ia8(uint8_t *px, uint16_t val) {
    uint8_t a = val & 0xFF, i = val >> 8;
    put(px, i, i, i, a);
}

static inline void decode_rgb565(uint8_t *px, uint16_t val) {
    put(px, conv5to8((val >> 11) & 0x1F), conv6to8((val >> 5) & 0x3F),
        conv5to8(val & 0x1F), 0xFF);
}

static inline void decode_rgb5a3(uint8_t *px, uint16_t val) {
    if (val & 0x8000) {
        put(px, conv5to8((val >> 10) & 0x1F), conv5to8((val >> 5) & 0x1F),
            conv5to8(val & 0x1F), 0xFF);
    } else {
        put(px, conv4to8((val >> 8) & 0xF), conv4to8((val >> 4) & 0xF),
            conv4to8(val & 0xF), conv3to8((val >> 12) & 0x7));
    }
}

// IA8 entries are little-endian, the RGB entries big-endian (Dolphin DecodePixel_Paletted).
static inline void decode_paletted(uint8_t *px, uint16_t idx, const uint8_t *tlut,
                                   GXTlutFormat tlutfmt) {
    const uint8_t *e = tlut + 2 * idx;
    switch (tlutfmt) {
    case GX_TLUT_IA8:    decode_ia8(px, rd_le16(e)); break;
    case GX_TLUT_RGB565: decode_rgb565(px, rd_be16(e)); break;
    case GX_TLUT_RGB5A3: decode_rgb5a3(px, rd_be16(e)); break;
    }
}

typedef struct { uint8_t r, g, b, a; } RGBA;

// 3/8 blend
static inline uint8_t dxt_blend(uint8_t v1, uint8_t v2) {
    return (uint8_t)((v1 * 3 + v2 * 5) >> 3);
}

static void decode_dxt_block(uint8_t *dst, int width, int x0, int y0, const uint8_t *b) {
    uint16_t c1 = rd_be16(b), c2 = rd_be16(b + 2);
    uint8_t r1 = conv5to8((c1 >> 11) & 0x1F), g1 = conv6to8((c1 >> 5) & 0x3F), b1 = conv5to8(c1 & 0x1F);
    uint8_t r2 = conv5to8((c2 >> 11) & 0x1F), g2 = conv6to8((c2 >> 5) & 0x3F), b2 = conv5to8(c2 & 0x1F);
    RGBA colors[4];
    colors[0] = (RGBA){r1, g1, b1, 255};
    colors[1] = (RGBA){r2, g2, b2, 255};
    if (c1 > c2) {
        colors[2] = (RGBA){dxt_blend(r2, r1), dxt_blend(g2, g1), dxt_blend(b2, b1), 255};
        colors[3] = (RGBA){dxt_blend(r1, r2), dxt_blend(g1, g2), dxt_blend(b1, b2), 255};
    } else {
        uint8_t ra = (r1 + r2) / 2, ga = (g1 + g2) / 2, ba = (b1 + b2) / 2;
        colors[2] = (RGBA){ra, ga, ba, 255};
        // GCN differs from DXT1  in that index 3 is the same average colour.
        colors[3] = (RGBA){ra, ga, ba, 0};
    }
    for (int y = 0; y < 4; y++) {
        int val = b[4 + y];
        for (int x = 0; x < 4; x++) {
            const RGBA *c = &colors[(val >> 6) & 3];
            put(dst + ((y0 + y) * width + (x0 + x)) * 4, c->r, c->g, c->b, c->a);
            val <<= 2;
        }
    }
}

int gx_texture_block_width(GXTextureFormat fmt) {
    switch (fmt) {
    case GX_TF_I4: case GX_TF_C4: case GX_TF_CMPR: return 8;
    case GX_TF_I8: case GX_TF_IA4: case GX_TF_C8:  return 8;
    default:                                       return 4;
    }
}

int gx_texture_block_height(GXTextureFormat fmt) {
    switch (fmt) {
    case GX_TF_I4: case GX_TF_C4: case GX_TF_CMPR: return 8;
    default:                                       return 4;
    }
}

// Texel size in nibbles.
static int texel_nibbles(GXTextureFormat fmt) {
    switch (fmt) {
    case GX_TF_I4: case GX_TF_C4: case GX_TF_CMPR:          return 1;
    case GX_TF_I8: case GX_TF_IA4: case GX_TF_C8:           return 2;
    case GX_TF_IA8: case GX_TF_RGB565: case GX_TF_RGB5A3:
    case GX_TF_C14X2:                                       return 4;
    case GX_TF_RGBA8:                                       return 8;
    default:                                                return 0;
    }
}

int gx_texture_encoded_size(int width, int height, GXTextureFormat fmt) {
    const int bw = gx_texture_block_width(fmt), bh = gx_texture_block_height(fmt);
    const int pw = (width + bw - 1) / bw * bw;
    const int ph = (height + bh - 1) / bh * bh;
    return pw * ph * texel_nibbles(fmt) / 2;
}

int gx_texture_palette_size(GXTextureFormat fmt) {
    switch (fmt) {
    case GX_TF_C4:    return 16 * 2;
    case GX_TF_C8:    return 256 * 2;
    case GX_TF_C14X2: return 16384 * 2;
    default:          return 0;
    }
}

int gx_texture_decode(uint8_t *dst, const uint8_t *src, int width, int height,
                      GXTextureFormat fmt, const uint8_t *tlut, GXTlutFormat tlutfmt) {
    const int ws4 = (width + 3) / 4;
    const int ws8 = (width + 7) / 8;

    switch (fmt) {
    case GX_TF_I4:
        for (int y = 0; y < height; y += 8)
            for (int x = 0; x < width; x += 8)
                for (int iy = 0; iy < 8; iy++, src += 4)
                    for (int ix = 0; ix < 4; ix++) {
                        uint8_t i1 = conv4to8(src[ix] >> 4), i2 = conv4to8(src[ix] & 0xF);
                        uint8_t *row = dst + ((y + iy) * width + x + ix * 2) * 4;
                        put(row, i1, i1, i1, i1);
                        put(row + 4, i2, i2, i2, i2);
                    }
        return 1;
    case GX_TF_I8:
        for (int y = 0; y < height; y += 4)
            for (int x = 0; x < width; x += 8)
                for (int iy = 0; iy < 4; iy++, src += 8)
                    for (int ix = 0; ix < 8; ix++) {
                        uint8_t v = src[ix];
                        put(dst + ((y + iy) * width + x + ix) * 4, v, v, v, v);
                    }
        return 1;
    case GX_TF_IA4:
        for (int y = 0; y < height; y += 4)
            for (int x = 0, ys = (y / 4) * ws8; x < width; x += 8, ys++)
                for (int iy = 0, xs = 4 * ys; iy < 4; iy++, xs++) {
                    const uint8_t *s = src + 8 * xs;
                    uint8_t *d = dst + ((y + iy) * width + x) * 4;
                    for (int ix = 0; ix < 8; ix++) {
                        uint8_t a = conv4to8(s[ix] >> 4), l = conv4to8(s[ix] & 0xF);
                        put(d + ix * 4, l, l, l, a);
                    }
                }
        return 1;
    case GX_TF_IA8:
        for (int y = 0; y < height; y += 4)
            for (int x = 0; x < width; x += 4)
                for (int iy = 0; iy < 4; iy++, src += 8)
                    for (int ix = 0; ix < 4; ix++)
                        decode_ia8(dst + ((y + iy) * width + x + ix) * 4, rd_le16(src + ix * 2));
        return 1;
    case GX_TF_RGB565:
        for (int y = 0; y < height; y += 4)
            for (int x = 0; x < width; x += 4)
                for (int iy = 0; iy < 4; iy++, src += 8)
                    for (int ix = 0; ix < 4; ix++)
                        decode_rgb565(dst + ((y + iy) * width + x + ix) * 4, rd_be16(src + ix * 2));
        return 1;
    case GX_TF_RGB5A3:
        for (int y = 0; y < height; y += 4)
            for (int x = 0; x < width; x += 4)
                for (int iy = 0; iy < 4; iy++, src += 8)
                    for (int ix = 0; ix < 4; ix++)
                        decode_rgb5a3(dst + ((y + iy) * width + x + ix) * 4, rd_be16(src + ix * 2));
        return 1;
    case GX_TF_RGBA8:
        // 4x4 tile of 64 bytes. 32 bytes of A/R pairs and 32 bytes of G/B pairs.
        for (int y = 0; y < height; y += 4)
            for (int x = 0; x < width; x += 4, src += 64)
                for (int iy = 0; iy < 4; iy++)
                    for (int ix = 0; ix < 4; ix++) {
                        const uint8_t *ar = src + 8 * iy + ix * 2;
                        const uint8_t *gb = src + 32 + 8 * iy + ix * 2;
                        put(dst + ((y + iy) * width + x + ix) * 4, ar[1], gb[0], gb[1], ar[0]);
                    }
        return 1;
    case GX_TF_C4:
        for (int y = 0; y < height; y += 8)
            for (int x = 0, ys = (y / 8) * ws8; x < width; x += 8, ys++)
                for (int iy = 0, xs = 8 * ys; iy < 8; iy++, xs++) {
                    const uint8_t *s = src + 4 * xs;
                    uint8_t *d = dst + ((y + iy) * width + x) * 4;
                    for (int ix = 0; ix < 4; ix++) {
                        decode_paletted(d + (ix * 2) * 4, s[ix] >> 4, tlut, tlutfmt);
                        decode_paletted(d + (ix * 2 + 1) * 4, s[ix] & 0xF, tlut, tlutfmt);
                    }
                }
        return 1;
    case GX_TF_C8:
        for (int y = 0; y < height; y += 4)
            for (int x = 0, ys = (y / 4) * ws8; x < width; x += 8, ys++)
                for (int iy = 0, xs = 4 * ys; iy < 4; iy++, xs++) {
                    const uint8_t *s = src + 8 * xs;
                    uint8_t *d = dst + ((y + iy) * width + x) * 4;
                    for (int ix = 0; ix < 8; ix++)
                        decode_paletted(d + ix * 4, s[ix], tlut, tlutfmt);
                }
        return 1;
    case GX_TF_C14X2:
        for (int y = 0; y < height; y += 4)
            for (int x = 0, ys = (y / 4) * ws4; x < width; x += 4, ys++)
                for (int iy = 0, xs = 4 * ys; iy < 4; iy++, xs++) {
                    const uint8_t *s = src + 8 * xs;
                    uint8_t *d = dst + ((y + iy) * width + x) * 4;
                    for (int ix = 0; ix < 4; ix++)
                        decode_paletted(d + ix * 4, rd_be16(s + ix * 2) & 0x3FFF, tlut, tlutfmt);
                }
        return 1;
    case GX_TF_CMPR:
        for (int y = 0; y < height; y += 8)
            for (int x = 0; x < width; x += 8) {
                decode_dxt_block(dst, width, x,     y,     src);      // 32 bytes = 4 blocks
                decode_dxt_block(dst, width, x + 4, y,     src + 8);
                decode_dxt_block(dst, width, x,     y + 4, src + 16);
                decode_dxt_block(dst, width, x + 4, y + 4, src + 24);
                src += 32;
            }
        return 1;
    default:
        return 0;
    }
}

// Self test
static int px_eq(const uint8_t *p, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return p[0] == r && p[1] == g && p[2] == b && p[3] == a;
}

int gx_texture_selftest(void) {
    uint8_t out[8 * 8 * 4];

    // Sizes
    if (gx_texture_encoded_size(4, 4, GX_TF_RGBA8) != 64) return 0;
    if (gx_texture_encoded_size(8, 8, GX_TF_CMPR) != 32) return 0;
    if (gx_texture_encoded_size(8, 8, GX_TF_I4) != 32) return 0;
    // Non-tile dimensions round up (i.e. 5x3 RGB565 -> 8x4 texels * 2B = 64B).
    if (gx_texture_encoded_size(5, 3, GX_TF_RGB565) != 64) return 0;

    // I4
    {
        uint8_t src[8 * 8 / 2] = {0};
        src[0] = 0x5A;
        memset(out, 0, sizeof(out));
        gx_texture_decode(out, src, 8, 8, GX_TF_I4, NULL, 0);
        if (!px_eq(out + 0, 0x55, 0x55, 0x55, 0x55)) return 0;
        if (!px_eq(out + 4, 0xAA, 0xAA, 0xAA, 0xAA)) return 0;
    }
    // I8
    {
        uint8_t src[8 * 4 * 2] = {0};
        src[0] = 0x3C;
        memset(out, 0, sizeof(out));
        gx_texture_decode(out, src, 8, 4, GX_TF_I8, NULL, 0);
        if (!px_eq(out, 0x3C, 0x3C, 0x3C, 0x3C)) return 0;
    }
    // IA4
    {
        uint8_t src[8 * 4] = {0};
        src[0] = 0x2F;
        memset(out, 0, sizeof(out));
        gx_texture_decode(out, src, 8, 4, GX_TF_IA4, NULL, 0);
        if (!px_eq(out, 0xFF, 0xFF, 0xFF, 0x22)) return 0;
    }
    // IA8
    {
        uint8_t src[4 * 4 * 2] = {0};
        src[0] = 0x40; src[1] = 0x80;
        memset(out, 0, sizeof(out));
        gx_texture_decode(out, src, 4, 4, GX_TF_IA8, NULL, 0);
        if (!px_eq(out, 0x80, 0x80, 0x80, 0x40)) return 0;
    }
    // RGB565
    {
        uint8_t src[4 * 4 * 2] = {0};
        src[0] = 0xF8; src[1] = 0x00;
        memset(out, 0, sizeof(out));
        gx_texture_decode(out, src, 4, 4, GX_TF_RGB565, NULL, 0);
        if (!px_eq(out, 0xFF, 0x00, 0x00, 0xFF)) return 0;
    }
    // RGB5A3
    {
        uint8_t src[4 * 4 * 2] = {0};
        src[0] = 0xFC; src[1] = 0x00;  // pixel 0
        src[2] = 0x0F; src[3] = 0xFF;  // pixel 1
        memset(out, 0, sizeof(out));
        gx_texture_decode(out, src, 4, 4, GX_TF_RGB5A3, NULL, 0);
        if (!px_eq(out + 0, 0xFF, 0x00, 0x00, 0xFF)) return 0;
        if (!px_eq(out + 4, 0xFF, 0xFF, 0xFF, 0x00)) return 0;
    }
    // RGBA8
    {
        uint8_t src[4 * 4 * 4] = {0};
        src[0] = 0x11; src[1] = 0x22; src[32] = 0x33; src[33] = 0x44;
        memset(out, 0, sizeof(out));
        gx_texture_decode(out, src, 4, 4, GX_TF_RGBA8, NULL, 0);
        if (!px_eq(out, 0x22, 0x33, 0x44, 0x11)) return 0;
    }
    // C8 with an RGB565 palette
    {
        uint8_t src[8 * 4] = {0};
        uint8_t pal[256 * 2] = {0};
        src[0] = 0x01;
        pal[2] = 0x07; pal[3] = 0xE0;  // entry 1 = green
        memset(out, 0, sizeof(out));
        gx_texture_decode(out, src, 8, 4, GX_TF_C8, pal, GX_TLUT_RGB565);
        if (!px_eq(out, 0x00, 0xFF, 0x00, 0xFF)) return 0;
    }
    // C4
    {
        uint8_t src[8 * 8 / 2] = {0};
        uint8_t pal[16 * 2] = {0};
        src[0] = 0x30;                 // texel 0 = entry 3, texel 1 = entry 0
        pal[6] = 0xF8; pal[7] = 0x00;  // entry 3, red
        memset(out, 0, sizeof(out));
        gx_texture_decode(out, src, 8, 8, GX_TF_C4, pal, GX_TLUT_RGB565);
        if (!px_eq(out, 0xFF, 0x00, 0x00, 0xFF)) return 0;
    }
    // C14X2 with an RGB565 palette
    {
        uint8_t src[4 * 4 * 2] = {0};
        uint8_t pal[16384 * 2] = {0};
        src[0] = 0xC0; src[1] = 0x05;  // big-endian 0xC005 & 0x3FFF = 5
        pal[10] = 0x00; pal[11] = 0x1F;  // entry 5, blue
        memset(out, 0, sizeof(out));
        gx_texture_decode(out, src, 4, 4, GX_TF_C14X2, pal, GX_TLUT_RGB565);
        if (!px_eq(out, 0x00, 0x00, 0xFF, 0xFF)) return 0;
    }
    // CMPR
    {
        uint8_t src[32] = {0};
        src[0] = 0xFF; src[1] = 0xFF;  // color1 = white
        // color2 and indices left zero
        memset(out, 0, sizeof(out));
        gx_texture_decode(out, src, 8, 8, GX_TF_CMPR, NULL, 0);
        if (!px_eq(out, 0xFF, 0xFF, 0xFF, 0xFF)) return 0;
    }
    // CMPR transparent path
    {
        uint8_t src[32] = {0};
        src[2] = 0xFF; src[3] = 0xFF;  // color2 = white, color1 = 0
        src[4] = 0xC0;                 // row 0 texel 0 = index 3
        memset(out, 0, sizeof(out));
        gx_texture_decode(out, src, 8, 8, GX_TF_CMPR, NULL, 0);
        if (!px_eq(out, 127, 127, 127, 0)) return 0;
    }

    return 1;
}
