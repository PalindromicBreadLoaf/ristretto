// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

// GX to GX2 shader program generators

#ifndef RISTRETTO_GPU_SHADER_GEN_H
#define RISTRETTO_GPU_SHADER_GEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gpu/gx2_shader.h"

#ifdef __cplusplus
extern "C" {
#endif

// Vertex program shape.
typedef struct {
    bool     has_color;      // pass a_color through as param 0
    bool     has_normal;     // fetch a_normal
    uint32_t num_texcoords;  // pass this many texcoords through as the next params
    bool     transform_position;
    bool     texgen[8];
    // Per-vertex diffuse lighting for colour channel 0 (rgb). When enabled the
    // colour param is computed from the material/ambient/light uniforms instead
    // of passed through. Requires has_color and has_normal.
    struct {
        bool     enable;
        uint32_t num_lights;      // directional lights, code emitted per light
        bool     diffuse;         // diffusefunc != None (multiply by N.L)
        bool     clamp;           // diffusefunc == Clamp (max(0, N.L))
        bool     mat_from_vertex; // material colour from vertex vs register
        bool     amb_from_vertex; // ambient colour from vertex vs register
    } light;
} ShaderGenVs;

// Lighting VS uniform block, in vec4 (cfile) registers, uploaded at this base
// with GX2SetVertexUniformReg. Layout (relative register, filled by
// gx_xf_build_light_cfile): 0..2 = normal matrix rows (n' = n * NRk),
// 3 = material rgba, 4 = ambient rgba, 5+2j = light j direction (xyz),
// 6+2j = light j colour (rgb).
#define SHADER_GEN_LIGHT_CFILE_BASE 32
#define SHADER_GEN_MAX_VS_LIGHTS    8

// Pixel program shape
typedef struct {
    bool sample_texture;  // sample texture unit 0 at the interpolated texcoord
    bool modulate_color;  // multiply the sample by the interpolated colour
} ShaderGenPs;

// Emit the R700 microcode program into buf.
size_t shader_gen_vs(uint8_t *buf, size_t cap, const ShaderGenVs *cfg);
size_t shader_gen_ps(uint8_t *buf, size_t cap, const ShaderGenPs *cfg);

// Fill the shader shape that matches the generated program.
void shader_gen_vs_shape(const ShaderGenVs *cfg, Gx2VsShape *out);
void shader_gen_ps_shape(const ShaderGenPs *cfg, Gx2PsShape *out);

// Build the modulate VS+PS and check them.
bool shader_gen_selftest(void);

#ifdef __cplusplus
}
#endif

#endif  // RISTRETTO_GPU_SHADER_GEN_H
