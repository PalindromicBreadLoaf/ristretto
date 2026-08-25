// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu/gx_texture.h"

#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/utils.h>
#include <gx2r/surface.h>

#include <stdlib.h>
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

// EFB-copy encode
GXTextureFormat gx_texture_copy_layout(GXCopyFormat fmt) {
    switch (fmt) {
    case GX_COPY_R4:                        return GX_TF_I4;
    case GX_COPY_R8_1: case GX_COPY_A8:
    case GX_COPY_R8: case GX_COPY_G8:
    case GX_COPY_B8:                        return GX_TF_I8;
    case GX_COPY_RA4:                       return GX_TF_IA4;
    case GX_COPY_RA8: case GX_COPY_RG8:
    case GX_COPY_GB8:                       return GX_TF_IA8;
    case GX_COPY_RGB565:                    return GX_TF_RGB565;
    case GX_COPY_RGB5A3:                    return GX_TF_RGB5A3;
    case GX_COPY_RGBA8:                     return GX_TF_RGBA8;
    default:                                return GX_TF_I8;
    }
}

// BT.601 luma from 8-bit RGB (Dolphin TextureEncoder RGB8_to_I).
static inline uint8_t rgb_to_i(uint8_t r, uint8_t g, uint8_t b) {
    uint32_t val = 4096u + 66u * r + 129u * g + 25u * b;
    val >>= 8;
    return val > 255u ? 255u : (uint8_t)val;
}

// One-byte grey/single-channel value a copy stores for the given format.
static inline uint8_t copy_channel(const uint8_t *px, GXCopyFormat fmt, bool intensity) {
    switch (fmt) {
    case GX_COPY_A8: return px[3];
    case GX_COPY_G8: return px[1];
    case GX_COPY_B8: return px[2];
    default:         return intensity ? rgb_to_i(px[0], px[1], px[2]) : px[0];
    }
}

static inline uint16_t enc_rgb565(const uint8_t *px) {
    return (uint16_t)(((px[0] >> 3) << 11) | ((px[1] >> 2) << 5) | (px[2] >> 3));
}

static inline uint16_t enc_rgb5a3(const uint8_t *px) {
    if (px[3] >= 224) {
        return (uint16_t)(0x8000u | ((px[0] >> 3) << 10) | ((px[1] >> 3) << 5) | (px[2] >> 3));
    }
    return (uint16_t)(((px[3] >> 5) << 12) | ((px[0] >> 4) << 8) | ((px[1] >> 4) << 4) |
                      (px[2] >> 4));
}

static inline void wr_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

// Clamp a sample coordinate to the region and return the source texel.
static inline const uint8_t *sample(const uint8_t *src, uint32_t src_pitch, int width,
                                    int height, int x, int y) {
    if (x >= width) x = width - 1;
    if (y >= height) y = height - 1;
    return src + (size_t)y * src_pitch + (size_t)x * 4u;
}

