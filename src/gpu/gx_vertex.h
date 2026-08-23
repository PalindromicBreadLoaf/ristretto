// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

// GX vertex loader

#ifndef RISTRETTO_GPU_GX_VERTEX_H
#define RISTRETTO_GPU_GX_VERTEX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gpu/gx_fifo.h"

#ifdef __cplusplus
extern "C" {
#endif

// GX vertex array ids
enum {
    GX_ARRAY_POSITION  = 0,
    GX_ARRAY_NORMAL    = 1,
    GX_ARRAY_COLOR0    = 2,
    GX_ARRAY_COLOR1    = 3,
    GX_ARRAY_TEXCOORD0 = 4,   // 4..11
    GX_ARRAY_COUNT     = 16,
};

// CP array base EAs + strides for indexed vertex components.
typedef struct {
    uint32_t base[GX_ARRAY_COUNT];
    uint32_t stride[GX_ARRAY_COUNT];
} GXArrayTable;

// Fetch `len` contiguous guest bytes at EA `ea`.
typedef const uint8_t *(*GXGuestRead)(void *user, uint32_t ea, uint32_t len);

typedef struct {
    bool     pos_3d;            // 3 vs 2 position elements
    bool     has_color0;
    bool     has_color1;
    bool     has_normal;
    bool     has_pos_mtx_idx;   // per-vertex position/normal matrix index present
    bool     has_tex_mtx_idx[8];
    uint32_t num_texcoords;     // count of present texcoord slots
    bool     tex_present[8];    // which texcoord slots are present
} GXVertexLayout;

// Derive the output layout from the vertex descriptor.
void gx_vertex_layout(const GXFifoState *st, uint8_t vat, GXVertexLayout *out);

// Decode `num_vertices` starting at `src` (the primitive payload) into the
// caller's float arrays.
size_t gx_vertex_load(const GXFifoState *st, uint8_t vat, const GXArrayTable *arr,
                      GXGuestRead read, void *user, const uint8_t *src, size_t avail,
                      uint32_t num_vertices, float *pos_out, float *col0_out,
                      float *col1_out, float *normal_out, float *tex_out[8],
                      uint8_t *pmi_out, uint8_t *tex_mtx_out[8]);

int gx_vertex_selftest(void);

#ifdef __cplusplus
}
#endif

#endif  // RISTRETTO_GPU_GX_VERTEX_H
