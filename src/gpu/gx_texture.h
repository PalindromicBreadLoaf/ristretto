// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

// GX texture format decode to a GX2 uploadable linear RGBA8 surface.

#ifndef RISTRETTO_GPU_GX_TEXTURE_H
#define RISTRETTO_GPU_GX_TEXTURE_H

#include <stdbool.h>
#include <stdint.h>

#include <gx2/enum.h>
#include <gx2/sampler.h>
#include <gx2/texture.h>

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

// EFB-copy destination formats
typedef enum {
    GX_COPY_R4     = 0x0,   // I4 tiling, red or intensity
    GX_COPY_R8_1   = 0x1,   // I8 tiling, red or intensity
    GX_COPY_RA4    = 0x2,   // IA4 tiling
    GX_COPY_RA8    = 0x3,   // IA8 tiling
    GX_COPY_RGB565 = 0x4,
    GX_COPY_RGB5A3 = 0x5,
    GX_COPY_RGBA8  = 0x6,
    GX_COPY_A8     = 0x7,   // I8 tiling, alpha
    GX_COPY_R8     = 0x8,   // I8 tiling, red
    GX_COPY_G8     = 0x9,   // I8 tiling, green
    GX_COPY_B8     = 0xA,   // I8 tiling, blue
    GX_COPY_RG8    = 0xB,   // IA8 tiling, red+green
    GX_COPY_GB8    = 0xC,   // IA8 tiling, green+blue
} GXCopyFormat;

// Every format is decoded to 8-bit UNORM RGBA.
#define GX_TEXTURE_GX2_FORMAT GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8

#define GX_TEXTURE_MAX_UNITS 8
#define GX_TEXTURE_CACHE_CAP 32
#define GX_TEXTURE_TMEM_SIZE (1024u * 1024u)

// BP texture-unit state.
typedef struct {
    uint32_t mode0;
    uint32_t mode1;
    uint32_t image0;
    uint32_t image1;
    uint32_t image2;
    uint32_t image3;
    uint32_t tlut;
} GXTextureUnit;

typedef const uint8_t *(*GXTextureGuestRead)(void *user, uint32_t ea, uint32_t size);

typedef struct {
    GX2Texture texture;
    uint32_t   source_ea;
    uint32_t   source_size;
    uint32_t   tlut_offset;
    uint32_t   tlut_size;
    uint32_t   levels;
    uint32_t   image0;
    uint32_t   image1;
    uint32_t   image2;
    uint32_t   image3;
    uint32_t   tlut;
    uint64_t   source_hash;
    uint64_t   tlut_hash;
    uint32_t   last_used;
    bool       allocated;
    bool       valid;
    bool       stale;
} GXTextureCacheEntry;

// Main-memory texture cache and the emulated 1 MiB texture-memory backing used
// for TLUT loads.
typedef struct {
    GXTextureUnit       unit[GX_TEXTURE_MAX_UNITS];
    GXTextureCacheEntry entry[GX_TEXTURE_CACHE_CAP];
    GX2Sampler          sampler[GX_TEXTURE_MAX_UNITS];
    uint32_t            sampler_mode0[GX_TEXTURE_MAX_UNITS];
    uint32_t            sampler_mode1[GX_TEXTURE_MAX_UNITS];
    uint8_t            *tmem;
    uint32_t            tlut_source;
    uint32_t            clock;
    GXTextureGuestRead  read_guest;
    void               *user;
    bool                sampler_valid[GX_TEXTURE_MAX_UNITS];
} GXTextureCache;

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

// The texture format whose tiled layout an EFB-copy format writes into.
GXTextureFormat gx_texture_copy_layout(GXCopyFormat fmt);

// Encode a linear RGBA8 image region into an EFB-copy format.
int gx_texture_encode_copy(uint8_t *dst, const uint8_t *src, int width, int height,
                           uint32_t src_pitch, GXCopyFormat fmt, bool intensity);

bool gx_texture_cache_init(GXTextureCache *cache, GXTextureGuestRead read_guest, void *user);
void gx_texture_cache_destroy(GXTextureCache *cache);
void gx_texture_cache_destroy_after_gpu_idle(GXTextureCache *cache);
void gx_texture_cache_reset_state(GXTextureCache *cache);

// Fold a BP texture/TLUT command.
void gx_texture_cache_apply_bp(GXTextureCache *cache, uint8_t reg, uint32_t value);

void gx_texture_cache_invalidate_range(GXTextureCache *cache, uint32_t ea, uint32_t size);
void gx_texture_cache_invalidate_all(GXTextureCache *cache);

GX2Texture *gx_texture_cache_get_texture(GXTextureCache *cache, uint8_t texmap);
GX2Sampler *gx_texture_cache_get_sampler(GXTextureCache *cache, uint8_t texmap);
GX2Texture *gx_texture_cache_get_texture_unit(GXTextureCache *cache,
                                               const GXTextureUnit *unit);
void gx_texture_sampler_from_unit(GX2Sampler *sampler, const GXTextureUnit *unit);

int gx_texture_selftest(void);

#ifdef __cplusplus
}
#endif

#endif  // RISTRETTO_GPU_GX_TEXTURE_H