int gx_texture_encode_copy(uint8_t *dst, const uint8_t *src, int width, int height,
                           uint32_t src_pitch, GXCopyFormat fmt, bool intensity) {
    if (!dst || !src || width <= 0 || height <= 0) return 0;
    const GXTextureFormat layout = gx_texture_copy_layout(fmt);
    uint8_t *d = dst;

    switch (layout) {
    case GX_TF_I4:  // R4: 8x8 tiles, two 4-bit texels per byte
        for (int y = 0; y < height; y += 8)
            for (int x = 0; x < width; x += 8)
                for (int iy = 0; iy < 8; iy++)
                    for (int ix = 0; ix < 4; ix++) {
                        uint8_t hi = copy_channel(sample(src, src_pitch, width, height,
                                                         x + ix * 2, y + iy), fmt, intensity);
                        uint8_t lo = copy_channel(sample(src, src_pitch, width, height,
                                                         x + ix * 2 + 1, y + iy), fmt, intensity);
                        *d++ = (uint8_t)((hi & 0xF0u) | (lo >> 4));
                    }
        break;
    case GX_TF_I8:  // R8/A8/single channel: 8x4 tiles, one byte per texel
        for (int y = 0; y < height; y += 4)
            for (int x = 0; x < width; x += 8)
                for (int iy = 0; iy < 4; iy++)
                    for (int ix = 0; ix < 8; ix++)
                        *d++ = copy_channel(sample(src, src_pitch, width, height,
                                                   x + ix, y + iy), fmt, intensity);
        break;
    case GX_TF_IA4:  // RA4: 8x4 tiles, high nibble alpha, low nibble luma
        for (int y = 0; y < height; y += 4)
            for (int x = 0; x < width; x += 8)
                for (int iy = 0; iy < 4; iy++)
                    for (int ix = 0; ix < 8; ix++) {
                        const uint8_t *px = sample(src, src_pitch, width, height,
                                                   x + ix, y + iy);
                        uint8_t lum = intensity ? rgb_to_i(px[0], px[1], px[2]) : px[0];
                        *d++ = (uint8_t)((px[3] & 0xF0u) | (lum >> 4));
                    }
        break;
    case GX_TF_IA8:  // RA8/RG8/GB8: 4x4 tiles, byte0 = second channel, byte1 = first
        for (int y = 0; y < height; y += 4)
            for (int x = 0; x < width; x += 4)
                for (int iy = 0; iy < 4; iy++)
                    for (int ix = 0; ix < 4; ix++) {
                        const uint8_t *px = sample(src, src_pitch, width, height,
                                                   x + ix, y + iy);
                        uint8_t lo, hi;  // decoded as a=byte0, i=byte1
                        if (fmt == GX_COPY_RG8)      { lo = px[1]; hi = px[0]; }
                        else if (fmt == GX_COPY_GB8) { lo = px[2]; hi = px[1]; }
                        else { lo = px[3]; hi = intensity ? rgb_to_i(px[0], px[1], px[2]) : px[0]; }
                        *d++ = lo;
                        *d++ = hi;
                    }
        break;
    case GX_TF_RGB565:
        for (int y = 0; y < height; y += 4)
            for (int x = 0; x < width; x += 4)
                for (int iy = 0; iy < 4; iy++)
                    for (int ix = 0; ix < 4; ix++, d += 2)
                        wr_be16(d, enc_rgb565(sample(src, src_pitch, width, height,
                                                     x + ix, y + iy)));
        break;
    case GX_TF_RGB5A3:
        for (int y = 0; y < height; y += 4)
            for (int x = 0; x < width; x += 4)
                for (int iy = 0; iy < 4; iy++)
                    for (int ix = 0; ix < 4; ix++, d += 2)
                        wr_be16(d, enc_rgb5a3(sample(src, src_pitch, width, height,
                                                     x + ix, y + iy)));
        break;
    case GX_TF_RGBA8:  // 4x4 tiles: 32 bytes of A/R pairs then 32 bytes of G/B pairs
        for (int y = 0; y < height; y += 4)
            for (int x = 0; x < width; x += 4, d += 64)
                for (int iy = 0; iy < 4; iy++)
                    for (int ix = 0; ix < 4; ix++) {
                        const uint8_t *px = sample(src, src_pitch, width, height,
                                                   x + ix, y + iy);
                        uint8_t *ar = d + 8 * iy + ix * 2;
                        uint8_t *gb = d + 32 + 8 * iy + ix * 2;
                        ar[0] = px[3]; ar[1] = px[0];
                        gb[0] = px[1]; gb[1] = px[2];
                    }
        break;
    default:
        return 0;
    }
    return gx_texture_encoded_size(width, height, layout);
}

typedef struct {
    GXTextureFormat fmt;
    GXTlutFormat    tlutfmt;
    uint32_t        width;
    uint32_t        height;
    uint32_t        levels;
    uint32_t        source_ea;
    uint32_t        source_size;
    uint32_t        tlut_offset;
    uint32_t        tlut_size;
} GXTextureDesc;

static bool is_valid_texture_format(GXTextureFormat fmt) {
    switch (fmt) {
    case GX_TF_I4: case GX_TF_I8: case GX_TF_IA4: case GX_TF_IA8:
    case GX_TF_RGB565: case GX_TF_RGB5A3: case GX_TF_RGBA8:
    case GX_TF_C4: case GX_TF_C8: case GX_TF_C14X2: case GX_TF_CMPR:
        return true;
    default:
        return false;
    }
}

static bool is_valid_tlut_format(GXTlutFormat fmt) {
    return fmt == GX_TLUT_IA8 || fmt == GX_TLUT_RGB565 || fmt == GX_TLUT_RGB5A3;
}

