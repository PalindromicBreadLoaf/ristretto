// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu/gx_state.h"

#include <math.h>
#include <string.h>

#define GX_EFB_WIDTH  640u
#define GX_EFB_HEIGHT 528u
#define GX_EFB_DEPTH_MAX 16777215.0f

// GX SrcBlendFactor (Zero,One,DstClr,InvDstClr,SrcAlpha,InvSrcAlpha,DstAlpha,InvDstAlpha)
// to GX2BlendMode.
static const GX2BlendMode kSrcFactor[8] = {
    GX2_BLEND_MODE_ZERO,          GX2_BLEND_MODE_ONE,
    GX2_BLEND_MODE_DST_COLOR,     GX2_BLEND_MODE_INV_DST_COLOR,
    GX2_BLEND_MODE_SRC_ALPHA,     GX2_BLEND_MODE_INV_SRC_ALPHA,
    GX2_BLEND_MODE_DST_ALPHA,     GX2_BLEND_MODE_INV_DST_ALPHA,
};

// GX DstBlendFactor (Zero,One,SrcClr,InvSrcClr,SrcAlpha,InvSrcAlpha,DstAlpha,InvDstAlpha)
// to GX2BlendMode.
static const GX2BlendMode kDstFactor[8] = {
    GX2_BLEND_MODE_ZERO,          GX2_BLEND_MODE_ONE,
    GX2_BLEND_MODE_SRC_COLOR,     GX2_BLEND_MODE_INV_SRC_COLOR,
    GX2_BLEND_MODE_SRC_ALPHA,     GX2_BLEND_MODE_INV_SRC_ALPHA,
    GX2_BLEND_MODE_DST_ALPHA,     GX2_BLEND_MODE_INV_DST_ALPHA,
};

// GX LogicOp to GX2LogicOp.
static const GX2LogicOp kLogicOp[16] = {
    GX2_LOGIC_OP_CLEAR,    GX2_LOGIC_OP_AND,      GX2_LOGIC_OP_REV_AND,  GX2_LOGIC_OP_COPY,
    GX2_LOGIC_OP_INV_AND,  GX2_LOGIC_OP_NOP,      GX2_LOGIC_OP_XOR,      GX2_LOGIC_OP_OR,
    GX2_LOGIC_OP_NOR,      GX2_LOGIC_OP_EQUIV,    GX2_LOGIC_OP_INV,      GX2_LOGIC_OP_REV_OR,
    GX2_LOGIC_OP_INV_COPY, GX2_LOGIC_OP_INV_OR,   GX2_LOGIC_OP_NOT_AND,  GX2_LOGIC_OP_SET,
};

// GX CompareMode (0..7) is identical to GX2CompareFunction.
static GX2CompareFunction to_compare(uint32_t gx_compare) {
    return (GX2CompareFunction)(gx_compare & 7);
}

GXEfbFormat gx_state_efb_format(const GXRenderState *state) {
    return (GXEfbFormat)(state->zcompare & 7u);
}

bool gx_state_efb_has_color(const GXRenderState *state) {
    return gx_state_efb_format(state) != GX_EFB_Z24;
}

bool gx_state_efb_has_alpha(const GXRenderState *state) {
    return gx_state_efb_format(state) == GX_EFB_RGBA6_Z24;
}

void gx_state_reset(GXRenderState *state) {
    memset(state, 0, sizeof(*state));
    state->scissor_tl = 342u | (342u << 12);
    state->scissor_br = (342u + GX_EFB_WIDTH - 1u) |
                        ((342u + GX_EFB_HEIGHT - 1u) << 12);
    state->scissor_offset = 171u | (171u << 10);
    state->efb_wh = (GX_EFB_WIDTH - 1u) | ((GX_EFB_HEIGHT - 1u) << 10);
    state->alpha_compare = (7u << 16) | (7u << 19);
}

