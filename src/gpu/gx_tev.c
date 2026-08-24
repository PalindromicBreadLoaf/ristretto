// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu/gx_tev.h"

#include <string.h>

// BP register addresses.
enum {
    BP_GENMODE    = 0x00,
    BP_IND_MTX_BASE = 0x06,
    BP_IND_MTX_LAST = 0x0E,
    BP_IND_STAGE_BASE = 0x10,
    BP_IND_STAGE_LAST = 0x1F,
    BP_IND_SCALE0 = 0x25,
    BP_IND_SCALE1 = 0x26,
    BP_IND_REF = 0x27,
    BP_TREF_BASE  = 0x28,  // 0x28..0x2F: two stage orders per register
    BP_TREF_LAST  = 0x2F,
    BP_COMB_BASE  = 0xC0,  // 0xC0..0xDF: colour (even) + alpha (odd) per stage
    BP_COMB_LAST  = 0xDF,
    BP_KSEL_BASE  = 0xF6,  // 0xF6..0xFD: konst selects + swap tables, two stages each
    BP_KSEL_LAST  = 0xFD,
    BP_TEVREG_BASE = 0xE0, // 0xE0..0xE7: four TEV colour/konst register pairs (RA/BG)
    BP_TEVREG_LAST = 0xE7,
    BP_FOGRANGE_BASE = 0xE8,
    BP_FOGRANGE_LAST = 0xED,
    BP_FOGPARAM0 = 0xEE,
    BP_FOGBMAGNITUDE = 0xEF,
    BP_FOGBEXPONENT = 0xF0,
    BP_FOGPARAM3 = 0xF1,
    BP_FOGCOLOR = 0xF2,
    BP_ZTEX1 = 0xF4,
    BP_ZTEX2 = 0xF5,
};

static inline uint32_t bits(uint32_t v, unsigned lo, unsigned n) {
    return (v >> lo) & ((1u << n) - 1u);
}

// Sign-extend an 11-bit TEV colour register field.
static inline int32_t sx11(uint32_t v) {
    return (int32_t)(v << 21) >> 21;
}

