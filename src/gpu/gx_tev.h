// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

// GX TEV combiner register decode

#ifndef RISTRETTO_GPU_GX_TEV_H
#define RISTRETTO_GPU_GX_TEV_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GX_TEV_MAX_STAGES 16
#define GX_TEV_PS_CFILE_COUNT 10

// Colour combiner input select
enum {
    GX_CC_CPREV = 0, GX_CC_APREV = 1, GX_CC_C0 = 2,  GX_CC_A0 = 3,
    GX_CC_C1 = 4,    GX_CC_A1 = 5,    GX_CC_C2 = 6,   GX_CC_A2 = 7,
    GX_CC_TEXC = 8,  GX_CC_TEXA = 9,  GX_CC_RASC = 10, GX_CC_RASA = 11,
    GX_CC_ONE = 12,  GX_CC_HALF = 13, GX_CC_KONST = 14, GX_CC_ZERO = 15,
};

// Alpha combiner input select
enum {
    GX_CA_APREV = 0, GX_CA_A0 = 1, GX_CA_A1 = 2,   GX_CA_A2 = 3,
    GX_CA_TEXA = 4,  GX_CA_RASA = 5, GX_CA_KONST = 6, GX_CA_ZERO = 7,
};

// TevBias / TevOp / TevScale / TevOutput
enum { GX_TB_ZERO = 0, GX_TB_ADDHALF = 1, GX_TB_SUBHALF = 2, GX_TB_COMPARE = 3 };
enum { GX_TEV_ADD = 0, GX_TEV_SUB = 1 };
enum { GX_TS_1 = 0, GX_TS_2 = 1, GX_TS_4 = 2, GX_TS_HALF = 3 };
enum { GX_TEVOUT_PREV = 0, GX_TEVOUT_C0 = 1, GX_TEVOUT_C1 = 2, GX_TEVOUT_C2 = 3 };

// Rasterised colour channel
enum { GX_RAS_COLOR0 = 0, GX_RAS_COLOR1 = 1, GX_RAS_ZERO = 7 };

// One combiner half
typedef struct {
    uint8_t a, b, c, d;  // input selects
    uint8_t bias;        // GX_TB_*
    uint8_t op;          // GX_TEV_ADD/SUB
    bool    clamp;
    uint8_t scale;       // GX_TS_*
    uint8_t dest;        // GX_TEVOUT_*
} TevCombiner;

// One fully-decoded TEV stage.
typedef struct {
    TevCombiner color;
    TevCombiner alpha;
    uint8_t rswap;      // ras swap-table select (0..3)
    uint8_t tswap;      // tex swap-table select (0..3)
    uint8_t texmap;     // texture unit (0..7)
    uint8_t texcoord;   // texcoord input (0..7)
    bool    tex_enable; // sample the texture
    uint8_t colorchan;  // GX_RAS_* rasterised colour channel
    uint8_t kcsel;      // KonstSel for colour konst (0..31)
    uint8_t kasel;      // KonstSel for alpha konst (0..31)
} TevStage;

// Pixel operations which are performed after the last TEV stage.
typedef struct {
    bool    alpha_test_enable;
    uint8_t alpha_comp0;
    uint8_t alpha_comp1;
    uint8_t alpha_op;
    uint8_t alpha_ref0;
    uint8_t alpha_ref1;
    bool    rgba6;
    bool    dst_alpha_enable;
    uint8_t dst_alpha;
} TevPixelState;

// The accumulated TEV pipeline.
typedef struct {
    uint8_t  num_stages;  // 1..16 (GENMODE numtevstages + 1)
    TevStage stage[GX_TEV_MAX_STAGES];
    uint8_t  swap[4][4];
    // TEV colour registers (index 0 = PREV, 1..3 = C0..C2) and konst colours
    // (K0..K3), each RGBA normalised to 0..1.
    float    color[4][4];
    float    konst[4][4];
    TevPixelState pixel;
} TevConfig;

// Reset to a defined identity.
void gx_tev_reset(TevConfig *cfg);

// Fold one BP register load into the config.
void gx_tev_apply_bp(TevConfig *cfg, uint8_t reg, uint32_t value);

// Lay the colour/konst registers into the pixel-shader uniform cfile the
// generated multi-stage TEV program expects.
void gx_tev_build_ps_cfile(const TevConfig *cfg, float out[GX_TEV_PS_CFILE_COUNT][4]);

int gx_tev_selftest(void);

#ifdef __cplusplus
}
#endif

#endif  // RISTRETTO_GPU_GX_TEV_H
