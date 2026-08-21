// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu/gx2_bind.h"

#include <malloc.h>
#include <string.h>

#include <gx2/enum.h>
#include <gx2/mem.h>

#include "gpu/shader_gen.h"

_Static_assert(sizeof(Gx2VsRegs) == sizeof(((GX2VertexShader *)0)->regs),
               "Gx2VsRegs must mirror GX2VertexShader::regs");
_Static_assert(sizeof(Gx2PsRegs) == sizeof(((GX2PixelShader *)0)->regs),
               "Gx2PsRegs must mirror GX2PixelShader::regs");

static void *alloc_program(const uint8_t *prog, size_t size) {
    if (!prog || size == 0) return NULL;
    void *buf = memalign(GX2_SHADER_PROGRAM_ALIGNMENT, size);
    if (!buf) return NULL;
    memcpy(buf, prog, size);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, buf, (uint32_t)size);
    return buf;
}

bool gx2_bind_build(Gx2BoundShader *out,
                    const uint8_t *vs_prog, size_t vs_size, const Gx2VsRegs *vs_regs,
                    const uint8_t *ps_prog, size_t ps_size, const Gx2PsRegs *ps_regs,
                    const GX2AttribStream *attribs, uint32_t attrib_count,
                    bool sampler_2d) {
    if (!out || !vs_prog || !ps_prog || !vs_regs || !ps_regs ||
        (attrib_count && !attribs)) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    out->vs_program = alloc_program(vs_prog, vs_size);
    out->ps_program = alloc_program(ps_prog, ps_size);
    if (!out->vs_program || !out->ps_program) {
        gx2_bind_free(out);
        return false;
    }

    // Vertex shader
    memcpy(&out->vs.regs, vs_regs, sizeof(out->vs.regs));
    out->vs.size    = (uint32_t)vs_size;
    out->vs.program = out->vs_program;
    out->vs.mode    = GX2_SHADER_MODE_UNIFORM_REGISTER;

    // Pixel shader
    memcpy(&out->ps.regs, ps_regs, sizeof(out->ps.regs));
    out->ps.size    = (uint32_t)ps_size;
    out->ps.program = out->ps_program;
    out->ps.mode    = GX2_SHADER_MODE_UNIFORM_REGISTER;

    if (sampler_2d) {
        out->ps_sampler.name     = "s_texture";
        out->ps_sampler.type     = GX2_SAMPLER_VAR_TYPE_SAMPLER_2D;
        out->ps_sampler.location = 0;
        out->ps.samplerVars      = &out->ps_sampler;
        out->ps.samplerVarCount  = 1;
    }

    // Fetch shader
    uint32_t fs_size = GX2CalcFetchShaderSizeEx(attrib_count,
                                                GX2_FETCH_SHADER_TESSELLATION_NONE,
                                                GX2_TESSELLATION_MODE_DISCRETE);
    out->fs_program = memalign(GX2_SHADER_PROGRAM_ALIGNMENT, fs_size);
    if (!out->fs_program) {
        gx2_bind_free(out);
        return false;
    }
    GX2InitFetchShaderEx(&out->fs, (uint8_t *)out->fs_program, attrib_count, attribs,
                         GX2_FETCH_SHADER_TESSELLATION_NONE,
                         GX2_TESSELLATION_MODE_DISCRETE);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, out->fs_program, fs_size);

    out->valid = true;
    return true;
}

bool gx2_bind_build_modulate(Gx2BoundShader *out,
                             const GX2AttribStream *attribs, uint32_t attrib_count) {
    if (!out) return false;

    static uint8_t vs_buf[512];
    static uint8_t ps_buf[512];

    ShaderGenVs vs_cfg = {.has_color = true, .has_texcoord = true};
    ShaderGenPs ps_cfg = {.sample_texture = true, .modulate_color = true};

    size_t vs_size = shader_gen_vs(vs_buf, sizeof(vs_buf), &vs_cfg);
    size_t ps_size = shader_gen_ps(ps_buf, sizeof(ps_buf), &ps_cfg);
    if (vs_size == 0 || ps_size == 0) return false;

    Gx2VsShape vshape;
    Gx2PsShape pshape;
    shader_gen_vs_shape(&vs_cfg, &vshape);
    shader_gen_ps_shape(&ps_cfg, &pshape);

    Gx2VsRegs vregs;
    Gx2PsRegs pregs;
    if (!gx2_vs_regs(&vregs, &vshape) || !gx2_ps_regs(&pregs, &pshape)) return false;

    return gx2_bind_build(out, vs_buf, vs_size, &vregs, ps_buf, ps_size, &pregs,
                          attribs, attrib_count, /*sampler_2d=*/true);
}

void gx2_bind_free(Gx2BoundShader *out) {
    if (!out) return;
    if (out->vs_program) { free(out->vs_program); out->vs_program = NULL; }
    if (out->ps_program) { free(out->ps_program); out->ps_program = NULL; }
    if (out->fs_program) { free(out->fs_program); out->fs_program = NULL; }
    out->valid = false;
}
