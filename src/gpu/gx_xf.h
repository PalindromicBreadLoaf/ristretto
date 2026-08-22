// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

// GX XF register decode.

#ifndef RISTRETTO_GPU_GX_XF_H
#define RISTRETTO_GPU_GX_XF_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// XF register addresses / ranges (Dolphin XFMemory.h)
enum {
    XF_POSMATRICES     = 0x000,  // 0x000..0x0FF: 64 position matrices
    XF_POSMATRICES_END = 0x100,
    XF_NORMALMATRICES  = 0x400,  // 0x400..0x45F: 32 normal matrices
    XF_NORMALMATRICES_END = 0x460,
    XF_POSTMATRICES    = 0x500,  // 0x500..0x5FF: post/tex matrices
    XF_POSTMATRICES_END = 0x600,
    XF_REGISTERS_START = 0x1000,
    XF_SETNUMCHAN      = 0x1009,
    XF_SETVIEWPORT     = 0x101A,  // 0x101A..0x101F
    XF_SETPROJECTION   = 0x1020,  // 0x1020..0x1025 raw
    XF_SETNUMTEXGENS   = 0x103F,
    XF_SETTEXMTXINFO   = 0x1040,  // 0x1040..0x1047
    XF_SETPOSTMTXINFO  = 0x1050,  // 0x1050..0x1057
    XF_REGISTERS_END   = 0x1058,
};

// Projection type
enum { GX_XF_PERSPECTIVE = 0, GX_XF_ORTHOGRAPHIC = 1 };

// Texcoord projection size
enum { GX_XF_TEX_ST = 0, GX_XF_TEX_STQ = 1 };

// Texgen type
enum {
    GX_XF_TG_REGULAR   = 0,
    GX_XF_TG_EMBOSSMAP = 1,
    GX_XF_TG_COLOR0    = 2,
    GX_XF_TG_COLOR1    = 3,
};

// Decoded per-texcoord transform info.
typedef struct {
    uint8_t projection;    // GX_XF_TEX_ST / STQ
    uint8_t inputform;     // 0 = AB11, 1 = ABC1
    uint8_t texgentype;    // GX_XF_TG_*
    uint8_t sourcerow;     // input row (geom/normal/colors/tex0..)
    uint8_t emboss_source; // emboss source shift
    uint8_t emboss_light;  // emboss light index
} XfTexMtxInfo;

#define GX_XF_MAX_TEXGENS 8

// The accumulated transform state.
typedef struct {
    float    pos_matrices[256];     // 3x4 row-major matrices, 4 words each
    float    normal_matrices[96];   // 3x3, 3 words per row
    float    post_matrices[256];
    float    viewport[6];           // wd, ht, zRange, xOrig, yOrig, farZ
    float    projection[6];         // raw projection coefficients
    uint32_t projection_type;       // GX_XF_PERSPECTIVE / ORTHOGRAPHIC
    uint32_t num_color_chans;       // XF_SETNUMCHAN
    uint32_t num_texgens;           // XF_SETNUMTEXGENS
    XfTexMtxInfo texmtx[GX_XF_MAX_TEXGENS];
} XfConfig;

void gx_xf_reset(XfConfig *cfg);

// Fold one XF register load into the config.
void gx_xf_apply_xf(XfConfig *cfg, uint16_t address, uint8_t count, const uint8_t *data);

// Build the 4x4 clip-space projection matrix from the raw coefficients.
void gx_xf_projection_matrix(const XfConfig *cfg, float out[16]);

// Expand the selected 3x4 position matrix to a 4x4.
void gx_xf_position_matrix(const XfConfig *cfg, uint32_t mtx_index, float out[16]);

// Build the transform VS's uniform matrix.
void gx_xf_build_vs_cfile(const XfConfig *cfg, uint32_t mtx_index, float out[16]);

int gx_xf_selftest(void);

#ifdef __cplusplus
}
#endif

#endif  // RISTRETTO_GPU_GX_XF_H
