// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu/gx_xf.h"

#include <math.h>
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

// LitChannel bit layout (Dolphin XFMemory.h)
static void decode_litchan(XfLitChannel *lc, uint32_t v) {
    lc->matsource      = (uint8_t)bits(v, 0, 1);
    lc->enablelighting = (uint8_t)bits(v, 1, 1);
    lc->ambsource      = (uint8_t)bits(v, 6, 1);
    lc->diffusefunc    = (uint8_t)bits(v, 7, 2);
    lc->attnfunc       = (uint8_t)bits(v, 9, 2);
    uint32_t m0_3 = bits(v, 2, 4);
    uint32_t m4_7 = bits(v, 11, 4);
    lc->light_mask = lc->enablelighting ? (uint8_t)(m0_3 | (m4_7 << 4)) : 0;
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
    if (addr >= XF_LIGHTS && addr < XF_LIGHTS_END) {
        uint32_t rel = (uint32_t)addr - XF_LIGHTS;
        XfLight *L = &cfg->lights[rel >> 4];
        uint32_t off = rel & 0x0Fu;
        if (off == 3)                    L->color = word;
        else if (off >= 4 && off <= 6)   L->cosatt[off - 4]  = u32_to_f(word);
        else if (off >= 7 && off <= 9)   L->distatt[off - 7] = u32_to_f(word);
        else if (off >= 10 && off <= 12) L->pos[off - 10]    = u32_to_f(word);
        else if (off >= 13 && off <= 15) L->dir[off - 13]    = u32_to_f(word);
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
        case XF_SETCHAN0_AMBCOLOR: cfg->amb_color[0] = word; return;
        case XF_SETCHAN1_AMBCOLOR: cfg->amb_color[1] = word; return;
        case XF_SETCHAN0_MATCOLOR: cfg->mat_color[0] = word; return;
        case XF_SETCHAN1_MATCOLOR: cfg->mat_color[1] = word; return;
        case XF_SETCHAN0_COLOR: decode_litchan(&cfg->color_chan[0], word); return;
        case XF_SETCHAN1_COLOR: decode_litchan(&cfg->color_chan[1], word); return;
        case XF_SETCHAN0_ALPHA: decode_litchan(&cfg->alpha_chan[0], word); return;
        case XF_SETCHAN1_ALPHA: decode_litchan(&cfg->alpha_chan[1], word); return;
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

// out = a * b
static void mat4_mul(const float a[16], const float b[16], float out[16]) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k)
                s += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = s;
        }
}

void gx_xf_build_vs_cfile(const XfConfig *cfg, uint32_t mtx_index, float out[16]) {
    float proj[16], pos[16], clip[16];
    gx_xf_projection_matrix(cfg, proj);
    gx_xf_position_matrix(cfg, mtx_index, pos);
    mat4_mul(proj, pos, clip);  // clip = P * M

    for (int i = 0; i < 4; ++i)
        for (int c = 0; c < 4; ++c)
            out[i * 4 + c] = clip[c * 4 + i];
}

// SourceRow (Dolphin XFMemory.h)
enum { GX_XF_SRCROW_TEX0 = 5 };

bool gx_xf_texgen_is_regular(const XfConfig *cfg, uint32_t tc) {
    if (tc >= GX_XF_MAX_TEXGENS) return false;
    const XfTexMtxInfo *tm = &cfg->texmtx[tc];
    return tm->texgentype == GX_XF_TG_REGULAR &&
           tm->sourcerow == (uint8_t)(GX_XF_SRCROW_TEX0 + tc);
}

void gx_xf_build_texmtx_cfile(const XfConfig *cfg, uint32_t texmtx_index, bool stq,
                              float out[12]) {
    uint32_t base = texmtx_index * 4u;
    float m0[4] = {0}, m1[4] = {0}, m2[4] = {0};
    if (base + 12u <= 256u) {
        for (int c = 0; c < 4; ++c) {
            m0[c] = cfg->pos_matrices[base + 0 + c];
            m1[c] = cfg->pos_matrices[base + 4 + c];
            m2[c] = cfg->pos_matrices[base + 8 + c];
        }
    }
    // row0 = coord.x coefficients, row1 = coord.y, row2 = folded constant term
    // (columns 2 and 3, since coord.z = coord.w = 1).
    out[0] = m0[0];            out[1] = m1[0];            out[2]  = stq ? m2[0] : 0.0f;            out[3]  = 0.0f;
    out[4] = m0[1];            out[5] = m1[1];            out[6]  = stq ? m2[1] : 0.0f;            out[7]  = 0.0f;
    out[8] = m0[2] + m0[3];    out[9] = m1[2] + m1[3];    out[10] = stq ? (m2[2] + m2[3]) : 1.0f;  out[11] = 0.0f;
}