static float fog_float(uint32_t value) {
    uint32_t bits = ((value >> 19) & 1u) << 31;
    bits |= ((value >> 11) & 0xFFu) << 23;
    bits |= (value & 0x7FFu) << 12;
    float result;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

static float indirect_scale(uint32_t scale) {
    float value = 1.0f;
    if (scale < 17u)
        for (uint32_t i = scale; i < 17u; ++i) value *= 0.5f;
    else
        for (uint32_t i = 17u; i < scale; ++i) value *= 2.0f;
    return value;
}

static void decode_indirect_matrix(TevConfig *cfg, uint32_t row, uint32_t value) {
    const uint32_t matrix = row / 3u;
    const uint32_t part = row % 3u;
    const float a = (float)sx11(bits(value, 0, 11)) / 1024.0f;
    const float b = (float)sx11(bits(value, 11, 11)) / 1024.0f;
    uint32_t scale = (uint32_t)cfg->indirect_mtx[matrix][0][3];
    if (part == 0) {
        cfg->indirect_mtx[matrix][0][0] = a;
        cfg->indirect_mtx[matrix][1][0] = b;
        scale = (scale & ~3u) | bits(value, 22, 2);
    } else if (part == 1) {
        cfg->indirect_mtx[matrix][0][1] = a;
        cfg->indirect_mtx[matrix][1][1] = b;
        scale = (scale & ~12u) | (bits(value, 22, 2) << 2);
    } else {
        cfg->indirect_mtx[matrix][0][2] = a;
        cfg->indirect_mtx[matrix][1][2] = b;
        scale = (scale & ~16u) | (bits(value, 22, 1) << 4);
    }
    cfg->indirect_mtx[matrix][0][3] = (float)scale;
    cfg->indirect_mtx[matrix][1][3] = (float)scale;
}

// Colour combiner
static void decode_color(TevCombiner *cc, uint32_t v) {
    cc->d     = bits(v, 0, 4);
    cc->c     = bits(v, 4, 4);
    cc->b     = bits(v, 8, 4);
    cc->a     = bits(v, 12, 4);
    cc->bias  = bits(v, 16, 2);
    cc->op    = bits(v, 18, 1);
    cc->clamp = bits(v, 19, 1);
    cc->scale = bits(v, 20, 2);
    cc->dest  = bits(v, 22, 2);
}

// Alpha combiner
static void decode_alpha(TevCombiner *ac, uint32_t v, uint8_t *rswap, uint8_t *tswap) {
    *rswap    = bits(v, 0, 2);
    *tswap    = bits(v, 2, 2);
    ac->d     = bits(v, 4, 3);
    ac->c     = bits(v, 7, 3);
    ac->b     = bits(v, 10, 3);
    ac->a     = bits(v, 13, 3);
    ac->bias  = bits(v, 16, 2);
    ac->op    = bits(v, 18, 1);
    ac->clamp = bits(v, 19, 1);
    ac->scale = bits(v, 20, 2);
    ac->dest  = bits(v, 22, 2);
}

static void decode_order(TevStage *even, TevStage *odd, uint32_t v) {
    even->texmap     = bits(v, 0, 3);
    even->texcoord   = bits(v, 3, 3);
    even->tex_enable = bits(v, 6, 1);
    even->colorchan  = bits(v, 7, 3);
    odd->texmap      = bits(v, 12, 3);
    odd->texcoord    = bits(v, 15, 3);
    odd->tex_enable  = bits(v, 18, 1);
    odd->colorchan   = bits(v, 19, 3);
}

void gx_tev_reset(TevConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->num_stages = 1;
    // Identity swap tables
    for (int t = 0; t < 4; ++t)
        for (int ch = 0; ch < 4; ++ch)
            cfg->swap[t][ch] = (uint8_t)ch;
}

void gx_tev_apply_bp(TevConfig *cfg, uint8_t reg, uint32_t value) {
    value &= 0xFFFFFFu;  // BP payloads are 24-bit

    if (reg == BP_GENMODE) {
        cfg->num_stages = (uint8_t)(bits(value, 10, 4) + 1u);  // stored as count-1
        return;
    }
    if (reg >= BP_IND_MTX_BASE && reg <= BP_IND_MTX_LAST) {
        decode_indirect_matrix(cfg, reg - BP_IND_MTX_BASE, value);
        return;
    }
    if (reg >= BP_IND_STAGE_BASE && reg <= BP_IND_STAGE_LAST) {
        cfg->stage[reg - BP_IND_STAGE_BASE].indirect = value & 0x1FFFFFu;
        return;
    }
    if (reg == BP_IND_REF) {
        for (uint32_t i = 0; i < 4; ++i) {
            cfg->indirect_stage[i].texmap = (uint8_t)bits(value, i * 6, 3);
            cfg->indirect_stage[i].texcoord = (uint8_t)bits(value, i * 6 + 3, 3);
        }
        return;
    }
    if (reg == BP_IND_SCALE0 || reg == BP_IND_SCALE1) {
        const uint32_t base = (reg - BP_IND_SCALE0) * 2u;
        cfg->indirect_stage[base].scale_s = (uint8_t)bits(value, 0, 4);
        cfg->indirect_stage[base].scale_t = (uint8_t)bits(value, 4, 4);
        cfg->indirect_stage[base + 1].scale_s = (uint8_t)bits(value, 8, 4);
        cfg->indirect_stage[base + 1].scale_t = (uint8_t)bits(value, 12, 4);
        return;
    }
    if (reg >= BP_TREF_BASE && reg <= BP_TREF_LAST) {
        uint32_t pair = reg - BP_TREF_BASE;
        decode_order(&cfg->stage[pair * 2], &cfg->stage[pair * 2 + 1], value);
        return;
    }
    if (reg >= BP_COMB_BASE && reg <= BP_COMB_LAST) {
        uint32_t idx = reg - BP_COMB_BASE;
        TevStage *st = &cfg->stage[idx / 2];
        if (idx & 1)
            decode_alpha(&st->alpha, value, &st->rswap, &st->tswap);
        else
            decode_color(&st->color, value);
        return;
    }
    if (reg >= BP_TEVREG_BASE && reg <= BP_TEVREG_LAST) {
        // 0xE0/E2/E4/E6 = RA (red+alpha), 0xE1/E3/E5/E7 = BG (blue+green).
        uint32_t num  = (reg >> 1) & 3u;
        bool     is_bg = reg & 1u;
        bool     is_konst = bits(value, 23, 1);
        float   *dst = is_konst ? cfg->konst[num] : cfg->color[num];
        if (is_bg) {
            dst[2] = (float)sx11(bits(value, 0, 11)) / 255.0f;   // blue
            dst[1] = (float)sx11(bits(value, 12, 11)) / 255.0f;  // green
        } else {
            dst[0] = (float)sx11(bits(value, 0, 11)) / 255.0f;   // red
            dst[3] = (float)sx11(bits(value, 12, 11)) / 255.0f;  // alpha
        }
        return;
    }
    if (reg >= BP_KSEL_BASE && reg <= BP_KSEL_LAST) {
        uint32_t i = reg - BP_KSEL_BASE;
        cfg->stage[i * 2].kcsel     = (uint8_t)bits(value, 4, 5);
        cfg->stage[i * 2].kasel     = (uint8_t)bits(value, 9, 5);
        cfg->stage[i * 2 + 1].kcsel = (uint8_t)bits(value, 14, 5);
        cfg->stage[i * 2 + 1].kasel = (uint8_t)bits(value, 19, 5);
        // Swap tables
        uint8_t swap_rb = (uint8_t)bits(value, 0, 2);
        uint8_t swap_ga = (uint8_t)bits(value, 2, 2);
        uint32_t table = i / 2;
        if (i & 1) {
            cfg->swap[table][2] = swap_rb;  // Blue source
            cfg->swap[table][3] = swap_ga;  // Alpha source
        } else {
            cfg->swap[table][0] = swap_rb;  // Red source
            cfg->swap[table][1] = swap_ga;  // Green source
        }
        return;
    }
    switch (reg) {
    case BP_FOGRANGE_BASE:
        cfg->pixel.fog_range[0] = value;
        return;
    case BP_FOGRANGE_BASE + 1: case BP_FOGRANGE_BASE + 2: case BP_FOGRANGE_BASE + 3:
    case BP_FOGRANGE_BASE + 4: case BP_FOGRANGE_LAST:
        cfg->pixel.fog_range[reg - BP_FOGRANGE_BASE] = value;
        return;
    case BP_FOGPARAM0: cfg->pixel.fog_a = value; return;
    case BP_FOGBMAGNITUDE: cfg->pixel.fog_b_magnitude = value; return;
    case BP_FOGBEXPONENT: cfg->pixel.fog_b_shift = value; return;
    case BP_FOGPARAM3:
        cfg->pixel.fog_c = value;
        cfg->pixel.fog_ortho = bits(value, 20, 1) != 0;
        cfg->pixel.fog_type = (uint8_t)bits(value, 21, 3);
        return;
    case BP_FOGCOLOR: cfg->pixel.fog_color = value; return;
    case BP_ZTEX1: cfg->pixel.ztex_bias = value; return;
    case BP_ZTEX2:
        cfg->pixel.ztex_format = (uint8_t)bits(value, 0, 2);
        cfg->pixel.ztex_op = (uint8_t)bits(value, 2, 2);
        return;
    default: return;
    }
}

void gx_tev_build_ps_cfile(const TevConfig *cfg, float out[GX_TEV_PS_CFILE_COUNT][4]) {
    for (int i = 0; i < 4; ++i)
        for (int ch = 0; ch < 4; ++ch) out[i][ch] = cfg->color[i][ch];
    for (int i = 0; i < 4; ++i)
        for (int ch = 0; ch < 4; ++ch) out[4 + i][ch] = cfg->konst[i][ch];
    out[8][0] = (float)cfg->pixel.alpha_ref0 / 255.0f;
    out[8][1] = (float)cfg->pixel.alpha_ref1 / 255.0f;
    out[8][2] = cfg->pixel.rgba6 ? (float)(cfg->pixel.dst_alpha >> 2) / 63.0f
                                  : (float)cfg->pixel.dst_alpha / 255.0f;
    out[8][3] = 0.0f;
    out[9][0] = cfg->pixel.rgba6 ? 63.0f : 255.0f;
    out[9][1] = cfg->pixel.rgba6 ? 1.0f / 63.0f : 1.0f / 255.0f;
    for (uint32_t m = 0; m < 3; ++m) {
        memcpy(out[10 + m * 2], cfg->indirect_mtx[m][0], 4 * sizeof(float));
        memcpy(out[11 + m * 2], cfg->indirect_mtx[m][1], 4 * sizeof(float));
        out[10 + m * 2][3] = out[11 + m * 2][3] =
            indirect_scale((uint32_t)cfg->indirect_mtx[m][0][3]);
    }
    out[16][0] = (float)((cfg->pixel.fog_color >> 16) & 0xFFu) / 255.0f;
    out[16][1] = (float)((cfg->pixel.fog_color >> 8) & 0xFFu) / 255.0f;
    out[16][2] = (float)(cfg->pixel.fog_color & 0xFFu) / 255.0f;
    out[16][3] = (float)(cfg->pixel.ztex_bias & 0xFFFFFFu) / 16777216.0f;
    out[17][0] = fog_float(cfg->pixel.fog_a);
    out[17][1] = fog_float(cfg->pixel.fog_c);
    out[17][2] = (float)(cfg->pixel.fog_b_magnitude & 0xFFFFFFu) / 16777216.0f;
    out[17][3] = (float)(cfg->pixel.fog_b_shift & 0x1Fu);
}

// Self test
int gx_tev_selftest(void) {
    TevConfig cfg;
    gx_tev_reset(&cfg);

    if (cfg.num_stages != 1) return 0;
    if (cfg.swap[2][0] != 0 || cfg.swap[2][3] != 3) return 0;  // identity default

    // GENMODE
    gx_tev_apply_bp(&cfg, BP_GENMODE, (3u << 10));
    if (cfg.num_stages != 4) return 0;

    // Stage 0 colour combiner
    uint32_t cc = (uint32_t)GX_CC_ZERO       << 0  |
                  (uint32_t)GX_CC_RASC       << 4  |
                  (uint32_t)GX_CC_TEXC       << 8  |
                  (uint32_t)GX_CC_ZERO       << 12 |
                  (uint32_t)GX_TB_ZERO       << 16 |
                  (uint32_t)GX_TEV_ADD       << 18 |
                  (uint32_t)1u               << 19 |
                  (uint32_t)GX_TS_1          << 20 |
                  (uint32_t)GX_TEVOUT_PREV   << 22;
    gx_tev_apply_bp(&cfg, BP_COMB_BASE, cc);
    const TevCombiner *c0 = &cfg.stage[0].color;
    if (c0->a != GX_CC_ZERO || c0->b != GX_CC_TEXC || c0->c != GX_CC_RASC ||
        c0->d != GX_CC_ZERO)
        return 0;
    if (c0->bias != GX_TB_ZERO || c0->op != GX_TEV_ADD || !c0->clamp ||
        c0->scale != GX_TS_1 || c0->dest != GX_TEVOUT_PREV)
        return 0;

    // Stage 0 alpha combiner
    uint32_t ac = (uint32_t)1u             << 0  |  // rswap
                  (uint32_t)2u             << 2  |  // tswap
                  (uint32_t)GX_CA_APREV    << 4  |  // d
                  (uint32_t)GX_CA_KONST    << 7  |  // c
                  (uint32_t)GX_CA_RASA     << 10 |  // b
                  (uint32_t)GX_CA_TEXA     << 13 |  // a
                  (uint32_t)GX_TB_SUBHALF  << 16 |
                  (uint32_t)GX_TEV_SUB     << 18 |
                  (uint32_t)0u             << 19 |
                  (uint32_t)GX_TS_2        << 20 |
                  (uint32_t)GX_TEVOUT_C0   << 22;
    gx_tev_apply_bp(&cfg, BP_COMB_BASE + 1, ac);
    const TevStage *s0 = &cfg.stage[0];
    if (s0->alpha.a != GX_CA_TEXA || s0->alpha.b != GX_CA_RASA ||
        s0->alpha.c != GX_CA_KONST || s0->alpha.d != GX_CA_APREV)
        return 0;
    if (s0->alpha.bias != GX_TB_SUBHALF || s0->alpha.op != GX_TEV_SUB ||
        s0->alpha.clamp || s0->alpha.scale != GX_TS_2 ||
        s0->alpha.dest != GX_TEVOUT_C0)
        return 0;
    if (s0->rswap != 1 || s0->tswap != 2) return 0;

    // Stage order register 0x28 carries stages 0 (even) and 1 (odd).
    uint32_t tref = (uint32_t)3u          << 0  |   // texmap_even
                    (uint32_t)5u          << 3  |   // texcoord_even
                    (uint32_t)1u          << 6  |   // enable_tex_even
                    (uint32_t)GX_RAS_COLOR0 << 7 |  // colorchan_even
                    (uint32_t)2u          << 12 |   // texmap_odd
                    (uint32_t)4u          << 15 |   // texcoord_odd
                    (uint32_t)0u          << 18 |   // enable_tex_odd
                    (uint32_t)GX_RAS_COLOR1 << 19;  // colorchan_odd
    gx_tev_apply_bp(&cfg, BP_TREF_BASE, tref);
    if (s0->texmap != 3 || s0->texcoord != 5 || !s0->tex_enable ||
        s0->colorchan != GX_RAS_COLOR0)
        return 0;
    const TevStage *s1 = &cfg.stage[1];
    if (s1->texmap != 2 || s1->texcoord != 4 || s1->tex_enable ||
        s1->colorchan != GX_RAS_COLOR1)
        return 0;

    // KSEL register 0xF6 carries stages 0/1 konst plus swap table 0's R/G.
    uint32_t ksel = (uint32_t)2u  << 0  |  // swap_rb
                    (uint32_t)3u  << 2  |  // swap_ga
                    (uint32_t)12u << 4  |  // kcsel_even (K0)
                    (uint32_t)16u << 9  |  // kasel_even (K0_R)
                    (uint32_t)13u << 14 |  // kcsel_odd (K1)
                    (uint32_t)29u << 19;   // kasel_odd (K1_A)
    gx_tev_apply_bp(&cfg, BP_KSEL_BASE, ksel);
    if (cfg.stage[0].kcsel != 12 || cfg.stage[0].kasel != 16) return 0;
    if (cfg.stage[1].kcsel != 13 || cfg.stage[1].kasel != 29) return 0;
    if (cfg.swap[0][0] != 2 || cfg.swap[0][1] != 3) return 0;  // R<-B, G<-A
    if (cfg.swap[0][2] != 2 || cfg.swap[0][3] != 3) return 0;  // untouched identity

    // Odd KSEL register 0xF7 writes swap table 0's B/A entries.
    uint32_t ksel_odd = (uint32_t)1u << 0 |  // swap_rb -> Blue source = Green(1)
                        (uint32_t)0u << 2;   // swap_ga -> Alpha source = Red(0)
    gx_tev_apply_bp(&cfg, BP_KSEL_BASE + 1, ksel_odd);
    if (cfg.swap[0][2] != 1 || cfg.swap[0][3] != 0) return 0;

    // Colour register 1
    gx_tev_apply_bp(&cfg, BP_TEVREG_BASE + 2,
                    (uint32_t)128u << 0 | (uint32_t)64u << 12);   // red=128, alpha=64
    gx_tev_apply_bp(&cfg, BP_TEVREG_BASE + 3,
                    (uint32_t)32u << 0 | (uint32_t)200u << 12);   // blue=32, green=200
    if (cfg.color[1][0] != 128.0f / 255.0f || cfg.color[1][3] != 64.0f / 255.0f)
        return 0;
    if (cfg.color[1][2] != 32.0f / 255.0f || cfg.color[1][1] != 200.0f / 255.0f)
        return 0;

    // Konst register 2
    gx_tev_apply_bp(&cfg, BP_TEVREG_BASE + 4,
                    (1u << 23) | (uint32_t)255u << 0 | (uint32_t)10u << 12);
    if (cfg.konst[2][0] != 1.0f || cfg.konst[2][3] != 10.0f / 255.0f) return 0;
    if (cfg.color[2][0] != 0.0f) return 0;  // color slot 2 stayed untouched

    // A negative 11-bit field sign-extends.
    gx_tev_apply_bp(&cfg, BP_TEVREG_BASE + 0, (uint32_t)0x7FFu << 0);  // red = -1
    if (cfg.color[0][0] != -1.0f / 255.0f) return 0;

    // cfile packs PREV/C0/C1/C2 then K0..K3.
    float cfile[GX_TEV_PS_CFILE_COUNT][4];
    gx_tev_build_ps_cfile(&cfg, cfile);
    if (cfile[1][0] != cfg.color[1][0] || cfile[1][1] != cfg.color[1][1]) return 0;
    if (cfile[6][0] != cfg.konst[2][0] || cfile[6][3] != cfg.konst[2][3]) return 0;
    cfg.pixel.alpha_ref0 = 0x20;
    cfg.pixel.alpha_ref1 = 0xE0;
    cfg.pixel.rgba6 = true;
    cfg.pixel.dst_alpha = 0x80;
    gx_tev_build_ps_cfile(&cfg, cfile);
    if (cfile[8][0] != 32.0f / 255.0f || cfile[8][1] != 224.0f / 255.0f ||
        cfile[8][2] != 32.0f / 63.0f || cfile[9][0] != 63.0f ||
        cfile[9][1] != 1.0f / 63.0f)
        return 0;

    // Indirect state BP registers retain the source, scale, matrix, and per-stage control.
    gx_tev_apply_bp(&cfg, BP_IND_REF, (3u << 0) | (4u << 3) | (2u << 6) | (1u << 9));
    gx_tev_apply_bp(&cfg, BP_IND_SCALE0, (2u << 0) | (3u << 4) | (4u << 8) | (5u << 12));
    gx_tev_apply_bp(&cfg, BP_IND_MTX_BASE, (uint32_t)1023u | ((uint32_t)512u << 11) |
                                      (1u << 22));
    gx_tev_apply_bp(&cfg, BP_IND_MTX_BASE + 1, (uint32_t)512u | ((uint32_t)256u << 11) |
                                          (2u << 22));
    gx_tev_apply_bp(&cfg, BP_IND_MTX_BASE + 2, (uint32_t)128u | ((uint32_t)64u << 11) |
                                          (1u << 22));
    gx_tev_apply_bp(&cfg, BP_IND_STAGE_BASE, 1u | (3u << 7) | (1u << 9));
    if (cfg.indirect_stage[0].texmap != 3 || cfg.indirect_stage[0].texcoord != 4 ||
        cfg.indirect_stage[0].scale_s != 2 || cfg.indirect_stage[1].scale_t != 5 ||
        cfg.stage[0].indirect != (1u | (3u << 7) | (1u << 9)) ||
        cfg.indirect_mtx[0][0][0] != 1023.0f / 1024.0f || cfg.indirect_mtx[0][1][0] != 0.5f ||
        cfg.indirect_mtx[0][0][1] != 0.5f || cfg.indirect_mtx[0][1][2] != 64.0f / 1024.0f)
        return 0;

    gx_tev_apply_bp(&cfg, BP_FOGPARAM0, 0x3F800u);
    gx_tev_apply_bp(&cfg, BP_FOGPARAM3, 0x3F800u | (1u << 20) | (4u << 21));
    gx_tev_apply_bp(&cfg, BP_FOGCOLOR, 0x112233u);
    gx_tev_apply_bp(&cfg, BP_ZTEX1, 0x123456u);
    gx_tev_apply_bp(&cfg, BP_ZTEX2, 2u | (1u << 2));
    gx_tev_build_ps_cfile(&cfg, cfile);
    if (cfg.pixel.fog_type != 4 || !cfg.pixel.fog_ortho || cfg.pixel.ztex_format != 2 ||
        cfg.pixel.ztex_op != 1 || cfg.pixel.ztex_bias != 0x123456u ||
        cfile[16][0] != 17.0f / 255.0f || cfile[16][2] != 51.0f / 255.0f ||
        cfile[10][0] != 1.0f || cfile[11][0] != 1.0f)
        return 0;

    return 1;
}
