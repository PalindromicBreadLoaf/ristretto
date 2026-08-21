// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <whb/proc.h>
#include <whb/gfx.h>
#include <whb/log.h>
#include <whb/log_udp.h>
#include <whb/sdcard.h>

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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boot/boot.h"
#include "cpu/cpu_exec.h"
#include "cpu/ppc_decode.h"
#include "cpu/ppc_interp.h"
#include "cpu/ppc_xlate.h"
#include "disc/disc.h"
#include "gpu/gx2_shader.h"
#include "gpu/gx_fifo.h"
#include "gpu/gx_state.h"
#include "gpu/gx_texture.h"
#include "gpu/r700_emit.h"
#include "gpu/tev_modulate_shader.h"
#include "ios/ios_ipc.h"
#include "mem/wii_memory.h"

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

// Verify memory setup worked before relying on it.
static bool selfTestWiiMemory(void) {
    bool ok = true;

    wii_write_u32(0x80000200, 0xC0FFEE00);
    if (wii_read_u32(0xC0000200) != 0xC0FFEE00 || wii_read_u32(0x00000200) != 0xC0FFEE00) {
        WHBLogPrint("wii_mem selftest: MEM1 cached/uncached/physical mirrors disagree");
        ok = false;
    }

    wii_write_u32(0x90001000, 0xABCDEF12);
    if (wii_read_u32(0xD0001000) != 0xABCDEF12) {
        WHBLogPrint("wii_mem selftest: MEM2 cached/uncached mirror disagree");
        ok = false;
    }

    if (wii_mem_ptr(0x81800000) != NULL || wii_mem_ptr(0x94000000) != NULL) {
        WHBLogPrint("wii_mem selftest: out-of-range EA resolved to a pointer");
        ok = false;
    }

    if (wii_mem_range(0x817FFFFC, 8) != NULL) {
        WHBLogPrint("wii_mem selftest: range crossing MEM1 end was accepted");
        ok = false;
    }

    wii_write_u32(0x80000200, 0);
    wii_write_u32(0x90001000, 0);
    return ok;
}

static void putBe32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

// Build a minimal test DOL
#define TEST_DOL_SIZE   0x140u
#define TEST_TEXT_EA    0x80004000u
#define TEST_DATA_EA    0x80004100u
#define TEST_BSS_EA     0x80004200u

static uint32_t buildSyntheticDol(uint8_t *buf) {
    memset(buf, 0, TEST_DOL_SIZE);
    putBe32(buf + 0x00, 0x100);           // text[0] file offset
    putBe32(buf + 0x1C, 0x120);           // data[0] file offset
    putBe32(buf + 0x48, TEST_TEXT_EA);    // text[0] address
    putBe32(buf + 0x64, TEST_DATA_EA);    // data[0] address
    putBe32(buf + 0x90, 0x20);            // text[0] size
    putBe32(buf + 0xAC, 0x20);            // data[0] size
    putBe32(buf + 0xD8, TEST_BSS_EA);     // bss address
    putBe32(buf + 0xDC, 0x40);            // bss size
    putBe32(buf + 0xE0, TEST_TEXT_EA);    // entry point

    putBe32(buf + 0x100, 0x7C13FBA6);     // mtspr HID4, r0
    putBe32(buf + 0x104, 0xDEADBEEF);     // text marker
    putBe32(buf + 0x120, 0xCAFEB0BA);     // data marker
    return TEST_DOL_SIZE;
}

