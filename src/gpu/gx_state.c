// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu/gx_state.h"

#include <string.h>

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

void gx_state_reset(GXRenderState *state) {
    memset(state, 0, sizeof(*state));
}

void gx_state_apply_bp(GXRenderState *state, uint8_t reg, uint32_t value) {
    switch (reg) {
    case GX_BP_GENMODE:       state->genmode = value; break;
    case GX_BP_ZMODE:         state->zmode = value; break;
    case GX_BP_BLENDMODE:     state->blendmode = value; break;
    case GX_BP_CONSTANTALPHA: state->constant_alpha = value; break;
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
    out->color_update = (b >> 3) & 1;
    out->alpha_update = (b >> 4) & 1;
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
    // Todo: EFB pixel format gating
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

    return 1;
}
