// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu/shader_gen.h"

#include <string.h>

#include "gpu/r700_emit.h"
#include "gpu/tev_modulate_shader.h"

// Reserve 32 CF slots so clause bodies start at byte 0x100.
#define SHADER_GEN_MAX_CF 32

#define SHADER_GEN_MAX_PROGRAM_CHECK 1024

// 1.0f
#define F32_ONE 0x3f800000u

static const uint8_t kSelXYZW[4] = {R700_CHAN_X, R700_CHAN_Y, R700_CHAN_Z, R700_CHAN_W};
// A vec2 texcoord exports x,y and fills z,w with 0.
static const uint8_t kSelXY00[4] = {R700_CHAN_X, R700_CHAN_Y, R700_SEL_0, R700_SEL_0};
// A 2D texture sample takes coordinates (x, y, 0, x).
static const uint8_t kCoord2D[4] = {R700_SEL_X, R700_SEL_Y, R700_SEL_0, R700_SEL_X};

#define R700_EXPORT_POS0 60

// Lighting VS uniform (cfile) registers
#define LIGHT_NRM0  (SHADER_GEN_LIGHT_CFILE_BASE + 0)
#define LIGHT_NRM1  (SHADER_GEN_LIGHT_CFILE_BASE + 1)
#define LIGHT_NRM2  (SHADER_GEN_LIGHT_CFILE_BASE + 2)
#define LIGHT_MAT   (SHADER_GEN_LIGHT_CFILE_BASE + 3)
#define LIGHT_AMB   (SHADER_GEN_LIGHT_CFILE_BASE + 4)
#define LIGHT_DIR(j) (SHADER_GEN_LIGHT_CFILE_BASE + 5 + 2u * (j))
#define LIGHT_COL(j) (SHADER_GEN_LIGHT_CFILE_BASE + 6 + 2u * (j))

