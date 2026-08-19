// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <whb/proc.h>
#include <whb/gfx.h>
#include <whb/log.h>
#include <whb/log_udp.h>

#include <gx2/draw.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/sampler.h>
#include <gx2/shaders.h>
#include <gx2/surface.h>
#include <gx2/texture.h>
#include <gx2/utils.h>
#include <gx2r/buffer.h>
#include <gx2r/draw.h>

#include <malloc.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "gpu/tev_modulate_shader.h"

// PoC for a fixed-function pipeline via GX2.

#define TEX_SIZE 64

static const float sPositions[] = {
    -0.8f, -0.8f,
     0.8f, -0.8f,
    -0.8f,  0.8f,
     0.8f,  0.8f,
};

static const float sColours[] = {
    1.0f, 0.2f, 0.2f, 1.0f,
    0.2f, 1.0f, 0.2f, 1.0f,
    0.2f, 0.2f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f,
};

static const float sTexCoords[] = {
    0.0f, 1.0f,
    1.0f, 1.0f,
    0.0f, 0.0f,
    1.0f, 0.0f,
};

static const float sIdentity[16] = {
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f,
};

static void fillCheckerboard(GX2Texture *texture) {
    uint8_t *image = (uint8_t *)texture->surface.image;
    uint32_t pitch = texture->surface.pitch;

    for (uint32_t y = 0; y < TEX_SIZE; ++y) {
        for (uint32_t x = 0; x < TEX_SIZE; ++x) {
            uint8_t *px = image + (y * pitch + x) * 4;
            bool light = ((x >> 3) ^ (y >> 3)) & 1;
            px[0] = light ? 0xFF : 0x30;                 // R
            px[1] = light ? 0xFF : 0x30;                 // G
            px[2] = light ? 0xFF : 0x30;                 // B
            px[3] = 0xFF;                                // A
        }
    }

    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE,
                  texture->surface.image, texture->surface.imageSize);
}

static bool createTexture(GX2Texture *texture) {
    memset(texture, 0, sizeof(*texture));
    texture->surface.dim       = GX2_SURFACE_DIM_TEXTURE_2D;
    texture->surface.width     = TEX_SIZE;
    texture->surface.height    = TEX_SIZE;
    texture->surface.depth     = 1;
    texture->surface.mipLevels = 1;
    texture->surface.format    = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    texture->surface.aa        = GX2_AA_MODE1X;
    texture->surface.use       = GX2_SURFACE_USE_TEXTURE;
    texture->surface.tileMode  = GX2_TILE_MODE_LINEAR_ALIGNED;
    texture->surface.swizzle   = 0;
    texture->viewNumMips       = 1;
    texture->viewNumSlices     = 1;
    texture->compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A);

    GX2CalcSurfaceSizeAndAlignment(&texture->surface);
    GX2InitTextureRegs(texture);

    texture->surface.image = memalign(texture->surface.alignment, texture->surface.imageSize);
    if (!texture->surface.image) {
        return false;
    }

    fillCheckerboard(texture);
    return true;
}

static bool makeAttributeBuffer(GX2RBuffer *buffer, const void *data,
                                uint32_t elemSize, uint32_t elemCount) {
    memset(buffer, 0, sizeof(*buffer));
    buffer->flags = GX2R_RESOURCE_BIND_VERTEX_BUFFER |
                    GX2R_RESOURCE_USAGE_CPU_READ |
                    GX2R_RESOURCE_USAGE_CPU_WRITE |
                    GX2R_RESOURCE_USAGE_GPU_READ;
    buffer->elemSize  = elemSize;
    buffer->elemCount = elemCount;
    if (!GX2RCreateBuffer(buffer)) {
        return false;
    }

    void *dst = GX2RLockBufferEx(buffer, 0);
    memcpy(dst, data, (size_t)elemSize * elemCount);
    GX2RUnlockBufferEx(buffer, 0);
    return true;
}