static uint32_t mip_dimension(uint32_t dimension, uint32_t level) {
    dimension >>= level;
    return dimension ? dimension : 1;
}

static uint32_t max_mip_levels(uint32_t width, uint32_t height) {
    uint32_t levels = 1;
    while (width > 1 || height > 1) {
        width = mip_dimension(width, 1);
        height = mip_dimension(height, 1);
        ++levels;
    }
    return levels;
}

static bool texture_desc_from_unit(const GXTextureUnit *unit, GXTextureDesc *out) {
    const uint32_t mode0 = unit->mode0;
    const uint32_t mode1 = unit->mode1;
    const uint32_t image0 = unit->image0;
    const GXTextureFormat fmt = (GXTextureFormat)((image0 >> 20) & 0xFu);
    const uint32_t width = (image0 & 0x3FFu) + 1u;
    const uint32_t height = ((image0 >> 10) & 0x3FFu) + 1u;

    if (!is_valid_texture_format(fmt)) return false;
    if (unit->image1 & (1u << 21)) return false;

    uint32_t levels = 1;
    const uint32_t mip_filter = (mode0 >> 5) & 3u;
    if (mip_filter != 0) {
        uint32_t requested = (((mode1 >> 8) & 0xFFu) + 15u) / 16u;
        uint32_t available = max_mip_levels(width, height) - 1u;
        if (requested > available) requested = available;
        levels += requested;
    }

    uint64_t source_size = 0;
    for (uint32_t level = 0; level < levels; ++level) {
        int bytes = gx_texture_encoded_size((int)mip_dimension(width, level),
                                            (int)mip_dimension(height, level), fmt);
        if (bytes <= 0) return false;
        source_size += (uint32_t)bytes;
    }
    const uint32_t source_ea = (unit->image3 & 0xFFFFFFu) << 5;
    if (source_size > UINT32_MAX || source_size > UINT64_C(0x100000000) - source_ea)
        return false;

    const uint32_t tlut_size = (uint32_t)gx_texture_palette_size(fmt);
    const GXTlutFormat tlutfmt = (GXTlutFormat)((unit->tlut >> 10) & 3u);
    const uint32_t tlut_offset = (unit->tlut & 0x3FFu) << 9;
    if (tlut_size && (!is_valid_tlut_format(tlutfmt) ||
                      tlut_offset > GX_TEXTURE_TMEM_SIZE - tlut_size))
        return false;

    *out = (GXTextureDesc){
        .fmt = fmt,
        .tlutfmt = tlutfmt,
        .width = width,
        .height = height,
        .levels = levels,
        .source_ea = source_ea,
        .source_size = (uint32_t)source_size,
        .tlut_offset = tlut_offset,
        .tlut_size = tlut_size,
    };
    return true;
}