size_t shader_gen_vs(uint8_t *buf, size_t cap, const ShaderGenVs *cfg)
{
    if (!buf || !cfg) return 0;

    R700Program p;
    if (!r700_prog_begin(&p, buf, cap, SHADER_GEN_MAX_CF)) return 0;

    // CF[0]
    r700_prog_cf(&p, r700_cf(0, R700_CF_CALL_FS, 0, false, false, false));

    // ALU
    R700Inst64 alu[256];
    uint32_t n = 0;

    // PV = 1.0 * col3
    for (uint32_t c = 0; c < 4; ++c)
        alu[n++] = r700_alu_op2(R700_OP2_MUL, 0, c,
                                R700_ALU_SRC_LITERAL, c, false,
                                R700_ALU_SRC_CFILE(3), c, false,
                                false, false, c == 3);
    alu[n++] = r700_alu_literal(F32_ONE, F32_ONE);
    alu[n++] = r700_alu_literal(F32_ONE, F32_ONE);
    // PV = R1.z * col2 + PV
    if (cfg->transform_position)
        for (uint32_t c = 0; c < 4; ++c)
            alu[n++] = r700_alu_op3(R700_OP3_MULADD, 127, c,
                                    1, R700_CHAN_Z, false,
                                    R700_ALU_SRC_CFILE(2), c, false,
                                    R700_ALU_SRC_PV, c, false,
                                    false, c == 3);
    // PV = R1.y * col1 + PV
    for (uint32_t c = 0; c < 4; ++c)
        alu[n++] = r700_alu_op3(R700_OP3_MULADD, 127, c,
                                1, R700_CHAN_Y, false,
                                R700_ALU_SRC_CFILE(1), c, false,
                                R700_ALU_SRC_PV, c, false,
                                false, c == 3);
    // R1 = R1.x * col0 + PV
    for (uint32_t c = 0; c < 4; ++c)
        alu[n++] = r700_alu_op3(R700_OP3_MULADD, 1, c,
                                1, R700_CHAN_X, false,
                                R700_ALU_SRC_CFILE(0), c, false,
                                R700_ALU_SRC_PV, c, false,
                                false, c == 3);

    // Per-vertex diffuse lighting for colour channel 0.
    if (cfg->light.enable) {
        uint32_t norm_gpr = 2u + (cfg->has_color ? 1u : 0u);
        uint32_t in_count = 1u + (cfg->has_color ? 1u : 0u) +
                            (cfg->has_normal ? 1u : 0u) + cfg->num_texcoords;
        uint32_t ntN  = in_count + 1u;  // transformed normal scratch
        uint32_t lacc = in_count + 2u;  // light accumulator scratch

        for (uint32_t c = 0; c < 3; ++c)
            alu[n++] = r700_alu_op2(R700_OP2_MUL, 0, c,
                                    norm_gpr, R700_CHAN_X, false,
                                    R700_ALU_SRC_CFILE(LIGHT_NRM0), c, false,
                                    false, false, c == 2);
        for (uint32_t c = 0; c < 3; ++c)
            alu[n++] = r700_alu_op3(R700_OP3_MULADD, 127, c,
                                    norm_gpr, R700_CHAN_Y, false,
                                    R700_ALU_SRC_CFILE(LIGHT_NRM1), c, false,
                                    R700_ALU_SRC_PV, c, false,
                                    false, c == 2);
        for (uint32_t c = 0; c < 3; ++c)
            alu[n++] = r700_alu_op3(R700_OP3_MULADD, ntN, c,
                                    norm_gpr, R700_CHAN_Z, false,
                                    R700_ALU_SRC_CFILE(LIGHT_NRM2), c, false,
                                    R700_ALU_SRC_PV, c, false,
                                    false, c == 2);

        // lacc.rgb = ambient
        for (uint32_t c = 0; c < 3; ++c) {
            uint32_t asel = cfg->light.amb_from_vertex
                                ? 2u : R700_ALU_SRC_CFILE(LIGHT_AMB);
            alu[n++] = r700_alu_op2(R700_OP2_MOV, lacc, c,
                                    asel, c, false, R700_ALU_SRC_0, 0, false,
                                    false, true, c == 2);
        }

        // Accumulate each directional light
        for (uint32_t j = 0; j < cfg->light.num_lights; ++j) {
            if (cfg->light.diffuse) {
                alu[n++] = r700_alu_op2(R700_OP2_MUL, 0, R700_CHAN_X,
                                        ntN, R700_CHAN_X, false,
                                        R700_ALU_SRC_CFILE(LIGHT_DIR(j)), R700_CHAN_X, false,
                                        false, false, true);
                alu[n++] = r700_alu_op3(R700_OP3_MULADD, 127, R700_CHAN_X,
                                        ntN, R700_CHAN_Y, false,
                                        R700_ALU_SRC_CFILE(LIGHT_DIR(j)), R700_CHAN_Y, false,
                                        R700_ALU_SRC_PV, R700_CHAN_X, false,
                                        false, true);
                alu[n++] = r700_alu_op3(R700_OP3_MULADD, 127, R700_CHAN_X,
                                        ntN, R700_CHAN_Z, false,
                                        R700_ALU_SRC_CFILE(LIGHT_DIR(j)), R700_CHAN_Z, false,
                                        R700_ALU_SRC_PV, R700_CHAN_X, false,
                                        false, true);
                if (cfg->light.clamp)  // max(0, N.L)
                    alu[n++] = r700_alu_op2(R700_OP2_MAX, 0, R700_CHAN_X,
                                            R700_ALU_SRC_PV, R700_CHAN_X, false,
                                            R700_ALU_SRC_0, 0, false,
                                            false, false, true);
                for (uint32_t c = 0; c < 3; ++c)
                    alu[n++] = r700_alu_op3(R700_OP3_MULADD, lacc, c,
                                            R700_ALU_SRC_PV, R700_CHAN_X, false,
                                            R700_ALU_SRC_CFILE(LIGHT_COL(j)), c, false,
                                            lacc, c, false,
                                            false, c == 2);
            } else {
                for (uint32_t c = 0; c < 3; ++c)
                    alu[n++] = r700_alu_op2(R700_OP2_ADD, lacc, c,
                                            lacc, c, false,
                                            R700_ALU_SRC_CFILE(LIGHT_COL(j)), c, false,
                                            false, true, c == 2);
            }
        }

        for (uint32_t c = 0; c < 3; ++c)
            alu[n++] = r700_alu_op2(R700_OP2_MAX, lacc, c,
                                    lacc, c, false, R700_ALU_SRC_0, 0, false,
                                    false, true, c == 2);
        for (uint32_t c = 0; c < 3; ++c)
            alu[n++] = r700_alu_op2(R700_OP2_MIN, lacc, c,
                                    lacc, c, false, R700_ALU_SRC_1, 0, false,
                                    false, true, c == 2);

        for (uint32_t c = 0; c < 3; ++c) {
            uint32_t msel = cfg->light.mat_from_vertex
                                ? 2u : R700_ALU_SRC_CFILE(LIGHT_MAT);
            alu[n++] = r700_alu_op2(R700_OP2_MUL, 2, c,
                                    msel, c, false, lacc, c, false,
                                    false, true, c == 2);
        }
    }

    // Regular matrix texgen
    for (uint32_t k = 0; k < cfg->num_texcoords; ++k) {
        if (!cfg->texgen[k]) continue;
        uint32_t g = 2u + (cfg->has_color ? 1u : 0u) +
                     (cfg->has_normal ? 1u : 0u) + k;
        uint32_t b = 4u + 3u * k;
        // PV = coord.x * row0
        for (uint32_t c = 0; c < 3; ++c)
            alu[n++] = r700_alu_op2(R700_OP2_MUL, 0, c,
                                    g, R700_CHAN_X, false,
                                    R700_ALU_SRC_CFILE(b + 0), c, false,
                                    false, false, c == 2);
        // PV = coord.y * row1 + PV
        for (uint32_t c = 0; c < 3; ++c)
            alu[n++] = r700_alu_op3(R700_OP3_MULADD, 127, c,
                                    g, R700_CHAN_Y, false,
                                    R700_ALU_SRC_CFILE(b + 1), c, false,
                                    R700_ALU_SRC_PV, c, false,
                                    false, c == 2);
        // Rg = coord.w(=1) * row2 + PV
        for (uint32_t c = 0; c < 3; ++c)
            alu[n++] = r700_alu_op3(R700_OP3_MULADD, g, c,
                                    g, R700_CHAN_W, false,
                                    R700_ALU_SRC_CFILE(b + 2), c, false,
                                    R700_ALU_SRC_PV, c, false,
                                    false, c == 2);
    }

    if (n > 128) return 0;
    if (!r700_prog_alu_clause(&p, alu, n, true)) return 0;

    uint32_t nparam = (cfg->has_color ? 1u : 0u) + cfg->num_texcoords;
    r700_prog_cf(&p, r700_cf_export(R700_EXPORT_POS, R700_EXPORT_POS0, 1, kSelXYZW,
                                    nparam == 0, true, R700_CF_EXPORT_DONE));

    uint32_t tc_gpr0 = 2u + (cfg->has_color ? 1u : 0u) + (cfg->has_normal ? 1u : 0u);
    uint32_t base = 0, emitted = 0;
    if (cfg->has_color) {
        bool last = emitted + 1 == nparam;
        r700_prog_cf(&p, r700_cf_export(R700_EXPORT_PARAM, base, 2, kSelXYZW,
                                        last, false,
                                        last ? R700_CF_EXPORT_DONE : R700_CF_EXPORT));
        ++base;
        ++emitted;
    }
    for (uint32_t k = 0; k < cfg->num_texcoords; ++k) {
        bool last = emitted + 1 == nparam;
        r700_prog_cf(&p, r700_cf_export(R700_EXPORT_PARAM, base, tc_gpr0 + k, kSelXY00,
                                        last, false,
                                        last ? R700_CF_EXPORT_DONE : R700_CF_EXPORT));
        ++base;
        ++emitted;
    }

    return r700_prog_finalize(&p);
}

