// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu/gx_efb.h"

#include <malloc.h>
#include <string.h>

#include <gx2/context.h>
#include <gx2/draw.h>
#include <gx2/enum.h>
#include <gx2/registers.h>
#include <gx2/surface.h>
#include <gx2/utils.h>
#include <gx2r/draw.h>
#include <gx2r/surface.h>

#include "gpu/gx_tev.h"

static const float kIdentity[16] = {
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f,
};

static GX2ChannelMask color_mask(bool color_enable, bool alpha_enable) {
    if (color_enable && alpha_enable) return GX2_CHANNEL_MASK_RGBA;
    if (color_enable) return GX2_CHANNEL_MASK_RGB;
    if (alpha_enable) return GX2_CHANNEL_MASK_A;
    return (GX2ChannelMask)0;
}

static bool create_buffer(GX2RBuffer *buffer, uint32_t elem_size, uint32_t elem_count) {
    memset(buffer, 0, sizeof(*buffer));
    buffer->flags = GX2R_RESOURCE_BIND_VERTEX_BUFFER | GX2R_RESOURCE_USAGE_CPU_WRITE |
                    GX2R_RESOURCE_USAGE_GPU_READ;
    buffer->elemSize = elem_size;
    buffer->elemCount = elem_count;
    return GX2RCreateBuffer(buffer);
}

static bool init_clear_shader(GXEfb *efb) {
    GX2AttribStream attribs[2] = {
        {
            .location = 0, .buffer = 0, .offset = 0,
            .format = GX2_ATTRIB_FORMAT_FLOAT_32_32_32,
            .type = GX2_ATTRIB_INDEX_PER_VERTEX, .aluDivisor = 0,
            .mask = GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_Z, GX2_SQ_SEL_1),
            .endianSwap = GX2_ENDIAN_SWAP_DEFAULT,
        },
        {
            .location = 1, .buffer = 1, .offset = 0,
            .format = GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32,
            .type = GX2_ATTRIB_INDEX_PER_VERTEX, .aluDivisor = 0,
            .mask = GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_Z, GX2_SQ_SEL_W),
            .endianSwap = GX2_ENDIAN_SWAP_DEFAULT,
        },
    };
    TevConfig tev;
    gx_tev_reset(&tev);
    tev.stage[0].color.d = GX_CC_RASC;
    tev.stage[0].alpha.d = GX_CA_RASA;
    if (!gx2_bind_build_tev(&efb->clear_shader, &tev, attribs, 2)) return false;
    if (!create_buffer(&efb->clear_position, 3 * sizeof(float), 4) ||
        !create_buffer(&efb->clear_color, 4 * sizeof(float), 4)) {
        return false;
    }
    return true;
}

bool gx_efb_init(GXEfb *efb) {
    if (!efb) return false;
    memset(efb, 0, sizeof(*efb));

    efb->context = memalign(GX2_CONTEXT_STATE_ALIGNMENT, sizeof(*efb->context));
    if (!efb->context) return false;
    GX2SetupContextStateEx(efb->context, TRUE);

    efb->color.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    efb->color.surface.width = GX_EFB_WIDTH;
    efb->color.surface.height = GX_EFB_HEIGHT;
    efb->color.surface.depth = 1;
    efb->color.surface.mipLevels = 1;
    efb->color.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    efb->color.surface.aa = GX2_AA_MODE1X;
    efb->color.surface.use = GX2_SURFACE_USE_COLOR_BUFFER;
    efb->color.surface.tileMode = GX2_TILE_MODE_DEFAULT;
    efb->color.viewNumSlices = 1;
    GX2CalcSurfaceSizeAndAlignment(&efb->color.surface);
    GX2InitColorBufferRegs(&efb->color);
    if (!GX2RCreateSurface(&efb->color.surface,
                           GX2R_RESOURCE_BIND_COLOR_BUFFER | GX2R_RESOURCE_USAGE_GPU_READ |
                           GX2R_RESOURCE_USAGE_GPU_WRITE)) {
        goto fail;
    }

    efb->depth.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    efb->depth.surface.width = GX_EFB_WIDTH;
    efb->depth.surface.height = GX_EFB_HEIGHT;
    efb->depth.surface.depth = 1;
    efb->depth.surface.mipLevels = 1;
    efb->depth.surface.format = GX2_SURFACE_FORMAT_FLOAT_D24_S8;
    efb->depth.surface.aa = GX2_AA_MODE1X;
    efb->depth.surface.use = GX2_SURFACE_USE_DEPTH_BUFFER;
    efb->depth.surface.tileMode = GX2_TILE_MODE_DEFAULT;
    efb->depth.viewNumSlices = 1;
    efb->depth.depthClear = 1.0f;
    GX2CalcSurfaceSizeAndAlignment(&efb->depth.surface);
    GX2InitDepthBufferRegs(&efb->depth);
    GX2InitDepthBufferHiZEnable(&efb->depth, FALSE);
    if (!GX2RCreateSurface(&efb->depth.surface,
                           GX2R_RESOURCE_BIND_DEPTH_BUFFER | GX2R_RESOURCE_USAGE_GPU_READ |
                           GX2R_RESOURCE_USAGE_GPU_WRITE)) {
        goto fail;
    }

    if (!init_clear_shader(efb)) goto fail;
    efb->ready = true;
    return true;

fail:
    gx_efb_shutdown(efb);
    return false;
}

