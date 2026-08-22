// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu/gx_xf.h"

#include <string.h>

static inline uint32_t bits(uint32_t v, unsigned lo, unsigned n) {
    return (v >> lo) & ((1u << n) - 1u);
}

static inline uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

// Reinterpret a guest 32-bit word as a float.
static inline float u32_to_f(uint32_t u) {
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

static void decode_texmtx(XfTexMtxInfo *tm, uint32_t v) {
    tm->projection    = (uint8_t)bits(v, 1, 1);
    tm->inputform     = (uint8_t)bits(v, 2, 1);
    tm->texgentype    = (uint8_t)bits(v, 4, 3);
    tm->sourcerow     = (uint8_t)bits(v, 7, 5);
    tm->emboss_source = (uint8_t)bits(v, 12, 3);
    tm->emboss_light  = (uint8_t)bits(v, 15, 3);
}

void gx_xf_reset(XfConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->projection_type = GX_XF_PERSPECTIVE;
    cfg->num_color_chans = 0;
    cfg->num_texgens     = 0;
    // Position matrix 0 defaults to identity.
    cfg->pos_matrices[0] = 1.0f;
    cfg->pos_matrices[5] = 1.0f;
    cfg->pos_matrices[10] = 1.0f;
}

// Fold a single XF register write into the config.
static void write_reg(XfConfig *cfg, uint16_t addr, uint32_t word) {
    if (addr < XF_POSMATRICES_END) {
        cfg->pos_matrices[addr] = u32_to_f(word);
        return;
    }
    if (addr >= XF_NORMALMATRICES && addr < XF_NORMALMATRICES_END) {
        cfg->normal_matrices[addr - XF_NORMALMATRICES] = u32_to_f(word);
        return;
    }
    if (addr >= XF_POSTMATRICES && addr < XF_POSTMATRICES_END) {
        cfg->post_matrices[addr - XF_POSTMATRICES] = u32_to_f(word);
        return;
    }
    if (addr >= XF_SETVIEWPORT && addr < XF_SETVIEWPORT + 6) {
        cfg->viewport[addr - XF_SETVIEWPORT] = u32_to_f(word);
        return;
    }
    if (addr >= XF_SETPROJECTION && addr < XF_SETPROJECTION + 6) {
        cfg->projection[addr - XF_SETPROJECTION] = u32_to_f(word);
        return;
    }
    switch (addr) {
        case XF_SETPROJECTION + 6:  // 0x1026 projection type
            cfg->projection_type = word & 1u;
            return;
        case XF_SETNUMCHAN:
            cfg->num_color_chans = bits(word, 0, 2);
            return;
        case XF_SETNUMTEXGENS:
            cfg->num_texgens = bits(word, 0, 4);
            return;
        default:
            break;
    }
    if (addr >= XF_SETTEXMTXINFO && addr < XF_SETTEXMTXINFO + GX_XF_MAX_TEXGENS) {
        decode_texmtx(&cfg->texmtx[addr - XF_SETTEXMTXINFO], word);
        return;
    }
}

void gx_xf_apply_xf(XfConfig *cfg, uint16_t address, uint8_t count, const uint8_t *data) {
    for (uint8_t i = 0; i < count; ++i) {
        uint16_t addr = (uint16_t)(address + i);
        if (addr >= XF_REGISTERS_END) break;
        write_reg(cfg, addr, rd_be32(data + (size_t)i * 4));
    }
}

void gx_xf_projection_matrix(const XfConfig *cfg, float out[16]) {
    const float *p = cfg->projection;
    memset(out, 0, sizeof(float) * 16);
    if (cfg->projection_type == GX_XF_PERSPECTIVE) {
        out[0]  = p[0];  out[2]  = p[1];
        out[5]  = p[2];  out[6]  = p[3];
        out[10] = p[4];  out[11] = p[5];
        out[14] = -1.0f;
    } else {  // orthographic
        out[0]  = p[0];  out[3]  = p[1];
        out[5]  = p[2];  out[7]  = p[3];
        out[10] = p[4];  out[11] = p[5];
        out[15] = 1.0f;
    }
}

void gx_xf_position_matrix(const XfConfig *cfg, uint32_t mtx_index, float out[16]) {
    uint32_t base = mtx_index * 4u;
    memset(out, 0, sizeof(float) * 16);
    if (base + 12u <= 256u) {
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                out[r * 4 + c] = cfg->pos_matrices[base + r * 4 + c];
    }
    out[15] = 1.0f;  // last row 0 0 0 1
}

// Self test
static void put_f(uint8_t *p, float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    p[0] = (uint8_t)(u >> 24); p[1] = (uint8_t)(u >> 16);
    p[2] = (uint8_t)(u >> 8);  p[3] = (uint8_t)u;
}

static void put_u(uint8_t *p, uint32_t u) {
    p[0] = (uint8_t)(u >> 24); p[1] = (uint8_t)(u >> 16);
    p[2] = (uint8_t)(u >> 8);  p[3] = (uint8_t)u;
}

int gx_xf_selftest(void) {
    XfConfig cfg;
    gx_xf_reset(&cfg);

    float m[16];
    gx_xf_position_matrix(&cfg, 0, m);
    if (m[0] != 1.0f || m[5] != 1.0f || m[10] != 1.0f || m[15] != 1.0f) return 0;
    if (m[1] != 0.0f || m[3] != 0.0f || m[12] != 0.0f) return 0;

    // Orthographic projection
    {
        uint8_t d[7 * 4];
        float raw[6] = {2.0f, -1.0f, 3.0f, 0.5f, -0.25f, 7.0f};
        for (int i = 0; i < 6; ++i) put_f(&d[i * 4], raw[i]);
        put_u(&d[6 * 4], GX_XF_ORTHOGRAPHIC);
        gx_xf_apply_xf(&cfg, XF_SETPROJECTION, 7, d);
        if (cfg.projection_type != GX_XF_ORTHOGRAPHIC) return 0;

        float proj[16];
        gx_xf_projection_matrix(&cfg, proj);
        if (proj[0] != 2.0f || proj[3] != -1.0f) return 0;
        if (proj[5] != 3.0f || proj[7] != 0.5f) return 0;
        if (proj[10] != -0.25f || proj[11] != 7.0f) return 0;
        if (proj[15] != 1.0f || proj[14] != 0.0f) return 0;
        if (proj[1] != 0.0f || proj[2] != 0.0f) return 0;
    }

    // Perspective projection reuses the same coefficient slots but a different
    // matrix layout.
    {
        uint8_t d[7 * 4];
        float raw[6] = {1.5f, 0.1f, 1.25f, 0.2f, -1.001f, -0.5f};
        for (int i = 0; i < 6; ++i) put_f(&d[i * 4], raw[i]);
        put_u(&d[6 * 4], GX_XF_PERSPECTIVE);
        gx_xf_apply_xf(&cfg, XF_SETPROJECTION, 7, d);

        float proj[16];
        gx_xf_projection_matrix(&cfg, proj);
        if (proj[0] != 1.5f || proj[2] != 0.1f) return 0;
        if (proj[5] != 1.25f || proj[6] != 0.2f) return 0;
        if (proj[10] != -1.001f || proj[11] != -0.5f) return 0;
        if (proj[14] != -1.0f || proj[15] != 0.0f) return 0;
        if (proj[3] != 0.0f || proj[7] != 0.0f) return 0;
    }

    // Load a distinct 3x4 position matrix at index 2.
    {
        uint8_t d[12 * 4];
        float pm[12] = {
            1, 0, 0, 10,
            0, 1, 0, 20,
            0, 0, 1, 30,
        };
        for (int i = 0; i < 12; ++i) put_f(&d[i * 4], pm[i]);
        gx_xf_apply_xf(&cfg, XF_POSMATRICES + 8, 12, d);

        float pos[16];
        gx_xf_position_matrix(&cfg, 2, pos);
        if (pos[3] != 10.0f || pos[7] != 20.0f || pos[11] != 30.0f) return 0;
        if (pos[0] != 1.0f || pos[5] != 1.0f || pos[10] != 1.0f) return 0;
        if (pos[12] != 0.0f || pos[13] != 0.0f || pos[14] != 0.0f || pos[15] != 1.0f)
            return 0;

        float pos0[16];
        gx_xf_position_matrix(&cfg, 0, pos0);
        if (pos0[3] != 0.0f || pos0[0] != 1.0f) return 0;
    }

    // Viewport
    {
        uint8_t d[6 * 4];
        float vp[6] = {320.0f, -240.0f, 16777215.0f, 342.0f, 342.0f, 16777215.0f};
        for (int i = 0; i < 6; ++i) put_f(&d[i * 4], vp[i]);
        gx_xf_apply_xf(&cfg, XF_SETVIEWPORT, 6, d);
        if (cfg.viewport[0] != 320.0f || cfg.viewport[1] != -240.0f) return 0;
        if (cfg.viewport[3] != 342.0f) return 0;
    }

    // numTexGens and a texgen matrix info register.
    {
        uint8_t d[4];
        put_u(d, 2);
        gx_xf_apply_xf(&cfg, XF_SETNUMTEXGENS, 1, d);
        if (cfg.num_texgens != 2) return 0;

        // TexMtxInfo: projection=STQ(1), inputform=ABC1(1), texgentype=COLOR0(2),
        // sourcerow=Colors(2).
        uint32_t tmi = (uint32_t)1u << 1 |   // projection
                       (uint32_t)1u << 2 |   // inputform
                       (uint32_t)GX_XF_TG_COLOR0 << 4 |
                       (uint32_t)2u << 7;     // sourcerow
        put_u(d, tmi);
        gx_xf_apply_xf(&cfg, XF_SETTEXMTXINFO + 1, 1, d);
        const XfTexMtxInfo *tm = &cfg.texmtx[1];
        if (tm->projection != GX_XF_TEX_STQ || tm->inputform != 1) return 0;
        if (tm->texgentype != GX_XF_TG_COLOR0 || tm->sourcerow != 2) return 0;
        // texmtx[0] untouched.
        if (cfg.texmtx[0].texgentype != 0 || cfg.texmtx[0].sourcerow != 0) return 0;
    }

    // numChan
    {
        uint8_t d[4];
        put_u(d, 1);
        gx_xf_apply_xf(&cfg, XF_SETNUMCHAN, 1, d);
        if (cfg.num_color_chans != 1) return 0;
    }

    return 1;
}
