// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

// GX texture format decode to a GX2 uploadable linear RGBA8 surface.

#ifndef RISTRETTO_GPU_GX_TEXTURE_H
#define RISTRETTO_GPU_GX_TEXTURE_H

#include <stdint.h>

#include <gx2/enum.h>

#ifdef __cplusplus
extern "C" {
#endif

// GX texture formats
typedef enum {
    GX_TF_I4     = 0x0,
    GX_TF_I8     = 0x1,
    GX_TF_IA4    = 0x2,
    GX_TF_IA8    = 0x3,
    GX_TF_RGB565 = 0x4,
    GX_TF_RGB5A3 = 0x5,
    GX_TF_RGBA8  = 0x6,
    GX_TF_C4     = 0x8,
    GX_TF_C8     = 0x9,
    GX_TF_C14X2  = 0xA,
    GX_TF_CMPR   = 0xE,
} GXTextureFormat;

// GX texture lookup table formats for the indexed texture formats.
typedef enum {
    GX_TLUT_IA8    = 0x0,
    GX_TLUT_RGB565 = 0x1,
    GX_TLUT_RGB5A3 = 0x2,
} GXTlutFormat;

// Every format is decoded to 8-bit UNORM RGBA.
#define GX_TEXTURE_GX2_FORMAT GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8

// Tile dimensions of a format in texels.
int gx_texture_block_width(GXTextureFormat fmt);
int gx_texture_block_height(GXTextureFormat fmt);

// Size of the encoded source data for a width*height texture in bytes.
int gx_texture_encoded_size(int width, int height, GXTextureFormat fmt);

// Palette size in bytes for an indexed format.
int gx_texture_palette_size(GXTextureFormat fmt);

// Decode an encoded GX texture into a linear RGBA8 surface.
int gx_texture_decode(uint8_t *dst, const uint8_t *src, int width, int height,
                      GXTextureFormat fmt, const uint8_t *tlut, GXTlutFormat tlutfmt);

int gx_texture_selftest(void);

#ifdef __cplusplus
}
#endif

#endif  // RISTRETTO_GPU_GX_TEXTURE_H