void gx_efb_shutdown(GXEfb *efb) {
    if (!efb) return;
    if (efb->clear_position.elemSize) GX2RDestroyBufferEx(&efb->clear_position, 0);
    if (efb->clear_color.elemSize) GX2RDestroyBufferEx(&efb->clear_color, 0);
    gx2_bind_free(&efb->clear_shader);
    if (efb->color.surface.image) GX2RDestroySurfaceEx(&efb->color.surface,
                                                        GX2R_RESOURCE_BIND_NONE);
    if (efb->depth.surface.image) GX2RDestroySurfaceEx(&efb->depth.surface,
                                                        GX2R_RESOURCE_BIND_NONE);
    free(efb->context);
    memset(efb, 0, sizeof(*efb));
}

bool gx_efb_bind(GXEfb *efb) {
    if (!efb || !efb->ready) return false;
    GX2SetContextState(efb->context);
    GX2SetColorBuffer(&efb->color, GX2_RENDER_TARGET_0);
    GX2SetDepthBuffer(&efb->depth);
    GX2SetRasterizerClipControl(TRUE, TRUE);
    return true;
}

void gx_efb_apply_viewport(const GXViewportState *viewport) {
    if (!viewport) return;
    GX2SetViewport(viewport->x, (float)GX_EFB_HEIGHT - viewport->y - viewport->height,
                   viewport->width, viewport->height, viewport->near_z, viewport->far_z);
}

void gx_efb_apply_scissor(const GXScissorState *scissor) {
    if (!scissor) return;
    const uint32_t y = GX_EFB_HEIGHT - scissor->y - scissor->height;
    GX2SetScissor(scissor->x, y, scissor->width, scissor->height);
}

void gx_efb_apply_color_mask(bool color_enable, bool alpha_enable) {
    const GX2ChannelMask mask = color_mask(color_enable, alpha_enable);
    GX2SetTargetChannelMasks(mask, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0,
                             (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0,
                             (GX2ChannelMask)0);
}

bool gx_efb_clear(GXEfb *efb, const GXClearState *clear) {
    if (!efb || !clear || !gx_efb_bind(efb) || clear->rect.width == 0 ||
        clear->rect.height == 0) {
        return false;
    }

    float *position = GX2RLockBufferEx(&efb->clear_position, 0);
    float *color = GX2RLockBufferEx(&efb->clear_color, 0);
    if (!position || !color) {
        if (position) GX2RUnlockBufferEx(&efb->clear_position, 0);
        if (color) GX2RUnlockBufferEx(&efb->clear_color, 0);
        return false;
    }
    static const float xy[4][2] = {
        {-1.0f, -1.0f}, {1.0f, -1.0f}, {-1.0f, 1.0f}, {1.0f, 1.0f},
    };
    for (uint32_t i = 0; i < 4; ++i) {
        position[i * 3 + 0] = xy[i][0];
        position[i * 3 + 1] = xy[i][1];
        position[i * 3 + 2] = clear->depth;
        memcpy(&color[i * 4], clear->color, sizeof(clear->color));
    }
    GX2RUnlockBufferEx(&efb->clear_position, 0);
    GX2RUnlockBufferEx(&efb->clear_color, 0);

    static const float zero_cfile[8][4] = {{0}};
    const GXViewportState full_viewport = {
        .x = 0.0f, .y = 0.0f, .width = GX_EFB_WIDTH, .height = GX_EFB_HEIGHT,
        .near_z = 0.0f, .far_z = 1.0f,
    };
    GX2SetFetchShader(&efb->clear_shader.fs);
    GX2SetVertexShader(&efb->clear_shader.vs);
    GX2SetPixelShader(&efb->clear_shader.ps);
    GX2SetVertexUniformReg(0, 16, (void *)kIdentity);
    GX2SetPixelUniformReg(0, 8 * 4, (void *)&zero_cfile[0][0]);
    gx_efb_apply_viewport(&full_viewport);
    gx_efb_apply_scissor(&clear->rect);
    GX2SetDepthOnlyControl(clear->depth_enable, clear->depth_enable,
                           GX2_COMPARE_FUNC_ALWAYS);
    GX2SetColorControl(GX2_LOGIC_OP_COPY, 0, TRUE,
                       clear->color_enable || clear->alpha_enable);
    gx_efb_apply_color_mask(clear->color_enable, clear->alpha_enable);
    GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_ZERO,
                       GX2_BLEND_COMBINE_MODE_ADD, TRUE, GX2_BLEND_MODE_ONE,
                       GX2_BLEND_MODE_ZERO, GX2_BLEND_COMBINE_MODE_ADD);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CW, FALSE, FALSE);
    GX2RSetAttributeBuffer(&efb->clear_position, 0, efb->clear_position.elemSize, 0);
    GX2RSetAttributeBuffer(&efb->clear_color, 1, efb->clear_color.elemSize, 0);
    GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLE_STRIP, 4, 0, 1);
    return true;
}