size_t shader_gen_ps(uint8_t *buf, size_t cap, const ShaderGenPs *cfg)
{
    if (!buf || !cfg) return 0;

    R700Program p;
    if (!r700_prog_begin(&p, buf, cap, SHADER_GEN_MAX_CF)) return 0;

    // Interpolant GPRs match the VS param order: colour -> R0, texcoord -> R1.
    uint32_t tex_gpr = cfg->modulate_color ? 1u : 0u;

    if (cfg->sample_texture && cfg->modulate_color) {
        // R0 = R0(colour) * R1(sampled texture)
        R700Inst64 mul[4];
        for (uint32_t c = 0; c < 4; ++c)
            mul[c] = r700_alu_op2(R700_OP2_MUL, 0, c,
                                  0, c, false, 1, c, false,
                                  false, true, c == 3);
        // Lay the ALU body ahead of the TEX body in memory
        uint32_t alu_addr = r700_prog_alu_body(&p, mul, 4);
        R700Inst128 tex = r700_tex_sample(0, 0, tex_gpr, tex_gpr, kCoord2D, kSelXYZW);
        uint32_t tex_addr = r700_prog_tex_body(&p, &tex, 1);
        if (alu_addr == UINT32_MAX || tex_addr == UINT32_MAX) return 0;

        r700_prog_cf(&p, r700_cf(tex_addr, R700_CF_TEX, 1, true, false, true));
        r700_prog_cf(&p, r700_cf_alu(alu_addr, 4, true));
        r700_prog_cf(&p, r700_cf_export(R700_EXPORT_PIXEL, 0, 0, kSelXYZW,
                                        true, true, R700_CF_EXPORT_DONE));
    } else if (cfg->sample_texture) {
        // Sample straight into R0 and export it.
        R700Inst128 tex = r700_tex_sample(0, 0, 0, 0, kCoord2D, kSelXYZW);
        if (!r700_prog_tex_clause(&p, &tex, 1, true, true)) return 0;
        r700_prog_cf(&p, r700_cf_export(R700_EXPORT_PIXEL, 0, 0, kSelXYZW,
                                        true, true, R700_CF_EXPORT_DONE));
    } else {
        // Pass the interpolated colour straight through.
        r700_prog_cf(&p, r700_cf_export(R700_EXPORT_PIXEL, 0, 0, kSelXYZW,
                                        true, true, R700_CF_EXPORT_DONE));
    }

    return r700_prog_finalize(&p);
}