static uint64_t hash_bytes(const uint8_t *data, uint32_t size) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (uint32_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint32_t canonical_guest_ea(uint32_t ea) {
    return ea & 0x1FFFFFFFu;
}

static bool ranges_overlap(uint32_t ea_a, uint32_t size_a, uint32_t ea_b, uint32_t size_b) {
    const uint64_t begin_a = canonical_guest_ea(ea_a);
    const uint64_t end_a = begin_a + size_a;
    const uint64_t begin_b = canonical_guest_ea(ea_b);
    const uint64_t end_b = begin_b + size_b;
    return begin_a < end_b && begin_b < end_a;
}

static bool entry_matches(const GXTextureCacheEntry *entry, const GXTextureUnit *unit,
                          const GXTextureDesc *desc) {
    return entry->source_ea == desc->source_ea && entry->source_size == desc->source_size &&
           entry->tlut_offset == desc->tlut_offset && entry->tlut_size == desc->tlut_size &&
           entry->levels == desc->levels &&
           entry->image0 == unit->image0 && entry->image1 == unit->image1 &&
           entry->image2 == unit->image2 && entry->image3 == unit->image3 &&
           entry->tlut == unit->tlut;
}

static void destroy_entry(GXTextureCacheEntry *entry, bool wait_for_gpu) {
    if (entry->allocated) {
        if (wait_for_gpu) GX2DrawDone();
        GX2RDestroySurfaceEx(&entry->texture.surface, GX2R_RESOURCE_BIND_NONE);
    }
    memset(entry, 0, sizeof(*entry));
}

static void invalidate_tlut_range(GXTextureCache *cache, uint32_t offset, uint32_t size) {
    for (uint32_t i = 0; i < GX_TEXTURE_CACHE_CAP; ++i) {
        GXTextureCacheEntry *entry = &cache->entry[i];
        if (entry->tlut_size && ranges_overlap(entry->tlut_offset, entry->tlut_size, offset, size))
            entry->stale = true;
    }
}

static GXTextureCacheEntry *find_entry(GXTextureCache *cache, const GXTextureUnit *unit,
                                        const GXTextureDesc *desc) {
    for (uint32_t i = 0; i < GX_TEXTURE_CACHE_CAP; ++i)
        if (entry_matches(&cache->entry[i], unit, desc)) return &cache->entry[i];

    GXTextureCacheEntry *oldest = &cache->entry[0];
    for (uint32_t i = 0; i < GX_TEXTURE_CACHE_CAP; ++i) {
        GXTextureCacheEntry *entry = &cache->entry[i];
        if (!entry->allocated && !entry->valid && entry->source_size == 0) return entry;
        if (entry->last_used < oldest->last_used) oldest = entry;
    }
    destroy_entry(oldest, true);
    return oldest;
}

static bool create_surface(GXTextureCacheEntry *entry, const GXTextureDesc *desc) {
    if (entry->allocated) destroy_entry(entry, true);

    GX2Texture *texture = &entry->texture;
    memset(texture, 0, sizeof(*texture));
    texture->surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    texture->surface.width = desc->width;
    texture->surface.height = desc->height;
    texture->surface.depth = 1;
    texture->surface.mipLevels = desc->levels;
    texture->surface.format = GX_TEXTURE_GX2_FORMAT;
    texture->surface.aa = GX2_AA_MODE1X;
    texture->surface.use = GX2_SURFACE_USE_TEXTURE;
    texture->surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    texture->viewNumMips = desc->levels;
    texture->viewNumSlices = 1;
    texture->compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A);

    if (!GX2RCreateSurface(&texture->surface, GX2R_RESOURCE_BIND_TEXTURE |
                            GX2R_RESOURCE_USAGE_CPU_WRITE | GX2R_RESOURCE_USAGE_GPU_READ)) {
        memset(texture, 0, sizeof(*texture));
        return false;
    }
    GX2InitTextureRegs(texture);
    entry->allocated = true;
    return true;
}

static uint32_t surface_pitch_for_level(const GX2Surface *surface, uint32_t level,
                                        uint32_t width) {
    if (level == 0) return surface->pitch;

    uint32_t pitch = 1;
    while (pitch < width) pitch <<= 1;
    return pitch < 64 ? 64 : pitch;
}