int main(int argc, char **argv) {
    WHBProcInit();
    WHBLogUdpInit();
    WHBGfxInit();

    int result = 0;
    WHBGfxShaderGroup group = {0};
    GX2RBuffer positionBuffer = {0};
    GX2RBuffer colourBuffer   = {0};
    GX2RBuffer texCoordBuffer = {0};
    GX2Texture texture = {0};
    GX2Sampler sampler;

    if (!WHBGfxLoadGFDShaderGroup(&group, 0, g_tevModulateShaderGsh)) {
        WHBLogPrint("Failed to load TEV shader group");
        result = -1;
        goto exit;
    }

    WHBGfxInitShaderAttribute(&group, "a_position", 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32);
    WHBGfxInitShaderAttribute(&group, "a_color",    1, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32);
    WHBGfxInitShaderAttribute(&group, "a_texcoord", 2, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32);
    WHBGfxInitFetchShader(&group);

    if (!createTexture(&texture)) {
        WHBLogPrint("Failed to allocate texture");
        result = -1;
        goto exit;
    }
    GX2InitSampler(&sampler, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_POINT);

    if (!makeAttributeBuffer(&positionBuffer, sPositions, 2 * sizeof(float), 4) ||
        !makeAttributeBuffer(&colourBuffer,   sColours,   4 * sizeof(float), 4) ||
        !makeAttributeBuffer(&texCoordBuffer, sTexCoords, 2 * sizeof(float), 4)) {
        WHBLogPrint("Failed to allocate vertex buffers");
        result = -1;
        goto exit;
    }

    while (WHBProcIsRunning()) {
        WHBGfxBeginRender();

        WHBGfxBeginRenderTV();
        WHBGfxClearColor(0.1f, 0.1f, 0.12f, 1.0f);
        GX2SetFetchShader(&group.fetchShader);
        GX2SetVertexShader(group.vertexShader);
        GX2SetPixelShader(group.pixelShader);
        GX2SetVertexUniformReg(0, 16, sIdentity);
        GX2SetPixelTexture(&texture, group.pixelShader->samplerVars[0].location);
        GX2SetPixelSampler(&sampler, group.pixelShader->samplerVars[0].location);
        GX2RSetAttributeBuffer(&positionBuffer, 0, positionBuffer.elemSize, 0);
        GX2RSetAttributeBuffer(&colourBuffer,   1, colourBuffer.elemSize, 0);
        GX2RSetAttributeBuffer(&texCoordBuffer, 2, texCoordBuffer.elemSize, 0);
        GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLE_STRIP, 4, 0, 1);
        WHBGfxFinishRenderTV();

        WHBGfxBeginRenderDRC();
        WHBGfxClearColor(0.1f, 0.1f, 0.12f, 1.0f);
        GX2SetFetchShader(&group.fetchShader);
        GX2SetVertexShader(group.vertexShader);
        GX2SetPixelShader(group.pixelShader);
        GX2SetVertexUniformReg(0, 16, sIdentity);
        GX2SetPixelTexture(&texture, group.pixelShader->samplerVars[0].location);
        GX2SetPixelSampler(&sampler, group.pixelShader->samplerVars[0].location);
        GX2RSetAttributeBuffer(&positionBuffer, 0, positionBuffer.elemSize, 0);
        GX2RSetAttributeBuffer(&colourBuffer,   1, colourBuffer.elemSize, 0);
        GX2RSetAttributeBuffer(&texCoordBuffer, 2, texCoordBuffer.elemSize, 0);
        GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLE_STRIP, 4, 0, 1);
        WHBGfxFinishRenderDRC();

        WHBGfxFinishRender();
    }

exit:
    WHBLogPrint("Exiting...");
    if (texture.surface.image) {
        free(texture.surface.image);
    }
    GX2RDestroyBufferEx(&positionBuffer, 0);
    GX2RDestroyBufferEx(&colourBuffer, 0);
    GX2RDestroyBufferEx(&texCoordBuffer, 0);
    WHBGfxFreeShaderGroup(&group);

    WHBGfxShutdown();
    WHBLogUdpDeinit();
    WHBProcShutdown();
    return result;
}