void shader_gen_vs_shape(const ShaderGenVs *cfg, Gx2VsShape *out)
{
    memset(out, 0, sizeof(*out));
    uint32_t inputs = 1;  // position
    out->input_semantics[0] = 0;
    if (cfg->has_color)  out->input_semantics[inputs] = (uint8_t)inputs, ++inputs;
    if (cfg->has_normal) out->input_semantics[inputs] = (uint8_t)inputs, ++inputs;
    for (uint32_t k = 0; k < cfg->num_texcoords; ++k)
        out->input_semantics[inputs] = (uint8_t)inputs, ++inputs;
    out->num_inputs = inputs;
    out->num_gprs = inputs + 1 + (cfg->light.enable ? 2u : 0u);
    out->stack_size = 1;

    // Param semantics are consecutive so they line up with the pixel shader's
    // interpolant contract.
    uint32_t exports = 0;
    if (cfg->has_color) out->export_semantics[exports++] = 0;
    for (uint32_t k = 0; k < cfg->num_texcoords; ++k)
        out->export_semantics[exports++] = (uint8_t)((cfg->has_color ? 1u : 0u) + k);
    out->num_exports = exports;
}

void shader_gen_ps_shape(const ShaderGenPs *cfg, Gx2PsShape *out)
{
    memset(out, 0, sizeof(*out));
    uint32_t inputs = 0;
    if (cfg->modulate_color) out->input_semantics[inputs++] = 0;
    if (cfg->sample_texture) out->input_semantics[inputs++] = 1;
    out->num_inputs = inputs;
    out->num_gprs = inputs ? inputs : 1;
    out->stack_size = 0;
    out->num_color_exports = 1;
}

// Self test

