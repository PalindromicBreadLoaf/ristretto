// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

// Assemble generated R700 microcode + register mirrors into live GX2 shader
// objects and bind them.

#ifndef RISTRETTO_GPU_GX2_BIND_H
#define RISTRETTO_GPU_GX2_BIND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <gx2/shaders.h>

#include "gpu/gx2_shader.h"
#include "gpu/gx_tev.h"

#ifdef __cplusplus
extern "C" {
#endif

// Up to one texture sampler per GX texmap.
#define GX2_BIND_MAX_SAMPLERS 8

// A fully built and bindable shader trio plus the GPU-visible program buffers it
// owns.
typedef struct {
    GX2VertexShader vs;
    GX2PixelShader  ps;
    GX2FetchShader  fs;
    // One sampler var per distinct texmap the pixel shader samples.
    GX2SamplerVar   ps_samplers[GX2_BIND_MAX_SAMPLERS];
    uint32_t        ps_sampler_count;

    void *vs_program;
    void *ps_program;
    void *fs_program;

    bool valid;
} Gx2BoundShader;

// Build a bindable shader from generated microcode + register mirrors.
bool gx2_bind_build(Gx2BoundShader *out,
                    const uint8_t *vs_prog, size_t vs_size, const Gx2VsRegs *vs_regs,
                    const uint8_t *ps_prog, size_t ps_size, const Gx2PsRegs *ps_regs,
                    const GX2AttribStream *attribs, uint32_t attrib_count,
                    const uint8_t *sampler_locs, uint32_t sampler_count);

bool gx2_bind_build_modulate(Gx2BoundShader *out,
                             const GX2AttribStream *attribs, uint32_t attrib_count);

// Build a bindable trio from a decoded multi-stage TEV config.
bool gx2_bind_build_tev(Gx2BoundShader *out, const TevConfig *cfg,
                        const GX2AttribStream *attribs, uint32_t attrib_count);

bool gx2_bind_build_tev_ex(Gx2BoundShader *out, const TevConfig *cfg,
                           bool transform_position, const bool *texgen,
                           const GX2AttribStream *attribs, uint32_t attrib_count);

// Upload the TEV colour/konst registers as the pixel-shader uniform cfile.
void gx2_bind_set_tev_uniforms(const TevConfig *cfg);

// Release the GPU-visible program buffers.
void gx2_bind_free(Gx2BoundShader *out);

#ifdef __cplusplus
}
#endif

#endif  // RISTRETTO_GPU_GX2_BIND_H