void gx_state_apply_bp(GXRenderState *state, uint8_t reg, uint32_t value) {
    switch (reg) {
    case GX_BP_GENMODE:       state->genmode = value; break;
    case GX_BP_SCISSORTL:     state->scissor_tl = value; break;
    case GX_BP_SCISSORBR:     state->scissor_br = value; break;
    case GX_BP_ZMODE:         state->zmode = value; break;
    case GX_BP_BLENDMODE:     state->blendmode = value; break;
    case GX_BP_CONSTANTALPHA: state->constant_alpha = value; break;
    case GX_BP_ZCOMPARE:      state->zcompare = value; break;
    case GX_BP_EFB_TL:        state->efb_tl = value; break;
    case GX_BP_EFB_WH:        state->efb_wh = value; break;
    case GX_BP_EFB_ADDR:      state->efb_addr = value; break;
    case GX_BP_COPY_STRIDE:   state->copy_stride = value; break;
    case GX_BP_COPY_Y_SCALE:  state->copy_y_scale = value; break;
    case GX_BP_CLEAR_AR:      state->clear_ar = value; break;
    case GX_BP_CLEAR_GB:      state->clear_gb = value; break;
    case GX_BP_CLEAR_Z:       state->clear_z = value; break;
    case GX_BP_TRIGGER_EFB_COPY:
        state->copy_exec = value;
        state->clear_pending = (value & (1u << 11)) != 0;
        state->copy_pending = true;
        break;
    case GX_BP_SCISSOROFFSET: state->scissor_offset = value; break;
    case GX_BP_ALPHACOMPARE:  state->alpha_compare = value; break;
    default: break;
    }
}

void gx_state_depth(const GXRenderState *state, GXDepthState *out) {
    const uint32_t z = state->zmode;
    out->test_enable = z & 1;
    out->func = to_compare((z >> 1) & 7);
    out->write_enable = (z >> 4) & 1;
}

void gx_state_blend(const GXRenderState *state, GXBlendState *out) {
    const uint32_t b = state->blendmode;
    const bool blend_enable = b & 1;
    const bool logic_enable = (b >> 1) & 1;
    const bool subtract = (b >> 11) & 1;
    const uint32_t dst_idx = (b >> 5) & 7;
    const uint32_t src_idx = (b >> 8) & 7;
    const uint32_t logic_idx = (b >> 12) & 0xF;

    memset(out, 0, sizeof(*out));
    out->color_update = ((b >> 3) & 1) && gx_state_efb_has_color(state);
    out->alpha_update = ((b >> 4) & 1) && gx_state_efb_has_alpha(state);
    out->dual_source_alpha = ((state->constant_alpha >> 8) & 1u) && out->alpha_update &&
                             (state->zcompare & 7u) == 1u;
    out->color_combine = GX2_BLEND_COMBINE_MODE_ADD;
    out->alpha_combine = GX2_BLEND_COMBINE_MODE_ADD;
    out->src_color = out->src_alpha = GX2_BLEND_MODE_ONE;
    out->dst_color = out->dst_alpha = GX2_BLEND_MODE_ZERO;

    if (blend_enable) {
        out->blend_enable = true;
        if (subtract) {
            // GX subtract computes dst - src with both factors forced to one (Dolphin RenderState.cpp).
            out->src_color = out->src_alpha = GX2_BLEND_MODE_ONE;
            out->dst_color = out->dst_alpha = GX2_BLEND_MODE_ONE;
            out->color_combine = out->alpha_combine = GX2_BLEND_COMBINE_MODE_REV_SUB;
        } else {
            out->src_color = kSrcFactor[src_idx];
            out->dst_color = kDstFactor[dst_idx];
            // The alpha equation reuses the colour factors but swaps any colour
            // term for the matching alpha term (Dolphin RenderState.cpp)
            uint32_t src_alpha_idx = (src_idx == 2 || src_idx == 3) ? src_idx + 4 : src_idx;
            uint32_t dst_alpha_idx = (dst_idx == 2 || dst_idx == 3) ? dst_idx + 2 : dst_idx;
            out->src_alpha = kSrcFactor[src_alpha_idx];
            out->dst_alpha = kDstFactor[dst_alpha_idx];
        }
    } else if (logic_enable) {
        out->logic_op_enable = true;
        out->logic_op = kLogicOp[logic_idx];
    }
    if (!gx_state_efb_has_alpha(state)) {
        if (out->src_color == GX2_BLEND_MODE_DST_ALPHA) out->src_color = GX2_BLEND_MODE_ONE;
        if (out->src_color == GX2_BLEND_MODE_INV_DST_ALPHA) out->src_color = GX2_BLEND_MODE_ZERO;
        if (out->dst_color == GX2_BLEND_MODE_DST_ALPHA) out->dst_color = GX2_BLEND_MODE_ONE;
        if (out->dst_color == GX2_BLEND_MODE_INV_DST_ALPHA) out->dst_color = GX2_BLEND_MODE_ZERO;
    }
    if (out->dual_source_alpha) {
        if (out->src_color == GX2_BLEND_MODE_SRC_ALPHA)
            out->src_color = GX2_BLEND_MODE_SRC1_ALPHA;
        else if (out->src_color == GX2_BLEND_MODE_INV_SRC_ALPHA)
            out->src_color = GX2_BLEND_MODE_INV_SRC1_ALPHA;
        if (out->dst_color == GX2_BLEND_MODE_SRC_ALPHA)
            out->dst_color = GX2_BLEND_MODE_SRC1_ALPHA;
        else if (out->dst_color == GX2_BLEND_MODE_INV_SRC_ALPHA)
            out->dst_color = GX2_BLEND_MODE_INV_SRC1_ALPHA;
        if (out->src_alpha == GX2_BLEND_MODE_SRC_ALPHA)
            out->src_alpha = GX2_BLEND_MODE_SRC1_ALPHA;
        else if (out->src_alpha == GX2_BLEND_MODE_INV_SRC_ALPHA)
            out->src_alpha = GX2_BLEND_MODE_INV_SRC1_ALPHA;
        if (out->dst_alpha == GX2_BLEND_MODE_SRC_ALPHA)
            out->dst_alpha = GX2_BLEND_MODE_SRC1_ALPHA;
        else if (out->dst_alpha == GX2_BLEND_MODE_INV_SRC_ALPHA)
            out->dst_alpha = GX2_BLEND_MODE_INV_SRC1_ALPHA;
    }
}

