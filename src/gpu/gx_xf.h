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
    XF_LIGHTS          = 0x600,  // 0x600..0x67F: 8 light objects, 16 words each
    XF_LIGHTS_END      = 0x680,
    XF_REGISTERS_START = 0x1000,
    XF_SETNUMCHAN      = 0x1009,
    XF_SETCHAN0_AMBCOLOR = 0x100A,  // 0x100A..0x100D ambient/material colours
    XF_SETCHAN1_AMBCOLOR = 0x100B,
    XF_SETCHAN0_MATCOLOR = 0x100C,
    XF_SETCHAN1_MATCOLOR = 0x100D,
    XF_SETCHAN0_COLOR  = 0x100E,  // 0x100E..0x1011 channel control
    XF_SETCHAN1_COLOR  = 0x100F,
    XF_SETCHAN0_ALPHA  = 0x1010,
    XF_SETCHAN1_ALPHA  = 0x1011,
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

// Channel control colour/alpha material + ambient source
enum { GX_XF_MATSRC_REG = 0, GX_XF_MATSRC_VTX = 1 };
enum { GX_XF_AMBSRC_REG = 0, GX_XF_AMBSRC_VTX = 1 };

// Light diffuse attenuation function
enum { GX_XF_DF_NONE = 0, GX_XF_DF_SIGN = 1, GX_XF_DF_CLAMP = 2 };

// Light attenuation function
enum { GX_XF_AF_NONE = 0, GX_XF_AF_SPEC = 1, GX_XF_AF_DIR = 2, GX_XF_AF_SPOT = 3 };

#define GX_XF_NUM_LIGHTS 8

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

// Decoded per-channel lighting control
typedef struct {
    uint8_t matsource;      // GX_XF_MATSRC_*
    uint8_t enablelighting;
    uint8_t ambsource;      // GX_XF_AMBSRC_*
    uint8_t diffusefunc;    // GX_XF_DF_*
    uint8_t attnfunc;       // GX_XF_AF_*
    uint8_t light_mask;     // 8-bit mask of enabled lights
} XfLitChannel;

// A decoded XF light object
typedef struct {
    uint32_t color;      // RGBA8, as written
    float    cosatt[3];  // cos/angle attenuation a0, a1, a2
    float    distatt[3]; // distance attenuation k0, k1, k2
    float    pos[3];     // light position (dpos) / spot direction (sdir)
    float    dir[3];     // light direction (ddir) / half-angle (shalfangle)
} XfLight;

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
    uint32_t amb_color[2];          // XF_SETCHAN{0,1}_AMBCOLOR (RGBA8)
    uint32_t mat_color[2];          // XF_SETCHAN{0,1}_MATCOLOR (RGBA8)
    XfLitChannel color_chan[2];     // XF_SETCHAN{0,1}_COLOR
    XfLitChannel alpha_chan[2];     // XF_SETCHAN{0,1}_ALPHA
    XfLight  lights[GX_XF_NUM_LIGHTS];
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

// Build the 3 cfile rows (12 floats) a regular matrix texgen consumes in the VS.
void gx_xf_build_texmtx_cfile(const XfConfig *cfg, uint32_t texmtx_index, bool stq,
                              float out[12]);

// True when texgen slot `tc` is a regular matrix texgen sourced from its own
// texcoord input.
bool gx_xf_texgen_is_regular(const XfConfig *cfg, uint32_t tc);

typedef struct {
    bool     enable;          // channel has lighting enabled
    uint8_t  num_lights;      // enabled lights
    bool     diffuse;         // diffusefunc != None
    bool     clamp;           // diffusefunc == Clamp
    bool     mat_from_vertex; // material colour from vertex vs register
    bool     amb_from_vertex; // ambient colour from vertex vs register
} XfLightDesc;

// Reduce a colour channel's lit-channel state to the VS descriptor.
bool gx_xf_lighting_desc(const XfConfig *cfg, uint32_t chan, XfLightDesc *out);

// Build the lighting VS uniform block for colour channel `chan`, using position
// matrix `pos_mtx_index` for the normal transform.
int gx_xf_build_light_cfile(const XfConfig *cfg, uint32_t chan, uint32_t pos_mtx_index,
                            float *out, int cap);

int gx_xf_selftest(void);

#ifdef __cplusplus
}
#endif

#endif  // RISTRETTO_GPU_GX_XF_H