static bool upload_entry(GXTextureCacheEntry *entry, const GXTextureUnit *unit,
                         const GXTextureDesc *desc, const uint8_t *source,
                         const uint8_t *tlut) {
    if (!entry->allocated && !create_surface(entry, desc)) return false;

    uint32_t source_offset = 0;
    for (uint32_t level = 0; level < desc->levels; ++level) {
        const uint32_t width = mip_dimension(desc->width, level);
        const uint32_t height = mip_dimension(desc->height, level);
        const uint32_t expanded_width = (width + (uint32_t)gx_texture_block_width(desc->fmt) - 1u) /
                                        (uint32_t)gx_texture_block_width(desc->fmt) *
                                        (uint32_t)gx_texture_block_width(desc->fmt);
        const uint32_t expanded_height = (height + (uint32_t)gx_texture_block_height(desc->fmt) - 1u) /
                                         (uint32_t)gx_texture_block_height(desc->fmt) *
                                         (uint32_t)gx_texture_block_height(desc->fmt);
        const int encoded_size = gx_texture_encoded_size((int)width, (int)height, desc->fmt);
        const size_t decoded_size = (size_t)expanded_width * expanded_height * 4u;
        uint8_t *decoded = malloc(decoded_size);
        if (!decoded) return false;

        bool decoded_ok = gx_texture_decode(decoded, source + source_offset,
                                            (int)expanded_width, (int)expanded_height,
                                            desc->fmt, tlut, desc->tlutfmt) != 0;
        uint8_t *dst = decoded_ok ? GX2RLockSurfaceEx(&entry->texture.surface, (int32_t)level,
                                                       GX2R_RESOURCE_BIND_NONE) : NULL;
        if (!dst) {
            free(decoded);
            return false;
        }
        const uint32_t pitch = surface_pitch_for_level(&entry->texture.surface, level, width);
        for (uint32_t y = 0; y < height; ++y)
            memcpy(dst + (size_t)y * pitch * 4u,
                   decoded + (size_t)y * expanded_width * 4u, (size_t)width * 4u);
        GX2RUnlockSurfaceEx(&entry->texture.surface, (int32_t)level, GX2R_RESOURCE_BIND_NONE);
        free(decoded);
        source_offset += (uint32_t)encoded_size;
    }

    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, entry->texture.surface.image,
                  entry->texture.surface.imageSize);
    if (entry->texture.surface.mipmaps && entry->texture.surface.mipmapSize)
        GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, entry->texture.surface.mipmaps,
                      entry->texture.surface.mipmapSize);

    entry->source_ea = desc->source_ea;
    entry->source_size = desc->source_size;
    entry->tlut_offset = desc->tlut_offset;
    entry->tlut_size = desc->tlut_size;
    entry->levels = desc->levels;
    entry->image0 = unit->image0;
    entry->image1 = unit->image1;
    entry->image2 = unit->image2;
    entry->image3 = unit->image3;
    entry->tlut = unit->tlut;
    return true;
}

static GX2TexClampMode clamp_mode(uint32_t mode) {
    switch (mode & 3u) {
    case 1: return GX2_TEX_CLAMP_MODE_WRAP;
    case 2: return GX2_TEX_CLAMP_MODE_MIRROR;
    default: return GX2_TEX_CLAMP_MODE_CLAMP;
    }
}

static GX2TexAnisoRatio aniso_mode(uint32_t mode) {
    switch ((mode >> 19) & 3u) {
    case 1: return GX2_TEX_ANISO_RATIO_2_TO_1;
    case 2: return GX2_TEX_ANISO_RATIO_4_TO_1;
    default: return GX2_TEX_ANISO_RATIO_NONE;
    }
}

static GX2TexMipFilterMode mip_filter_mode(uint32_t mode) {
    switch ((mode >> 5) & 3u) {
    case 2: return GX2_TEX_MIP_FILTER_MODE_LINEAR;
    case 0: return GX2_TEX_MIP_FILTER_MODE_NONE;
    default: return GX2_TEX_MIP_FILTER_MODE_POINT;
    }
}

static void build_sampler(GX2Sampler *sampler, uint32_t mode0, uint32_t mode1) {
    const GX2TexXYFilterMode mag = (mode0 & (1u << 4)) ? GX2_TEX_XY_FILTER_MODE_LINEAR
                                                         : GX2_TEX_XY_FILTER_MODE_POINT;
    const GX2TexXYFilterMode min = (mode0 & (1u << 7)) ? GX2_TEX_XY_FILTER_MODE_LINEAR
                                                         : GX2_TEX_XY_FILTER_MODE_POINT;
    const GX2TexMipFilterMode mip = mip_filter_mode(mode0);
    const int32_t raw_bias = (int8_t)((mode0 >> 9) & 0xFFu);
    float lod_min = (float)(mode1 & 0xFFu) / 16.0f;
    float lod_max = (float)((mode1 >> 8) & 0xFFu) / 16.0f;
    float lod_bias = (float)raw_bias / 32.0f;
    if (mip == GX2_TEX_MIP_FILTER_MODE_NONE) {
        lod_min = 0.0f;
        lod_max = 0.0f;
        lod_bias = 0.0f;
    } else if (lod_min > lod_max) {
        lod_min = lod_max;
    }

    GX2InitSampler(sampler, clamp_mode(mode0), min);
    GX2InitSamplerClamping(sampler, clamp_mode(mode0), clamp_mode(mode0 >> 2),
                           GX2_TEX_CLAMP_MODE_CLAMP);
    GX2InitSamplerXYFilter(sampler, mag, min, aniso_mode(mode0));
    GX2InitSamplerZMFilter(sampler, GX2_TEX_Z_FILTER_MODE_NONE, mip);
    GX2InitSamplerLOD(sampler, lod_min, lod_max, lod_bias);
}