static uint8_t popcount8(uint8_t v) {
    uint8_t n = 0;
    while (v) { n += (uint8_t)(v & 1u); v >>= 1; }
    return n;
}

bool gx_xf_lighting_desc(const XfConfig *cfg, uint32_t chan, XfLightDesc *out) {
    memset(out, 0, sizeof(*out));
    if (chan >= 2) return false;
    const XfLitChannel *cc = &cfg->color_chan[chan];
    out->enable = cc->enablelighting != 0;
    if (!out->enable) return true;
    out->num_lights      = popcount8(cc->light_mask);
    out->diffuse         = cc->diffusefunc != GX_XF_DF_NONE;
    out->clamp           = cc->diffusefunc == GX_XF_DF_CLAMP;
    out->mat_from_vertex = cc->matsource == GX_XF_MATSRC_VTX;
    out->amb_from_vertex = cc->ambsource == GX_XF_AMBSRC_VTX;
    return true;
}

static void rgba8_to_f(uint32_t w, float out[4]) {
    out[0] = (float)((w >> 24) & 0xFFu) / 255.0f;
    out[1] = (float)((w >> 16) & 0xFFu) / 255.0f;
    out[2] = (float)((w >> 8)  & 0xFFu) / 255.0f;
    out[3] = (float)(w & 0xFFu) / 255.0f;
}

