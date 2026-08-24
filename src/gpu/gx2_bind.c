// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu/gx2_bind.h"

#include <malloc.h>
#include <string.h>

#include <gx2/enum.h>
#include <gx2/mem.h>
#include <gx2/shaders.h>

#include "gpu/shader_gen.h"
#include "gpu/tev_shader_gen.h"

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

// Persistent sampler-var names.
static const char *const kSamplerNames[GX2_BIND_MAX_SAMPLERS] = {
    "s_tex0", "s_tex1", "s_tex2", "s_tex3",
    "s_tex4", "s_tex5", "s_tex6", "s_tex7",
};

static bool tev_uses_raster_color1(const TevConfig *cfg) {
    for (uint32_t s = 0; s < cfg->num_stages; ++s)
        if (cfg->stage[s].colorchan == GX_RAS_COLOR1)
            return true;
    return false;
}

static void setup_samplers(Gx2BoundShader *out, const uint8_t *locs,
                           uint32_t count) {
    if (count > GX2_BIND_MAX_SAMPLERS) count = GX2_BIND_MAX_SAMPLERS;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t loc = locs[i] & (GX2_BIND_MAX_SAMPLERS - 1);
        out->ps_samplers[i].name     = kSamplerNames[loc];
        out->ps_samplers[i].type     = GX2_SAMPLER_VAR_TYPE_SAMPLER_2D;
        out->ps_samplers[i].location = loc;
    }
    out->ps_sampler_count = count;
    if (count) {
        out->ps.samplerVars     = out->ps_samplers;
        out->ps.samplerVarCount = count;
    }
}

bool gx2_bind_build(Gx2BoundShader *out,
                    const uint8_t *vs_prog, size_t vs_size, const Gx2VsRegs *vs_regs,
                    const uint8_t *ps_prog, size_t ps_size, const Gx2PsRegs *ps_regs,
                    const GX2AttribStream *attribs, uint32_t attrib_count,
                    const uint8_t *sampler_locs, uint32_t sampler_count) {
    if (!out || !vs_prog || !ps_prog || !vs_regs || !ps_regs ||
        (attrib_count && !attribs) || (sampler_count && !sampler_locs)) {
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

    setup_samplers(out, sampler_locs, sampler_count);

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

    ShaderGenVs vs_cfg = {.has_color = true, .num_texcoords = 1};
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

    static const uint8_t modulate_loc[1] = {0};
    return gx2_bind_build(out, vs_buf, vs_size, &vregs, ps_buf, ps_size, &pregs,
                          attribs, attrib_count, modulate_loc, 1);
}

bool gx2_bind_build_tev(Gx2BoundShader *out, const TevConfig *cfg,
                        const GX2AttribStream *attribs, uint32_t attrib_count) {
    return gx2_bind_build_tev_ex(out, cfg, false, NULL, attribs, attrib_count);
}

bool gx2_bind_build_tev_ex(Gx2BoundShader *out, const TevConfig *cfg,
                           bool transform_position, const bool *texgen,
                           const GX2AttribStream *attribs, uint32_t attrib_count) {
    if (!out || !cfg) return false;

    static uint8_t ps_buf[8192];
    size_t ps_size = tev_shader_gen_ps(ps_buf, sizeof(ps_buf), cfg);
    if (ps_size == 0) return false;

    Gx2PsShape pshape;
    tev_shader_gen_ps_shape(cfg, &pshape);
    Gx2PsRegs pregs;
    if (!gx2_ps_regs(&pregs, &pshape)) return false;

    // One sampler per distinct texmap the config samples.
    uint8_t  locs[GX2_BIND_MAX_SAMPLERS];
    uint32_t nloc = 0;
    bool     seen_map[GX2_BIND_MAX_SAMPLERS] = {false};
    uint32_t ntexcoord = 0;
    bool     seen_coord[GX2_BIND_MAX_SAMPLERS] = {false};
    for (uint32_t s = 0; s < cfg->num_stages; ++s) {
        const TevStage *st = &cfg->stage[s];
        if (!st->tex_enable) continue;
        uint8_t m = st->texmap & (GX2_BIND_MAX_SAMPLERS - 1);
        if (!seen_map[m]) { seen_map[m] = true; locs[nloc++] = m; }
        uint8_t tc = st->texcoord & (GX2_BIND_MAX_SAMPLERS - 1);
        if (!seen_coord[tc]) { seen_coord[tc] = true; ++ntexcoord; }
    }

    // Generate the matching vertex shader.
    static uint8_t vs_buf[512];
    ShaderGenVs vs_cfg = {.has_color = true, .has_color1 = tev_uses_raster_color1(cfg),
                          .num_texcoords = ntexcoord, .transform_position = transform_position};
    if (texgen)
        for (uint32_t k = 0; k < ntexcoord && k < 8; ++k) vs_cfg.texgen[k] = texgen[k];
    size_t vs_size = shader_gen_vs(vs_buf, sizeof(vs_buf), &vs_cfg);
    if (vs_size == 0) return false;
    Gx2VsShape vshape;
    shader_gen_vs_shape(&vs_cfg, &vshape);
    Gx2VsRegs vregs;
    if (!gx2_vs_regs(&vregs, &vshape)) return false;

    return gx2_bind_build(out, vs_buf, vs_size, &vregs, ps_buf, ps_size, &pregs,
                          attribs, attrib_count, locs, nloc);
}

void gx2_bind_set_tev_uniforms(const TevConfig *cfg) {
    if (!cfg) return;
    float cfile[GX_TEV_PS_CFILE_COUNT][4];
    gx_tev_build_ps_cfile(cfg, cfile);
    GX2SetPixelUniformReg(0, GX_TEV_PS_CFILE_COUNT * 4, &cfile[0][0]);
}

void gx2_bind_free(Gx2BoundShader *out) {
    if (!out) return;
    if (out->vs_program) { free(out->vs_program); out->vs_program = NULL; }
    if (out->ps_program) { free(out->ps_program); out->ps_program = NULL; }
    if (out->fs_program) { free(out->fs_program); out->fs_program = NULL; }
    out->valid = false;
}