void gx_texture_sampler_from_unit(GX2Sampler *sampler, const GXTextureUnit *unit) {
    if (!sampler || !unit) return;
    build_sampler(sampler, unit->mode0, unit->mode1);
}

bool gx_texture_cache_init(GXTextureCache *cache, GXTextureGuestRead read_guest, void *user) {
    if (!cache) return false;
    memset(cache, 0, sizeof(*cache));
    cache->tmem = calloc(1, GX_TEXTURE_TMEM_SIZE);
    if (!cache->tmem) return false;
    cache->read_guest = read_guest;
    cache->user = user;
    return true;
}

void gx_texture_cache_destroy(GXTextureCache *cache) {
    if (!cache) return;
    for (uint32_t i = 0; i < GX_TEXTURE_CACHE_CAP; ++i)
        destroy_entry(&cache->entry[i], true);
    free(cache->tmem);
    memset(cache, 0, sizeof(*cache));
}

void gx_texture_cache_destroy_after_gpu_idle(GXTextureCache *cache) {
    if (!cache) return;
    for (uint32_t i = 0; i < GX_TEXTURE_CACHE_CAP; ++i)
        destroy_entry(&cache->entry[i], false);
    free(cache->tmem);
    memset(cache, 0, sizeof(*cache));
}

void gx_texture_cache_invalidate_all(GXTextureCache *cache) {
    if (!cache) return;
    for (uint32_t i = 0; i < GX_TEXTURE_CACHE_CAP; ++i)
        cache->entry[i].stale = true;
}

void gx_texture_cache_reset_state(GXTextureCache *cache) {
    if (!cache) return;
    memset(cache->unit, 0, sizeof(cache->unit));
    memset(cache->sampler_valid, 0, sizeof(cache->sampler_valid));
    cache->tlut_source = 0;
    if (cache->tmem) memset(cache->tmem, 0, GX_TEXTURE_TMEM_SIZE);
    gx_texture_cache_invalidate_all(cache);
}

void gx_texture_cache_invalidate_range(GXTextureCache *cache, uint32_t ea, uint32_t size) {
    if (!cache || size == 0) return;
    for (uint32_t i = 0; i < GX_TEXTURE_CACHE_CAP; ++i) {
        GXTextureCacheEntry *entry = &cache->entry[i];
        if (entry->source_size && ranges_overlap(entry->source_ea, entry->source_size, ea, size))
            entry->stale = true;
    }
}

void gx_texture_cache_apply_bp(GXTextureCache *cache, uint8_t reg, uint32_t value) {
    if (!cache) return;
    value &= 0xFFFFFFu;

    if (reg == 0x64) {
        cache->tlut_source = value << 5;
        return;
    }
    if (reg == 0x65) {
        const uint32_t offset = (value & 0x3FFu) << 9;
        const uint32_t size = ((value >> 10) & 0x7FFu) << 5;
        if (size == 0 || offset > GX_TEXTURE_TMEM_SIZE - size) return;
        const uint8_t *source = cache->read_guest ? cache->read_guest(cache->user,
                                                                        cache->tlut_source, size)
                                                 : NULL;
        if (source) memcpy(cache->tmem + offset, source, size);
        else memset(cache->tmem + offset, 0, size);
        invalidate_tlut_range(cache, offset, size);
        return;
    }
    if (reg == 0x66) {
        gx_texture_cache_invalidate_all(cache);
        return;
    }
    if (reg < 0x80u || reg > 0xBFu) return;

    const uint8_t address = reg & 0x3Fu;
    const uint8_t unit = (address & 3u) | ((address >> 3) & 4u);
    const uint8_t field = (address >> 2) & 7u;
    if (unit >= GX_TEXTURE_MAX_UNITS) return;
    GXTextureUnit *state = &cache->unit[unit];
    switch (field) {
    case 0: state->mode0 = value; cache->sampler_valid[unit] = false; break;
    case 1: state->mode1 = value; cache->sampler_valid[unit] = false; break;
    case 2: state->image0 = value; break;
    case 3: state->image1 = value; break;
    case 4: state->image2 = value; break;
    case 5: state->image3 = value; break;
    case 6: state->tlut = value; break;
    default: break;
    }
}