void gx_state_cull(const GXRenderState *state, GXCullState *out) {
    const uint32_t mode = (state->genmode >> 14) & 3;  // 0 None 1 Back 2 Front 3 All
    out->cull_back = (mode == 1 || mode == 3);
    out->cull_front = (mode == 2 || mode == 3);
    // Wii front-facing winding is clockwise (Dolphin Vulkan backend).
    out->front_face = GX2_FRONT_FACE_CW;
}

void gx_state_alpha_test(const GXRenderState *state, GXAlphaTestState *out) {
    const uint32_t a = state->alpha_compare;
    out->ref0 = a & 0xFF;
    out->ref1 = (a >> 8) & 0xFF;
    out->comp0 = to_compare((a >> 16) & 7);
    out->comp1 = to_compare((a >> 19) & 7);
    out->op = (a >> 22) & 3;
    out->enable = !(out->comp0 == GX2_COMPARE_FUNC_ALWAYS &&
                    out->comp1 == GX2_COMPARE_FUNC_ALWAYS);
}

static uint32_t clamp_u32(int32_t value, uint32_t limit) {
    if (value <= 0) return 0;
    if ((uint32_t)value >= limit) return limit;
    return (uint32_t)value;
}

void gx_state_scissor(const GXRenderState *state, GXScissorState *out) {
    const int32_t xoff = (int32_t)((state->scissor_offset & 0x1FFu) << 1);
    const int32_t yoff = (int32_t)(((state->scissor_offset >> 10) & 0x1FFu) << 1);
    const int32_t left = (int32_t)(state->scissor_tl & 0x7FFu) - xoff;
    const int32_t top = (int32_t)((state->scissor_tl >> 12) & 0x7FFu) - yoff;
    const int32_t right = (int32_t)(state->scissor_br & 0x7FFu) - xoff;
    const int32_t bottom = (int32_t)((state->scissor_br >> 12) & 0x7FFu) - yoff;

    const uint32_t x0 = clamp_u32(left, GX_EFB_WIDTH);
    const uint32_t y0 = clamp_u32(top, GX_EFB_HEIGHT);
    const uint32_t x1 = clamp_u32(right + 1, GX_EFB_WIDTH);
    const uint32_t y1 = clamp_u32(bottom + 1, GX_EFB_HEIGHT);
    out->x = x0;
    out->y = y0;
    out->width = x1 > x0 ? x1 - x0 : 0;
    out->height = y1 > y0 ? y1 - y0 : 0;
}

