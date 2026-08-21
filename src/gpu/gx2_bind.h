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

#ifdef __cplusplus
extern "C" {
#endif

// A fully built and bindable shader trio plus the GPU-visible program buffers it
// owns.
typedef struct {
    GX2VertexShader vs;
    GX2PixelShader  ps;
    GX2FetchShader  fs;
    GX2SamplerVar   ps_sampler;

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
                    bool sampler_2d);

bool gx2_bind_build_modulate(Gx2BoundShader *out,
                             const GX2AttribStream *attribs, uint32_t attrib_count);

// Release the GPU-visible program buffers.
void gx2_bind_free(Gx2BoundShader *out);

#ifdef __cplusplus
}
#endif

#endif  // RISTRETTO_GPU_GX2_BIND_H