GX2Texture *gx_texture_cache_get_texture_unit(GXTextureCache *cache, const GXTextureUnit *unit) {
    if (!cache || !cache->read_guest || !unit) return NULL;
    GXTextureDesc desc;
    if (!texture_desc_from_unit(unit, &desc)) return NULL;
    const uint8_t *source = cache->read_guest(cache->user, desc.source_ea, desc.source_size);
    if (!source || (desc.tlut_size && !cache->tmem)) return NULL;
    const uint8_t *tlut = desc.tlut_size ? cache->tmem + desc.tlut_offset : NULL;
    const uint64_t source_hash = hash_bytes(source, desc.source_size);
    const uint64_t tlut_hash = desc.tlut_size ? hash_bytes(tlut, desc.tlut_size) : 0;

    GXTextureCacheEntry *entry = find_entry(cache, unit, &desc);
    if (!entry->valid || entry->stale || entry->source_hash != source_hash ||
        entry->tlut_hash != tlut_hash) {
        if (!upload_entry(entry, unit, &desc, source, tlut)) {
            entry->valid = false;
            return NULL;
        }
        entry->source_hash = source_hash;
        entry->tlut_hash = tlut_hash;
        entry->valid = true;
        entry->stale = false;
    }
    entry->last_used = ++cache->clock;
    return &entry->texture;
}

GX2Texture *gx_texture_cache_get_texture(GXTextureCache *cache, uint8_t texmap) {
    if (!cache || texmap >= GX_TEXTURE_MAX_UNITS) return NULL;
    return gx_texture_cache_get_texture_unit(cache, &cache->unit[texmap]);
}

GX2Sampler *gx_texture_cache_get_sampler(GXTextureCache *cache, uint8_t texmap) {
    if (!cache || texmap >= GX_TEXTURE_MAX_UNITS) return NULL;
    const GXTextureUnit *unit = &cache->unit[texmap];
    if (!cache->sampler_valid[texmap] || cache->sampler_mode0[texmap] != unit->mode0 ||
        cache->sampler_mode1[texmap] != unit->mode1) {
        gx_texture_sampler_from_unit(&cache->sampler[texmap], unit);
        cache->sampler_mode0[texmap] = unit->mode0;
        cache->sampler_mode1[texmap] = unit->mode1;
        cache->sampler_valid[texmap] = true;
    }
    return &cache->sampler[texmap];
}

// Self test
static int px_eq(const uint8_t *p, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return p[0] == r && p[1] == g && p[2] == b && p[3] == a;
}

typedef struct {
    uint8_t bytes[256];
} GXTextureTestMemory;