static float clamp_finite(float value, float fallback) {
    return isfinite(value) ? value : fallback;
}

static float clamp_float(float value, float lo, float hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

void gx_state_viewport(const GXRenderState *state, const float xf_viewport[6],
                       GXViewportState *out) {
    const float wd = clamp_finite(xf_viewport[0], 320.0f);
    const float ht = clamp_finite(xf_viewport[1], -264.0f);
    const float z_range = clamp_finite(xf_viewport[2], GX_EFB_DEPTH_MAX);
    const float x_orig = clamp_finite(xf_viewport[3], 320.0f);
    const float y_orig = clamp_finite(xf_viewport[4], 264.0f);
    const float far_z = clamp_finite(xf_viewport[5], GX_EFB_DEPTH_MAX);
    const float xoff = (float)((state->scissor_offset & 0x1FFu) << 1);
    const float yoff = (float)(((state->scissor_offset >> 10) & 0x1FFu) << 1);

    out->x = x_orig - xoff - wd;
    out->y = y_orig - yoff + ht;
    out->width = 2.0f * wd;
    out->height = -2.0f * ht;
    if (out->width < 0.0f) {
        out->x += out->width;
        out->width = -out->width;
    }
    if (out->height < 0.0f) {
        out->y += out->height;
        out->height = -out->height;
    }
    if (!isfinite(out->x) || !isfinite(out->y) || !isfinite(out->width) ||
        !isfinite(out->height)) {
        out->x = 0.0f;
        out->y = 0.0f;
        out->width = GX_EFB_WIDTH;
        out->height = GX_EFB_HEIGHT;
    } else {
        out->x = clamp_float(out->x, -(float)GX_EFB_WIDTH, 2.0f * GX_EFB_WIDTH);
        out->y = clamp_float(out->y, -(float)GX_EFB_HEIGHT, 2.0f * GX_EFB_HEIGHT);
        out->width = clamp_float(out->width, 0.0f, 3.0f * GX_EFB_WIDTH);
        out->height = clamp_float(out->height, 0.0f, 3.0f * GX_EFB_HEIGHT);
    }
    out->near_z = (far_z - z_range) / GX_EFB_DEPTH_MAX;
    out->far_z = far_z / GX_EFB_DEPTH_MAX;
    if (!isfinite(out->near_z) || !isfinite(out->far_z)) {
        out->near_z = 0.0f;
        out->far_z = 1.0f;
    } else {
        out->near_z = clamp_float(out->near_z, 0.0f, 1.0f);
        out->far_z = clamp_float(out->far_z, 0.0f, 1.0f);
    }
}

bool gx_state_take_clear(GXRenderState *state, GXClearState *out) {
    if (!state->clear_pending) return false;
    state->clear_pending = false;

    const int32_t x = (int32_t)(state->efb_tl & 0x3FFu);
    const int32_t y = (int32_t)((state->efb_tl >> 10) & 0x3FFu);
    const int32_t width = (int32_t)(state->efb_wh & 0x3FFu) + 1;
    const int32_t height = (int32_t)((state->efb_wh >> 10) & 0x3FFu) + 1;
    const uint32_t x0 = clamp_u32(x, GX_EFB_WIDTH);
    const uint32_t y0 = clamp_u32(y, GX_EFB_HEIGHT);
    const uint32_t x1 = clamp_u32(x + width, GX_EFB_WIDTH);
    const uint32_t y1 = clamp_u32(y + height, GX_EFB_HEIGHT);

    out->rect.x = x0;
    out->rect.y = y0;
    out->rect.width = x1 > x0 ? x1 - x0 : 0;
    out->rect.height = y1 > y0 ? y1 - y0 : 0;
    out->color[0] = (float)(state->clear_ar & 0xFFu) / 255.0f;
    out->color[1] = (float)((state->clear_gb >> 8) & 0xFFu) / 255.0f;
    out->color[2] = (float)(state->clear_gb & 0xFFu) / 255.0f;
    out->color[3] = (float)((state->clear_ar >> 8) & 0xFFu) / 255.0f;
    out->depth = (float)(state->clear_z & 0xFFFFFFu) / GX_EFB_DEPTH_MAX;
    out->color_enable = ((state->blendmode & (1u << 3)) != 0) && gx_state_efb_has_color(state);
    out->alpha_enable = ((state->blendmode & (1u << 4)) != 0) && gx_state_efb_has_alpha(state);
    out->depth_enable = (state->zmode & (1u << 4)) != 0;
    return out->color_enable || out->alpha_enable || out->depth_enable;
}

bool gx_state_take_copy(GXRenderState *state, GXCopyState *out) {
    if (!state->copy_pending) return false;
    state->copy_pending = false;

    const int32_t x = (int32_t)(state->efb_tl & 0x3FFu);
    const int32_t y = (int32_t)((state->efb_tl >> 10) & 0x3FFu);
    const int32_t width = (int32_t)(state->efb_wh & 0x3FFu) + 1;
    const int32_t height = (int32_t)((state->efb_wh >> 10) & 0x3FFu) + 1;
    const uint32_t x0 = clamp_u32(x, GX_EFB_WIDTH);
    const uint32_t y0 = clamp_u32(y, GX_EFB_HEIGHT);
    const uint32_t x1 = clamp_u32(x + width, GX_EFB_WIDTH);
    const uint32_t y1 = clamp_u32(y + height, GX_EFB_HEIGHT);

    out->src_x = x0;
    out->src_y = y0;
    out->width = x1 > x0 ? x1 - x0 : 0;
    out->height = y1 > y0 ? y1 - y0 : 0;
    out->dst_ea = state->efb_addr << 5;
    out->dst_stride = (state->copy_stride & 0x3FFu) << 5;
    out->y_scale = state->copy_y_scale ? state->copy_y_scale : 256u;

    const uint32_t tpf = (state->copy_exec >> 3) & 0xFu;
    out->format = (uint8_t)((tpf >> 1) | ((tpf & 1u) << 3));
    out->half_scale = (state->copy_exec >> 9) & 1u;
    out->scale_invert = (state->copy_exec >> 10) & 1u;
    out->to_xfb = (state->copy_exec >> 14) & 1u;
    out->intensity = (state->copy_exec >> 15) & 1u;
    return out->width != 0 && out->height != 0;
}

// Self test
int gx_state_selftest(void) {
    GXRenderState st;
    gx_state_reset(&st);

    // ZMODE
    gx_state_apply_bp(&st, GX_BP_ZMODE, (1u) | (3u << 1) | (1u << 4));
    GXDepthState depth;
    gx_state_depth(&st, &depth);
    if (!depth.test_enable || !depth.write_enable ||
        depth.func != GX2_COMPARE_FUNC_LEQUAL)
        return 0;

    // BLENDMODE
    gx_state_apply_bp(&st, GX_BP_ZCOMPARE, GX_EFB_RGBA6_Z24);
    gx_state_apply_bp(&st, GX_BP_BLENDMODE,
                      (1u) | (1u << 3) | (1u << 4) | (5u << 5) | (4u << 8));
    GXBlendState blend;
    gx_state_blend(&st, &blend);
    if (!blend.blend_enable || blend.logic_op_enable) return 0;
    if (blend.src_color != GX2_BLEND_MODE_SRC_ALPHA) return 0;
    if (blend.dst_color != GX2_BLEND_MODE_INV_SRC_ALPHA) return 0;
    if (blend.src_alpha != GX2_BLEND_MODE_SRC_ALPHA) return 0;
    if (blend.dst_alpha != GX2_BLEND_MODE_INV_SRC_ALPHA) return 0;
    if (blend.color_combine != GX2_BLEND_COMBINE_MODE_ADD) return 0;
    if (!blend.color_update || !blend.alpha_update) return 0;

    // Colour factors map across the enum split and the derived alpha factors
    // swap to the alpha terms.
    gx_state_apply_bp(&st, GX_BP_BLENDMODE, (1u) | (2u << 5) | (2u << 8));
    gx_state_blend(&st, &blend);
    if (blend.src_color != GX2_BLEND_MODE_DST_COLOR) return 0;
    if (blend.dst_color != GX2_BLEND_MODE_SRC_COLOR) return 0;
    if (blend.src_alpha != GX2_BLEND_MODE_DST_ALPHA) return 0;
    if (blend.dst_alpha != GX2_BLEND_MODE_SRC_ALPHA) return 0;

    // Subtract
    gx_state_apply_bp(&st, GX_BP_BLENDMODE, (1u) | (1u << 11));
    gx_state_blend(&st, &blend);
    if (blend.src_color != GX2_BLEND_MODE_ONE || blend.dst_color != GX2_BLEND_MODE_ONE)
        return 0;
    if (blend.color_combine != GX2_BLEND_COMBINE_MODE_REV_SUB) return 0;
    if (blend.alpha_combine != GX2_BLEND_COMBINE_MODE_REV_SUB) return 0;

    // Logic op (blend disabled)
    // Logic_op_enable, mode=Xor(6) maps to GX2 XOR 0x66.
    gx_state_apply_bp(&st, GX_BP_BLENDMODE, (1u << 1) | (6u << 12));
    gx_state_blend(&st, &blend);
    if (blend.blend_enable || !blend.logic_op_enable) return 0;
    if (blend.logic_op != GX2_LOGIC_OP_XOR) return 0;

    // GENMODE
    gx_state_apply_bp(&st, GX_BP_GENMODE, (2u << 14));
    GXCullState cull;
    gx_state_cull(&st, &cull);
    if (!cull.cull_front || cull.cull_back) return 0;
    if (cull.front_face != GX2_FRONT_FACE_CW) return 0;

    gx_state_apply_bp(&st, GX_BP_GENMODE, (3u << 14));  // All
    gx_state_cull(&st, &cull);
    if (!cull.cull_front || !cull.cull_back) return 0;

    // ALPHACOMPARE
    gx_state_apply_bp(&st, GX_BP_ALPHACOMPARE,
                      0x80u | (0x10u << 8) | (6u << 16) | (3u << 19) | (0u << 22));
    GXAlphaTestState at;
    gx_state_alpha_test(&st, &at);
    if (at.ref0 != 0x80 || at.ref1 != 0x10) return 0;
    if (at.comp0 != GX2_COMPARE_FUNC_GEQUAL || at.comp1 != GX2_COMPARE_FUNC_LEQUAL) return 0;
    if (at.op != 0 || !at.enable) return 0;

    // ALWAYS/ALWAYS is a no-op test.
    gx_state_apply_bp(&st, GX_BP_ALPHACOMPARE, (7u << 16) | (7u << 19));
    gx_state_alpha_test(&st, &at);
    if (at.enable) return 0;

    gx_state_apply_bp(&st, GX_BP_ZCOMPARE, 1u);
    gx_state_apply_bp(&st, GX_BP_CONSTANTALPHA, 0x80u | (1u << 8));
    gx_state_apply_bp(&st, GX_BP_BLENDMODE, 1u | (1u << 3) | (1u << 4) | (4u << 8));
    gx_state_blend(&st, &blend);
    if (!blend.dual_source_alpha || blend.src_color != GX2_BLEND_MODE_SRC1_ALPHA)
        return 0;

    gx_state_apply_bp(&st, GX_BP_SCISSORTL, 342u | (342u << 12));
    gx_state_apply_bp(&st, GX_BP_SCISSORBR, 981u | (869u << 12));
    gx_state_apply_bp(&st, GX_BP_SCISSOROFFSET, 171u | (171u << 10));
    GXScissorState scissor;
    gx_state_scissor(&st, &scissor);
    if (scissor.x != 0 || scissor.y != 0 || scissor.width != 640 || scissor.height != 528)
        return 0;

    gx_state_apply_bp(&st, GX_BP_SCISSOROFFSET, 0);
    const float xf_viewport[6] = {320.0f, -264.0f, GX_EFB_DEPTH_MAX,
                                  320.0f, 264.0f, GX_EFB_DEPTH_MAX};
    GXViewportState viewport;
    gx_state_viewport(&st, xf_viewport, &viewport);
    if (viewport.x != 0.0f || viewport.y != 0.0f || viewport.width != 640.0f ||
        viewport.height != 528.0f || viewport.near_z != 0.0f || viewport.far_z != 1.0f)
        return 0;

    gx_state_apply_bp(&st, GX_BP_BLENDMODE, (1u << 3) | (1u << 4));
    gx_state_apply_bp(&st, GX_BP_ZMODE, (1u << 4));
    gx_state_apply_bp(&st, GX_BP_EFB_TL, 4u | (8u << 10));
    gx_state_apply_bp(&st, GX_BP_EFB_WH, 15u | (31u << 10));
    gx_state_apply_bp(&st, GX_BP_CLEAR_AR, 0xA011u);
    gx_state_apply_bp(&st, GX_BP_CLEAR_GB, 0x2233u);
    gx_state_apply_bp(&st, GX_BP_CLEAR_Z, 0x7FFFFFu);
    gx_state_apply_bp(&st, GX_BP_EFB_ADDR, 0x00010000u >> 5);
    gx_state_apply_bp(&st, GX_BP_TRIGGER_EFB_COPY, 1u << 11);
    GXClearState clear;
    if (!gx_state_take_clear(&st, &clear) || clear.rect.x != 4 || clear.rect.y != 8 ||
        clear.rect.width != 16 || clear.rect.height != 32 || clear.color[0] != 17.0f / 255.0f ||
        clear.color[1] != 34.0f / 255.0f || clear.color[2] != 51.0f / 255.0f ||
        clear.color[3] != 160.0f / 255.0f || clear.depth < 0.4999f || clear.depth > 0.5001f ||
        !clear.color_enable || !clear.alpha_enable || !clear.depth_enable)
        return 0;
    if (gx_state_take_clear(&st, &clear)) return 0;
    GXCopyState copy;
    if (!gx_state_take_copy(&st, &copy) || copy.format != 0)
        return 0;

    gx_state_apply_bp(&st, GX_BP_EFB_TL, 4u | (8u << 10));
    gx_state_apply_bp(&st, GX_BP_EFB_WH, 15u | (31u << 10));
    gx_state_apply_bp(&st, GX_BP_EFB_ADDR, 0x00010000u >> 5);
    gx_state_apply_bp(&st, GX_BP_COPY_STRIDE, 0);
    gx_state_apply_bp(&st, GX_BP_COPY_Y_SCALE, 256u);
    // target_pixel_format=0xC (>>1|<<3 -> realFormat 6 = RGBA8), intensity+xfb clear.
    gx_state_apply_bp(&st, GX_BP_TRIGGER_EFB_COPY, (0xCu << 3));
    if (gx_state_take_clear(&st, &clear)) return 0;
    if (!gx_state_take_copy(&st, &copy) || copy.src_x != 4 || copy.src_y != 8 ||
        copy.width != 16 || copy.height != 32 || copy.dst_ea != 0x00010000u ||
        copy.format != 0x6 || copy.to_xfb || copy.intensity || copy.half_scale ||
        copy.y_scale != 256u || copy.scale_invert)
        return 0;
    if (gx_state_take_copy(&st, &copy)) return 0;

    // realFormat 8 with intensity + copy-to-xfb + half scale.
    gx_state_apply_bp(&st, GX_BP_TRIGGER_EFB_COPY,
                      (0x1u << 3) | (1u << 9) | (1u << 10) | (1u << 14) | (1u << 15));
    if (!gx_state_take_copy(&st, &copy) || copy.format != 0x8 || !copy.intensity ||
        !copy.to_xfb || !copy.half_scale || !copy.scale_invert)
        return 0;

    return 1;
}
