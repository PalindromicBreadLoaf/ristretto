// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu/gx_tev.h"

#include <string.h>

// BP register addresses.
enum {
    BP_GENMODE    = 0x00,
    BP_TREF_BASE  = 0x28,  // 0x28..0x2F: two stage orders per register
    BP_TREF_LAST  = 0x2F,
    BP_COMB_BASE  = 0xC0,  // 0xC0..0xDF: colour (even) + alpha (odd) per stage
    BP_COMB_LAST  = 0xDF,
    BP_KSEL_BASE  = 0xF6,  // 0xF6..0xFD: konst selects + swap tables, two stages each
    BP_KSEL_LAST  = 0xFD,
};

static inline uint32_t bits(uint32_t v, unsigned lo, unsigned n) {
    return (v >> lo) & ((1u << n) - 1u);
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

    return 1;
}