static const uint8_t *texture_test_read(void *user, uint32_t ea, uint32_t size) {
    GXTextureTestMemory *memory = user;
    if (ea > sizeof(memory->bytes) || size > sizeof(memory->bytes) - ea) return NULL;
    return memory->bytes + ea;
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

    {
        GXTextureTestMemory memory = {0};
        GXTextureCache cache;
        if (!gx_texture_cache_init(&cache, texture_test_read, &memory)) return 0;

        memory.bytes[0x40] = 0x12;
        memory.bytes[0x41] = 0x34;
        gx_texture_cache_apply_bp(&cache, 0x64, 0x04);  // source 0x80
        gx_texture_cache_apply_bp(&cache, 0x65, 0x01 | (1u << 10));
        if (cache.tmem[0x200] != 0x00 || cache.tmem[0x201] != 0x00) {
            gx_texture_cache_destroy(&cache);
            return 0;
        }
        memory.bytes[0x80] = 0xAB;
        memory.bytes[0x81] = 0xCD;
        gx_texture_cache_apply_bp(&cache, 0x65, 0x01 | (1u << 10));
        if (cache.tmem[0x200] != 0xAB || cache.tmem[0x201] != 0xCD) {
            gx_texture_cache_destroy(&cache);
            return 0;
        }

        // Unit 4 lives at BP 0xA0, proving the split unit-address encoding.
        gx_texture_cache_apply_bp(&cache, 0xA0, (2u << 5));
        gx_texture_cache_apply_bp(&cache, 0xE0, 0xFFFFFFu);
        if (cache.unit[4].mode0 != (2u << 5)) {
            gx_texture_cache_destroy(&cache);
            return 0;
        }
        gx_texture_cache_apply_bp(&cache, 0xA4, (32u << 8));
        gx_texture_cache_apply_bp(&cache, 0xA8, 7u | (7u << 10) | ((uint32_t)GX_TF_C4 << 20));
        gx_texture_cache_apply_bp(&cache, 0xAC, 0);
        gx_texture_cache_apply_bp(&cache, 0xB0, 0);
        gx_texture_cache_apply_bp(&cache, 0xB4, 2u);  // guest source 0x40
        gx_texture_cache_apply_bp(&cache, 0xB8, 2u | ((uint32_t)GX_TLUT_RGB565 << 10));
        GXTextureDesc desc;
        if (!texture_desc_from_unit(&cache.unit[4], &desc) || desc.width != 8 ||
            desc.height != 8 || desc.levels != 3 || desc.source_ea != 0x40 ||
            desc.source_size != 96 || desc.tlut_offset != 0x400 || desc.tlut_size != 32) {
            gx_texture_cache_destroy(&cache);
            return 0;
        }

        GXTextureCacheEntry *entry = &cache.entry[0];
        entry->source_ea = 0x40;
        entry->source_size = 96;
        entry->valid = true;
        gx_texture_cache_invalidate_range(&cache, 0x80000060u, 4);
        if (!entry->stale) {
            gx_texture_cache_destroy(&cache);
            return 0;
        }
        entry->stale = false;
        gx_texture_cache_apply_bp(&cache, 0x66, 0);
        if (!entry->stale) {
            gx_texture_cache_destroy(&cache);
            return 0;
        }
        gx_texture_cache_destroy(&cache);
    }

    // EFB-copy encode
    {
        uint8_t linear[8 * 8 * 4];
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                uint8_t *px = linear + (y * 8 + x) * 4;
                px[0] = (uint8_t)(x * 32 + 7);
                px[1] = (uint8_t)(y * 32 + 3);
                px[2] = (uint8_t)((x ^ y) * 16);
                px[3] = (uint8_t)(x < 4 ? 0xFF : 0x40);
            }

        uint8_t encoded[8 * 8 * 4];
        uint8_t decoded[8 * 8 * 4];

        // RGBA8
        if (gx_texture_encode_copy(encoded, linear, 8, 8, 8 * 4, GX_COPY_RGBA8, false) !=
            gx_texture_encoded_size(8, 8, GX_TF_RGBA8))
            return 0;
        gx_texture_decode(decoded, encoded, 8, 8, GX_TF_RGBA8, NULL, 0);
        if (memcmp(decoded, linear, sizeof(linear)) != 0) return 0;

        // RGB565
        gx_texture_encode_copy(encoded, linear, 8, 8, 8 * 4, GX_COPY_RGB565, false);
        gx_texture_decode(decoded, encoded, 8, 8, GX_TF_RGB565, NULL, 0);
        for (int i = 0; i < 8 * 8; ++i) {
            if ((decoded[i * 4 + 0] >> 3) != (linear[i * 4 + 0] >> 3)) return 0;
            if ((decoded[i * 4 + 1] >> 2) != (linear[i * 4 + 1] >> 2)) return 0;
            if (decoded[i * 4 + 3] != 0xFF) return 0;
        }

        // I8
        gx_texture_encode_copy(encoded, linear, 8, 8, 8 * 4, GX_COPY_R8, false);
        gx_texture_decode(decoded, encoded, 8, 8, GX_TF_I8, NULL, 0);
        if (decoded[0] != linear[0]) return 0;
        gx_texture_encode_copy(encoded, linear, 8, 8, 8 * 4, GX_COPY_R8, true);
        gx_texture_decode(decoded, encoded, 8, 8, GX_TF_I8, NULL, 0);
        if (decoded[0] != rgb_to_i(linear[0], linear[1], linear[2])) return 0;

        // IA8
        gx_texture_encode_copy(encoded, linear, 8, 8, 8 * 4, GX_COPY_RA8, false);
        gx_texture_decode(decoded, encoded, 8, 8, GX_TF_IA8, NULL, 0);
        if (decoded[3] != linear[3] || decoded[0] != linear[0]) return 0;
    }

    return 1;
}