static bool selfTestBoot(void) {
    uint8_t dol[TEST_DOL_SIZE];
    uint32_t size = buildSyntheticDol(dol);

    DolLoadResult r;
    if (!boot_dol_from_buffer(dol, size, "RTST01", &r)) {
        WHBLogPrint("boot selftest: dol_load rejected a valid image");
        return false;
    }

    bool ok = true;
    if (r.entry_point != TEST_TEXT_EA || r.section_count != 2 || !r.is_wii) {
        WHBLogPrintf("boot selftest: header wrong (entry=0x%08X sects=%u wii=%d)",
                     r.entry_point, r.section_count, r.is_wii);
        ok = false;
    }
    if (wii_read_u32(TEST_TEXT_EA) != 0x7C13FBA6 ||
        wii_read_u32(TEST_TEXT_EA + 4) != 0xDEADBEEF ||
        wii_read_u32(TEST_DATA_EA) != 0xCAFEB0BA) {
        WHBLogPrint("boot selftest: section bytes did not land in guest memory");
        ok = false;
    }
    if (wii_read_u32(0x80000030) != (TEST_BSS_EA + 0x40)) {
        WHBLogPrintf("boot selftest: arenaLo wrong (0x%08X)", wii_read_u32(0x80000030));
        ok = false;
    }
    if (memcmp(wii_mem_ptr(0x80000000), "RTST01", 6) != 0) {
        WHBLogPrint("boot selftest: disc ID not written to __OSBootInfo");
        ok = false;
    }
    return ok;
}

// Load proper Wii DOL from SD Card.
static void tryLoadDolFromSd(void) {
    if (!WHBMountSdCard()) {
        WHBLogPrint("boot: SD mount failed. Skipping real DOL smoke test");
        return;
    }

    char path[256];
    snprintf(path, sizeof(path), "%s/wiiu/apps/ristretto/boot.dol",
             WHBGetSdCardMountPath());

    FILE *f = fopen(path, "rb");
    if (!f) {
        WHBLogPrintf("boot: no %s (optional). Skipping real DOL smoke test", path);
        WHBUnmountSdCard();
        return;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) {
        WHBLogPrint("boot: boot.dol is empty");
        fclose(f);
        WHBUnmountSdCard();
        return;
    }

    uint8_t *buf = malloc((size_t)len);
    if (buf && fread(buf, 1, (size_t)len, f) == (size_t)len) {
        DolLoadResult r;
        WHBLogPrintf("boot: loading real DOL from SD (%ld bytes)", len);
        if (boot_dol_from_buffer(buf, (uint32_t)len, NULL, &r))
            WHBLogPrint("boot: real DOL loaded OK");
        else
            WHBLogPrint("boot: real DOL failed to load");
    } else {
        WHBLogPrint("boot: failed to read boot.dol");
    }
    free(buf);
    fclose(f);
    WHBUnmountSdCard();
}

// A disc image kept mounted for the app's lifetime.
static Disc g_disc;
static bool g_disc_mounted = false;

static void tryMountDiscFromSd(void) {
    if (!WHBMountSdCard()) {
        WHBLogPrint("disc: SD mount failed. Skipping disc loopback test");
        return;
    }

    char path[256];
    snprintf(path, sizeof(path), "%s/wiiu/apps/ristretto/game.iso",
             WHBGetSdCardMountPath());

    if (!disc_open_file(&g_disc, path)) {
        WHBLogPrintf("disc: no %s. Skipping disc loopback test", path);
        WHBUnmountSdCard();
        return;
    }

    g_disc_mounted = true;
    ios_ipc_mount_disc(&g_disc);
    WHBLogPrintf("disc: mounted id=%s (%s) size=%llu MiB", g_disc.game_id,
                 g_disc.is_wii ? "Wii" : "GC",
                 (unsigned long long)(g_disc.size >> 20));

    if (!g_disc.is_wii) {
        WHBLogPrint("disc: GC image mounted");
        return;
    }

    uint64_t part;
    if (!disc_find_game_partition(&g_disc, &part)) {
        WHBLogPrint("disc: no data partition found");
        return;
    }
    if (!disc_open_partition(&g_disc, part)) {
        WHBLogPrintf("disc: open partition @0x%llx failed",
                     (unsigned long long)part);
        return;
    }

    uint8_t boot[0x20];
    if (!disc_read_partition(&g_disc, 0, boot, sizeof(boot))) {
        WHBLogPrint("disc: decrypted partition read failed");
        return;
    }
    // Decrypted partition data opens with boot.bin.
    char id[7] = {0};
    memcpy(id, boot, 6);
    WHBLogPrintf("disc: game partition @0x%llx decrypted OK, boot id=%s",
                 (unsigned long long)part, id);
}