int gx_xf_build_light_cfile(const XfConfig *cfg, uint32_t chan, uint32_t pos_mtx_index,
                            float *out, int cap) {
    if (chan >= 2) return 0;
    const XfLitChannel *cc = &cfg->color_chan[chan];
    if (!cc->enablelighting) return 0;
    uint8_t nlights = popcount8(cc->light_mask);
    if (nlights > GX_XF_NUM_LIGHTS) nlights = GX_XF_NUM_LIGHTS;

    int regs = 5 + 2 * (int)nlights;
    if (cap < regs * 4) return 0;
    memset(out, 0, (size_t)regs * 4 * sizeof(float));

    float pos[16];
    gx_xf_position_matrix(cfg, pos_mtx_index, pos);
    for (int i = 0; i < 3; ++i)          // input component
        for (int c = 0; c < 3; ++c)      // output channel
            out[i * 4 + c] = pos[c * 4 + i];

    // reg 3
    rgba8_to_f(cfg->mat_color[chan], &out[3 * 4]);
    // reg 4
    rgba8_to_f(cfg->amb_color[chan], &out[4 * 4]);

    // regs 5+2j / 6+2j
    int j = 0;
    for (int li = 0; li < GX_XF_NUM_LIGHTS && j < nlights; ++li) {
        if (!(cc->light_mask & (1u << li))) continue;
        const XfLight *L = &cfg->lights[li];
        float *dir = &out[(5 + 2 * j) * 4];
        float len = sqrtf(L->pos[0] * L->pos[0] + L->pos[1] * L->pos[1] +
                          L->pos[2] * L->pos[2]);
        if (len > 1e-8f) {
            dir[0] = L->pos[0] / len;
            dir[1] = L->pos[1] / len;
            dir[2] = L->pos[2] / len;
        }
        float col[4];
        rgba8_to_f(L->color, col);
        float *cout = &out[(6 + 2 * j) * 4];
        cout[0] = col[0]; cout[1] = col[1]; cout[2] = col[2];
        ++j;
    }
    return regs * 4;
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

    // Transform VS uniform
    {
        XfConfig t;
        gx_xf_reset(&t);

        // Orthographic projection
        uint8_t pd[7 * 4];
        float raw[6] = {0.5f, 0.0f, -0.25f, 0.0f, -0.001f, -1.0f};
        for (int i = 0; i < 6; ++i) put_f(&pd[i * 4], raw[i]);
        put_u(&pd[6 * 4], GX_XF_ORTHOGRAPHIC);
        gx_xf_apply_xf(&t, XF_SETPROJECTION, 7, pd);

        uint8_t md[12 * 4];
        float pm[12] = {
            1, 0, 0, 4,
            0, 1, 0, 5,
            0, 0, 1, 6,
        };
        for (int i = 0; i < 12; ++i) put_f(&md[i * 4], pm[i]);
        gx_xf_apply_xf(&t, XF_POSMATRICES + 3 * 4, 12, md);

        float cfile[16];
        gx_xf_build_vs_cfile(&t, 3, cfile);

        float proj[16], pos[16];
        gx_xf_projection_matrix(&t, proj);
        gx_xf_position_matrix(&t, 3, pos);
        const float v[4] = {2.0f, -3.0f, 7.0f, 1.0f};
        float eye[4], want[4];
        for (int r = 0; r < 4; ++r) {
            eye[r] = 0.0f;
            for (int k = 0; k < 4; ++k) eye[r] += pos[r * 4 + k] * v[k];
        }
        for (int r = 0; r < 4; ++r) {
            want[r] = 0.0f;
            for (int k = 0; k < 4; ++k) want[r] += proj[r * 4 + k] * eye[k];
        }

        // out[c] = sum_i v[i] * cfile[i][c].
        for (int c = 0; c < 4; ++c) {
            float got = 0.0f;
            for (int i = 0; i < 4; ++i) got += v[i] * cfile[i * 4 + c];
            float diff = got - want[c];
            if (diff < -1e-4f || diff > 1e-4f) return 0;
        }
    }

    // Regular matrix texgen cfile.
    {
        XfConfig t;
        gx_xf_reset(&t);

        float tm[12] = {
            2.0f, 0.0f, 0.0f, 0.25f,
            0.0f, 3.0f, 0.0f, -0.5f,
            0.0f, 0.0f, 1.0f, 0.75f,
        };
        uint8_t d[12 * 4];
        for (int i = 0; i < 12; ++i) put_f(&d[i * 4], tm[i]);
        gx_xf_apply_xf(&t, XF_POSMATRICES + 6 * 4, 12, d);

        const float s = 0.4f, tt = 0.6f;  // input coord (s, t, 1, 1)
        const float coord[4] = {s, tt, 1.0f, 1.0f};

        // STQ
        float cf[12];
        gx_xf_build_texmtx_cfile(&t, 6, true, cf);
        for (int c = 0; c < 3; ++c) {
            float got = s * cf[0 + c] + tt * cf[4 + c] + 1.0f * cf[8 + c];
            float want = 0.0f;
            for (int i = 0; i < 4; ++i) want += tm[c * 4 + i] * coord[i];
            float diff = got - want;
            if (diff < -1e-4f || diff > 1e-4f) return 0;
        }

        // ST
        gx_xf_build_texmtx_cfile(&t, 6, false, cf);
        {
            float qs = s * cf[0 + 0] + tt * cf[4 + 0] + cf[8 + 0];
            float qt = s * cf[0 + 1] + tt * cf[4 + 1] + cf[8 + 1];
            float qq = s * cf[0 + 2] + tt * cf[4 + 2] + cf[8 + 2];
            if (qs - (2.0f * s + 0.25f) < -1e-4f || qs - (2.0f * s + 0.25f) > 1e-4f) return 0;
            if (qt - (3.0f * tt - 0.5f) < -1e-4f || qt - (3.0f * tt - 0.5f) > 1e-4f) return 0;
            if (qq - 1.0f < -1e-4f || qq - 1.0f > 1e-4f) return 0;
        }

        // Default identity matrix
        gx_xf_build_texmtx_cfile(&t, 0, false, cf);
        {
            float qs = s * cf[0 + 0] + tt * cf[4 + 0] + cf[8 + 0];
            float qt = s * cf[0 + 1] + tt * cf[4 + 1] + cf[8 + 1];
            if (qs - s < -1e-4f || qs - s > 1e-4f) return 0;
            if (qt - tt < -1e-4f || qt - tt > 1e-4f) return 0;
        }

        // texgen classification
        {
            uint8_t td[4];
            uint32_t tmi = (uint32_t)GX_XF_TG_REGULAR << 4 |
                           (uint32_t)(5u + 2u) << 7;  // sourcerow = Tex2
            put_u(td, tmi);
            gx_xf_apply_xf(&t, XF_SETTEXMTXINFO + 2, 1, td);
            if (!gx_xf_texgen_is_regular(&t, 2)) return 0;
            uint32_t cmi = (uint32_t)GX_XF_TG_COLOR0 << 4 | (uint32_t)2u << 7;
            put_u(td, cmi);
            gx_xf_apply_xf(&t, XF_SETTEXMTXINFO + 3, 1, td);
            if (gx_xf_texgen_is_regular(&t, 3)) return 0;
        }
    }

    // Lighting: ambient/material colours, channel control, and a light object.
    {
        XfConfig t;
        gx_xf_reset(&t);

        uint8_t d[4];
        put_u(d, 0x11223344u);
        gx_xf_apply_xf(&t, XF_SETCHAN0_AMBCOLOR, 1, d);
        put_u(d, 0xAABBCCDDu);
        gx_xf_apply_xf(&t, XF_SETCHAN0_MATCOLOR, 1, d);
        if (t.amb_color[0] != 0x11223344u || t.mat_color[0] != 0xAABBCCDDu) return 0;

        // Colour channel control: matsource=vtx(1), enablelighting=1,
        // lightMask0_3=0b0101 (lights 0 and 2), ambsource=reg(0),
        // diffusefunc=Clamp(2), attnfunc=Spot(3), lightMask4_7=0b0001 (light 4).
        uint32_t lc = (uint32_t)GX_XF_MATSRC_VTX << 0 |
                      (uint32_t)1u << 1 |
                      (uint32_t)0x5u << 2 |
                      (uint32_t)GX_XF_AMBSRC_REG << 6 |
                      (uint32_t)GX_XF_DF_CLAMP << 7 |
                      (uint32_t)GX_XF_AF_SPOT << 9 |
                      (uint32_t)0x1u << 11;
        put_u(d, lc);
        gx_xf_apply_xf(&t, XF_SETCHAN0_COLOR, 1, d);
        const XfLitChannel *cc = &t.color_chan[0];
        if (cc->matsource != GX_XF_MATSRC_VTX || cc->enablelighting != 1) return 0;
        if (cc->ambsource != GX_XF_AMBSRC_REG || cc->diffusefunc != GX_XF_DF_CLAMP) return 0;
        if (cc->attnfunc != GX_XF_AF_SPOT) return 0;
        if (cc->light_mask != (uint8_t)(0x5u | (0x1u << 4))) return 0;  // 0x15

        uint32_t lc_off = (uint32_t)0xFu << 2 | (uint32_t)0xFu << 11;
        put_u(d, lc_off);
        gx_xf_apply_xf(&t, XF_SETCHAN0_ALPHA, 1, d);
        if (t.alpha_chan[0].enablelighting != 0 || t.alpha_chan[0].light_mask != 0) return 0;

        uint8_t ld[13 * 4];
        put_u(&ld[0], 0xDEADBEEFu);
        float cos[3] = {1.0f, 0.0f, 0.0f};
        float dist[3] = {1.0f, 0.5f, 0.25f};
        float pos[3] = {10.0f, 20.0f, 30.0f};
        float dir[3] = {-1.0f, 0.0f, 0.0f};
        for (int i = 0; i < 3; ++i) put_f(&ld[(1 + i) * 4], cos[i]);   // off 4..6
        for (int i = 0; i < 3; ++i) put_f(&ld[(4 + i) * 4], dist[i]);  // off 7..9
        for (int i = 0; i < 3; ++i) put_f(&ld[(7 + i) * 4], pos[i]);   // off 10..12
        for (int i = 0; i < 3; ++i) put_f(&ld[(10 + i) * 4], dir[i]);  // off 13..15
        gx_xf_apply_xf(&t, XF_LIGHTS + 16 + 3, 13, ld);
        const XfLight *L = &t.lights[1];
        if (L->color != 0xDEADBEEFu) return 0;
        if (L->cosatt[0] != 1.0f || L->distatt[1] != 0.5f || L->distatt[2] != 0.25f) return 0;
        if (L->pos[0] != 10.0f || L->pos[2] != 30.0f || L->dir[0] != -1.0f) return 0;
        if (t.lights[0].color != 0 || t.lights[0].pos[0] != 0.0f) return 0;
    }

    // VS uniform cfile builder.
    {
        XfConfig t;
        gx_xf_reset(&t);

        uint8_t d[4];
        uint32_t lc = (uint32_t)GX_XF_MATSRC_REG << 0 |
                      (uint32_t)1u << 1 |
                      (uint32_t)0x1u << 2 |
                      (uint32_t)GX_XF_AMBSRC_REG << 6 |
                      (uint32_t)GX_XF_DF_CLAMP << 7;
        put_u(d, lc);
        gx_xf_apply_xf(&t, XF_SETCHAN0_COLOR, 1, d);
        put_u(d, 0x80402010u);
        gx_xf_apply_xf(&t, XF_SETCHAN0_MATCOLOR, 1, d);
        put_u(d, 0x20304000u);
        gx_xf_apply_xf(&t, XF_SETCHAN0_AMBCOLOR, 1, d);

        XfLightDesc desc;
        if (!gx_xf_lighting_desc(&t, 0, &desc)) return 0;
        if (!desc.enable || desc.num_lights != 1 || !desc.diffuse || !desc.clamp) return 0;
        if (desc.mat_from_vertex || desc.amb_from_vertex) return 0;

        uint8_t ld[13 * 4];
        float cosa[3] = {1, 0, 0}, dist[3] = {1, 0, 0};
        float lpos[3] = {0, 0, -2}, ldir[3] = {0, 0, 0};
        put_u(&ld[0], 0xFFFFFF00u);
        for (int i = 0; i < 3; ++i) put_f(&ld[(1 + i) * 4], cosa[i]);
        for (int i = 0; i < 3; ++i) put_f(&ld[(4 + i) * 4], dist[i]);
        for (int i = 0; i < 3; ++i) put_f(&ld[(7 + i) * 4], lpos[i]);
        for (int i = 0; i < 3; ++i) put_f(&ld[(10 + i) * 4], ldir[i]);
        gx_xf_apply_xf(&t, XF_LIGHTS + 3, 13, ld);  // light 0, from word 3

        float cf[64];
        int nf = gx_xf_build_light_cfile(&t, 0, 0, cf, 64);
        if (nf != (5 + 2) * 4) return 0;

        if (cf[0 * 4 + 0] != 1.0f || cf[1 * 4 + 1] != 1.0f || cf[2 * 4 + 2] != 1.0f) return 0;
        if (cf[0 * 4 + 1] != 0.0f || cf[0 * 4 + 2] != 0.0f) return 0;
        // Material / ambient reg3 / reg4
        if (fabsf(cf[3 * 4 + 0] - 128.0f / 255.0f) > 1e-4f) return 0;
        if (fabsf(cf[3 * 4 + 2] - 32.0f / 255.0f) > 1e-4f) return 0;
        if (fabsf(cf[4 * 4 + 2] - 64.0f / 255.0f) > 1e-4f) return 0;
        // Light dir reg5 normalised, colour reg6 white
        if (cf[5 * 4 + 0] != 0.0f || fabsf(cf[5 * 4 + 2] + 1.0f) > 1e-4f) return 0;
        if (fabsf(cf[6 * 4 + 0] - 1.0f) > 1e-4f) return 0;

        float N[3] = {0, 0, -1};
        float np[3];
        for (int c = 0; c < 3; ++c)
            np[c] = N[0] * cf[0 * 4 + c] + N[1] * cf[1 * 4 + c] + N[2] * cf[2 * 4 + c];
        float ndotl = np[0] * cf[5 * 4 + 0] + np[1] * cf[5 * 4 + 1] + np[2] * cf[5 * 4 + 2];
        if (ndotl < 0.0f) ndotl = 0.0f;
        float res[3];
        for (int c = 0; c < 3; ++c) {
            float lacc = cf[4 * 4 + c] + ndotl * cf[6 * 4 + c];
            if (lacc < 0.0f) lacc = 0.0f;
            if (lacc > 1.0f) lacc = 1.0f;
            res[c] = cf[3 * 4 + c] * lacc;
        }
        if (fabsf(res[0] - 128.0f / 255.0f) > 1e-4f) return 0;
        if (fabsf(res[1] - 64.0f / 255.0f) > 1e-4f) return 0;
        if (fabsf(res[2] - 32.0f / 255.0f) > 1e-4f) return 0;

        XfConfig u;
        gx_xf_reset(&u);
        XfLightDesc off;
        if (!gx_xf_lighting_desc(&u, 0, &off) || off.enable) return 0;
        if (gx_xf_build_light_cfile(&u, 0, 0, cf, 64) != 0) return 0;
    }

    return 1;
}