static uint32_t rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t rd_le32(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

// Locate a GFD block's data by type in an embedded .gsh.
static const uint8_t *gfd_block(const uint8_t *gsh, uint32_t len, uint32_t type,
                                uint32_t *out_size)
{
    if (len < 8 || memcmp(gsh, "Gfx2", 4) != 0) return NULL;
    uint32_t off = rd_be32(gsh + 4);  // file header size
    while (off + 32 <= len) {
        if (memcmp(gsh + off, "BLK{", 4) != 0) return NULL;
        uint32_t hs = rd_be32(gsh + off + 4);
        uint32_t t  = rd_be32(gsh + off + 16);
        uint32_t ds = rd_be32(gsh + off + 20);
        if (t == type) {
            if ((size_t)off + hs + ds > len) return NULL;
            *out_size = ds;
            return gsh + off + hs;
        }
        off += hs + ds;
    }
    return NULL;
}

static void put_word(uint8_t *out, size_t *olen, uint32_t w)
{
    out[(*olen)++] = (uint8_t)w;
    out[(*olen)++] = (uint8_t)(w >> 8);
    out[(*olen)++] = (uint8_t)(w >> 16);
    out[(*olen)++] = (uint8_t)(w >> 24);
}

// Canonicalise a program
static size_t canon(const uint8_t *prog, size_t size, uint8_t *out, size_t outcap)
{
    const uint8_t *body[R700_PROG_MAX_CLAUSE];
    uint32_t       blen[R700_PROG_MAX_CLAUSE];
    uint32_t       nb = 0;
    size_t         olen = 0;

    for (size_t i = 0;; ++i) {
        if ((i + 1) * 8 > size) return 0;
        uint32_t w0 = rd_le32(prog + i * 8);
        uint32_t w1 = rd_le32(prog + i * 8 + 4);
        uint32_t nw0 = w0;

        if (w1 & (1u << 29)) {  // CF_ALU
            uint32_t addr = w0 & 0xFFFFFFu;
            uint32_t slots = ((w1 >> 18) & 0x7Fu) + 1u;
            if (nb >= R700_PROG_MAX_CLAUSE) return 0;
            body[nb] = prog + (size_t)addr * 8;
            blen[nb] = slots * 8u;
            ++nb;
            nw0 = w0 & ~0xFFFFFFu;
        } else if (((w1 >> 23) & 0x7Fu) == R700_CF_TEX) {
            uint32_t addr = w0 & 0xFFFFFFu;
            uint32_t count = (((w1 >> 10) & 7u) | (((w1 >> 19) & 1u) << 3)) + 1u;
            if (nb >= R700_PROG_MAX_CLAUSE) return 0;
            body[nb] = prog + (size_t)addr * 8;
            blen[nb] = count * 16u;
            ++nb;
            nw0 = w0 & ~0xFFFFFFu;
        }

        if (olen + 8 > outcap) return 0;
        put_word(out, &olen, nw0);
        put_word(out, &olen, w1);

        if (w1 & (1u << 21)) break;  // END_OF_PROGRAM
    }

    for (uint32_t b = 0; b < nb; ++b) {
        if (body[b] + blen[b] > prog + size) return 0;
        if (olen + blen[b] > outcap) return 0;
        memcpy(out + olen, body[b], blen[b]);
        olen += blen[b];
    }
    return olen;
}

static bool equivalent(const uint8_t *golden, uint32_t gsize,
                       const uint8_t *gen, size_t gensize)
{
    uint8_t ca[1024], cb[1024];
    size_t na = canon(golden, gsize, ca, sizeof(ca));
    size_t nb = canon(gen, gensize, cb, sizeof(cb));
    return na != 0 && na == nb && memcmp(ca, cb, na) == 0;
}

// Count PARAM exports in a finalized program.
static uint32_t count_param_exports(const uint8_t *prog, size_t size)
{
    uint32_t n = 0;
    for (size_t i = 0;; ++i) {
        if ((i + 1) * 8 > size) return n;
        uint32_t w0 = rd_le32(prog + i * 8);
        uint32_t w1 = rd_le32(prog + i * 8 + 4);
        if (w1 & (1u << 29)) continue;  // CF_ALU
        uint32_t inst = (w1 >> 23) & 0x7Fu;
        if ((inst == R700_CF_EXPORT || inst == R700_CF_EXPORT_DONE) &&
            ((w0 >> 13) & 3u) == R700_EXPORT_PARAM)
            ++n;
        if (w1 & (1u << 21)) return n;  // END_OF_PROGRAM
    }
}

bool shader_gen_selftest(void)
{
    const uint8_t *gsh = g_tevModulateShaderGsh;
    uint32_t glen = g_tevModulateShaderGshLen;

    // VS program is block type 5, PS program is type 7.
    uint32_t gvs_size = 0, gps_size = 0;
    const uint8_t *gvs = gfd_block(gsh, glen, 5, &gvs_size);
    const uint8_t *gps = gfd_block(gsh, glen, 7, &gps_size);
    if (!gvs || !gps) return false;

    static uint8_t vs_buf[SHADER_GEN_MAX_PROGRAM_CHECK];
    static uint8_t ps_buf[SHADER_GEN_MAX_PROGRAM_CHECK];

    ShaderGenVs vs_cfg = {.has_color = true, .num_texcoords = 1};
    ShaderGenPs ps_cfg = {.sample_texture = true, .modulate_color = true};

    size_t vs_size = shader_gen_vs(vs_buf, sizeof(vs_buf), &vs_cfg);
    size_t ps_size = shader_gen_ps(ps_buf, sizeof(ps_buf), &ps_cfg);
    if (vs_size == 0 || ps_size == 0) return false;

    if (vs_size != gvs_size || memcmp(vs_buf, gvs, vs_size) != 0) return false;

    if (!equivalent(gps, gps_size, ps_buf, ps_size)) return false;

    // The matching register shapes must build valid GX2 register state.
    Gx2VsShape vshape;
    Gx2PsShape pshape;
    shader_gen_vs_shape(&vs_cfg, &vshape);
    shader_gen_ps_shape(&ps_cfg, &pshape);
    Gx2VsRegs vregs;
    Gx2PsRegs pregs;
    if (!gx2_vs_regs(&vregs, &vshape) || !gx2_ps_regs(&pregs, &pshape))
        return false;
    if (vshape.num_gprs != 4 || vshape.num_inputs != 3 || vshape.num_exports != 2)
        return false;
    if (pshape.num_gprs != 2 || pshape.num_inputs != 2)
        return false;

    // A multi-texcoord passthrough VS exports colour + one param per texcoord,
    // with consecutive semantics that line up with the TEV PS interpolants.
    ShaderGenVs multi_cfg = {.has_color = true, .num_texcoords = 3};
    size_t multi_size = shader_gen_vs(vs_buf, sizeof(vs_buf), &multi_cfg);
    if (multi_size == 0) return false;
    if (count_param_exports(vs_buf, multi_size) != 4) return false;

    Gx2VsShape multi_shape;
    shader_gen_vs_shape(&multi_cfg, &multi_shape);
    if (multi_shape.num_exports != 4 || multi_shape.num_inputs != 5) return false;
    if (multi_shape.export_semantics[0] != 0 || multi_shape.export_semantics[1] != 1 ||
        multi_shape.export_semantics[2] != 2 || multi_shape.export_semantics[3] != 3)
        return false;
    Gx2VsRegs multi_regs;
    if (!gx2_vs_regs(&multi_regs, &multi_shape)) return false;

    ShaderGenVs xf_cfg = {.has_color = true, .num_texcoords = 1,
                          .transform_position = true};
    size_t xf_size = shader_gen_vs(vs_buf, sizeof(vs_buf), &xf_cfg);
    if (xf_size == 0 || xf_size != gvs_size + 32) return false;
    if (count_param_exports(vs_buf, xf_size) != 2) return false;

    Gx2VsShape xf_shape;
    shader_gen_vs_shape(&xf_cfg, &xf_shape);
    if (xf_shape.num_inputs != 3 || xf_shape.num_exports != 2) return false;
    Gx2VsRegs xf_regs;
    if (!gx2_vs_regs(&xf_regs, &xf_shape)) return false;

    ShaderGenVs tg_cfg = {.has_color = true, .num_texcoords = 1, .texgen = {true}};
    size_t tg_size = shader_gen_vs(vs_buf, sizeof(vs_buf), &tg_cfg);
    if (tg_size == 0 || tg_size != gvs_size + 72) return false;
    if (count_param_exports(vs_buf, tg_size) != 2) return false;

    Gx2VsShape tg_shape;
    shader_gen_vs_shape(&tg_cfg, &tg_shape);
    if (tg_shape.num_inputs != 3 || tg_shape.num_exports != 2) return false;
    Gx2VsRegs tg_regs;
    if (!gx2_vs_regs(&tg_regs, &tg_shape)) return false;

    ShaderGenVs tg2_cfg = {.has_color = true, .num_texcoords = 2,
                           .texgen = {false, true}};
    size_t tg2_size = shader_gen_vs(vs_buf, sizeof(vs_buf), &tg2_cfg);
    if (tg2_size == 0) return false;
    ShaderGenVs pass2_cfg = {.has_color = true, .num_texcoords = 2};
    size_t pass2_size = shader_gen_vs(vs_buf, sizeof(vs_buf), &pass2_cfg);
    if (pass2_size == 0 || tg2_size != pass2_size + 72) return false;

    // Per-vertex diffuse lighting VS
    ShaderGenVs lit_cfg = {.has_color = true, .has_normal = true,
                           .transform_position = true, .num_texcoords = 0,
                           .light = {.enable = true, .num_lights = 1,
                                     .diffuse = true, .clamp = true}};
    size_t lit_size = shader_gen_vs(vs_buf, sizeof(vs_buf), &lit_cfg);
    if (lit_size == 0) return false;
    if (count_param_exports(vs_buf, lit_size) != 1) return false;

    Gx2VsShape lit_shape;
    shader_gen_vs_shape(&lit_cfg, &lit_shape);
    if (lit_shape.num_inputs != 3 || lit_shape.num_exports != 1 ||
        lit_shape.num_gprs != 6)
        return false;
    Gx2VsRegs lit_regs;
    if (!gx2_vs_regs(&lit_regs, &lit_shape)) return false;

    ShaderGenVs lit2_cfg = lit_cfg;
    lit2_cfg.light.num_lights = 2;
    size_t lit2_size = shader_gen_vs(vs_buf, sizeof(vs_buf), &lit2_cfg);
    if (lit2_size == 0 || lit2_size != lit_size + 7 * 8) return false;

    return true;
}
