// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

// GX fixed-function render state to GX2 pipeline state translation.

#ifndef RISTRETTO_GPU_GX_STATE_H
#define RISTRETTO_GPU_GX_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include <gx2/enum.h>

#ifdef __cplusplus
extern "C" {
#endif

// BP register addresses this translator consumes.
enum {
    GX_BP_GENMODE       = 0x00,
    GX_BP_SCISSORTL     = 0x20,
    GX_BP_SCISSORBR     = 0x21,
    GX_BP_ZMODE         = 0x40,
    GX_BP_BLENDMODE     = 0x41,
    GX_BP_CONSTANTALPHA = 0x42,
    GX_BP_ZCOMPARE      = 0x43,
    GX_BP_EFB_TL        = 0x49,
    GX_BP_EFB_WH        = 0x4A,
    GX_BP_CLEAR_AR      = 0x4F,
    GX_BP_CLEAR_GB      = 0x50,
    GX_BP_CLEAR_Z       = 0x51,
    GX_BP_TRIGGER_EFB_COPY = 0x52,
    GX_BP_SCISSOROFFSET = 0x59,
    GX_BP_ALPHACOMPARE  = 0xF3,
};

// Latest raw values of the render-state BP registers.
typedef struct {
    uint32_t genmode;        // 0x00
    uint32_t zmode;          // 0x40
    uint32_t blendmode;      // 0x41
    uint32_t constant_alpha; // 0x42
    uint32_t zcompare;       // 0x43
    uint32_t scissor_tl;     // 0x20
    uint32_t scissor_br;     // 0x21
    uint32_t scissor_offset; // 0x59
    uint32_t efb_tl;         // 0x49
    uint32_t efb_wh;         // 0x4A
    uint32_t clear_ar;       // 0x4F
    uint32_t clear_gb;       // 0x50
    uint32_t clear_z;        // 0x51
    uint32_t alpha_compare;  // 0xF3
    bool     clear_pending;
} GXRenderState;

// GX2 depth test pipeline state.
typedef struct {
    bool test_enable;
    bool write_enable;
    GX2CompareFunction func;
} GXDepthState;

// GX2 colour-control + blend-control pipeline state.
typedef struct {
    bool blend_enable;
    bool logic_op_enable;
    bool color_update;
    bool alpha_update;
    bool dual_source_alpha;
    GX2BlendMode src_color;
    GX2BlendMode dst_color;
    GX2BlendMode src_alpha;
    GX2BlendMode dst_alpha;
    GX2BlendCombineMode color_combine;
    GX2BlendCombineMode alpha_combine;
    GX2LogicOp logic_op;
} GXBlendState;

// GX2 rasteriser cull state.
typedef struct {
    bool cull_front;
    bool cull_back;
    GX2FrontFace front_face;
} GXCullState;

// GX2 emulates a fixed-function alpha test via pixel shaders.
typedef struct {
    bool enable;
    uint8_t ref0;
    uint8_t ref1;
    GX2CompareFunction comp0;
    GX2CompareFunction comp1;
    uint8_t op;
} GXAlphaTestState;

typedef struct {
    float x;
    float y;
    float width;
    float height;
    float near_z;
    float far_z;
} GXViewportState;

typedef struct {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} GXScissorState;

typedef struct {
    GXScissorState rect;
    float          color[4];
    float          depth;
    bool           color_enable;
    bool           alpha_enable;
    bool           depth_enable;
} GXClearState;

// Reset all tracked registers to zero.
void gx_state_reset(GXRenderState *state);

// Fold one BP register load into the tracked state.
void gx_state_apply_bp(GXRenderState *state, uint8_t reg, uint32_t value);

// Translate the tracked registers into GX2 pipeline state.
void gx_state_depth(const GXRenderState *state, GXDepthState *out);
void gx_state_blend(const GXRenderState *state, GXBlendState *out);
void gx_state_cull(const GXRenderState *state, GXCullState *out);
void gx_state_alpha_test(const GXRenderState *state, GXAlphaTestState *out);

// Convert the guest XF viewport and BP scissor fields to native EFB coordinates.
void gx_state_viewport(const GXRenderState *state, const float xf_viewport[6],
                       GXViewportState *out);
void gx_state_scissor(const GXRenderState *state, GXScissorState *out);

// Consume a clear requested by an EFB-copy BP command.
bool gx_state_take_clear(GXRenderState *state, GXClearState *out);

int gx_state_selftest(void);

#ifdef __cplusplus
}
#endif

#endif  // RISTRETTO_GPU_GX_STATE_H