int main(int argc, char **argv) {
    WHBProcInit();
    WHBLogUdpInit();
    WHBGfxInit();

    if (!wii_mem_init()) {
        WHBLogPrint("Failed to allocate guest memory map");
        WHBGfxShutdown();
        WHBLogUdpDeinit();
        WHBProcShutdown();
        return -1;
    }
    wii_mem_setup_lowmem();
    wii_mem_log_layout();
    WHBLogPrintf("wii_mem selftest: %s", selfTestWiiMemory() ? "PASS" : "FAIL");
    WHBLogPrintf("boot selftest: %s", selfTestBoot() ? "PASS" : "FAIL");
    tryLoadDolFromSd();

    switch (cpu_exec_selftest()) {
        case CPU_EXEC_PASS:        WHBLogPrint("cpu_exec selftest: PASS"); break;
        case CPU_EXEC_UNAVAILABLE: WHBLogPrint("cpu_exec selftest: UNAVAILABLE"); break;
        case CPU_EXEC_FAIL:        WHBLogPrint("cpu_exec selftest: FAIL"); break;
    }
    cpu_ps_probe_all();
    cpu_privilege_probe_all();

    WHBLogPrintf("cpu_xlate selftest: decoder %s", ppc_decode_selftest() ? "PASS" : "FAIL");
    WHBLogPrintf("cpu_xlate selftest: interp %s", ppc_interp_selftest() ? "PASS" : "FAIL");
    WHBLogPrintf("cpu_xlate selftest: identity-block %s", ppc_xlate_identity_selftest() ? "PASS" : "FAIL");
    WHBLogPrintf("cpu_xlate selftest: memblock %s", ppc_xlate_memblock_selftest() ? "PASS" : "FAIL");
    WHBLogPrintf("cpu_xlate selftest: branches %s", ppc_xlate_branch_selftest() ? "PASS" : "FAIL");
    WHBLogPrintf("cpu_xlate selftest: mmio %s", ppc_xlate_mmio_selftest() ? "PASS" : "FAIL");
    WHBLogPrintf("cpu_xlate selftest: entry %s", ppc_xlate_entry_selftest() ? "PASS" : "FAIL");

    switch (ios_ipc_selftest()) {
        case IOS_IPC_SELFTEST_PASS: WHBLogPrint("ios_ipc selftest: PASS"); break;
        case IOS_IPC_SELFTEST_FAIL: WHBLogPrint("ios_ipc selftest: FAIL"); break;
    }

    WHBLogPrintf("disc selftest: %s", disc_selftest() ? "PASS" : "FAIL");
    tryMountDiscFromSd();

    WHBLogPrintf("gx_fifo selftest: %s", gx_fifo_selftest() ? "PASS" : "FAIL");
    WHBLogPrintf("gx_state selftest: %s", gx_state_selftest() ? "PASS" : "FAIL");
    WHBLogPrintf("gx_texture selftest: %s", gx_texture_selftest() ? "PASS" : "FAIL");
    WHBLogPrintf("r700_emit selftest: %s", r700_emit_selftest() ? "PASS" : "FAIL");
    WHBLogPrintf("gx2_shader selftest: %s", gx2_shader_selftest() ? "PASS" : "FAIL");

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

    if (g_disc_mounted) {
        ios_ipc_mount_disc(NULL);
        disc_close(&g_disc);
        WHBUnmountSdCard();
    }
    wii_mem_shutdown();
    WHBGfxShutdown();
    WHBLogUdpDeinit();
    WHBProcShutdown();
    return result;
}
