// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu/tev_shader_gen.h"

#include <string.h>

#include "gpu/r700_emit.h"

// Reserve 48 CF slots so bodies start at byte 0x180.
// This is enough for 16 stages plus the reg init clause and the export.
#define TEV_PS_MAX_CF 48

#define TEV_PS_REG_CFILE_BASE 0u

#define TEV_PS_PV_GPR 127u

// A 2D texture is sampled with coordinates (x, y, 0, x).
static const uint8_t kCoord2D[4] = {R700_SEL_X, R700_SEL_Y, R700_SEL_0, R700_SEL_X};
static const uint8_t kSelXYZW[4] = {R700_CHAN_X, R700_CHAN_Y, R700_CHAN_Z, R700_CHAN_W};

// GPR allocation for a generated program.
typedef struct {
    uint32_t ras;           // rasterised colour 0
    uint32_t coord_gpr[8];  // interpolant GPR per texcoord index
    bool     coord_used[8];
    uint32_t ncoord;
    uint32_t reg[4];        // PREV, C0, C1, C2
    uint32_t tex;           // current-stage texture sample
    uint32_t konst;         // current-stage konst colour
    uint32_t num_gprs;
} TevPsAlloc;

static uint32_t f32_bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

// One resolved ALU operand.
typedef struct { uint32_t sel; uint32_t chan; } AluSrc;

static void plan_alloc(const TevConfig *cfg, TevPsAlloc *a)
{
    memset(a, 0, sizeof(*a));
    a->ras = 0;

    // Assign an interpolant GPR to each distinct texcoord in ascending order.
    for (uint32_t s = 0; s < cfg->num_stages; ++s) {
        if (!cfg->stage[s].tex_enable) continue;
        uint8_t tc = cfg->stage[s].texcoord & 7u;
        a->coord_used[tc] = true;
    }
    uint32_t next = 1;
    for (uint32_t tc = 0; tc < 8; ++tc)
        if (a->coord_used[tc]) { a->coord_gpr[tc] = next++; a->ncoord++; }

    uint32_t base = next;
    for (uint32_t i = 0; i < 4; ++i) a->reg[i] = base + i;
    a->tex   = base + 4;
    a->konst = base + 5;
    a->num_gprs = base + 6;
}

// Colour-combiner input select to operand for output channel ch (0..2).
static AluSrc resolve_cc(const TevPsAlloc *a, uint8_t sel, uint32_t ch)
{
    AluSrc r = {R700_ALU_SRC_0, 0};
    switch (sel) {
        case GX_CC_CPREV: r.sel = a->reg[0]; r.chan = ch; break;
        case GX_CC_APREV: r.sel = a->reg[0]; r.chan = 3; break;
        case GX_CC_C0:    r.sel = a->reg[1]; r.chan = ch; break;
        case GX_CC_A0:    r.sel = a->reg[1]; r.chan = 3; break;
        case GX_CC_C1:    r.sel = a->reg[2]; r.chan = ch; break;
        case GX_CC_A1:    r.sel = a->reg[2]; r.chan = 3; break;
        case GX_CC_C2:    r.sel = a->reg[3]; r.chan = ch; break;
        case GX_CC_A2:    r.sel = a->reg[3]; r.chan = 3; break;
        case GX_CC_ONE:   r.sel = R700_ALU_SRC_1;   r.chan = 0; break;
        case GX_CC_HALF:  r.sel = R700_ALU_SRC_0_5; r.chan = 0; break;
        case GX_CC_KONST: r.sel = a->konst; r.chan = ch; break;
        case GX_CC_ZERO:  r.sel = R700_ALU_SRC_0;   r.chan = 0; break;
        default: break;
    }
    return r;
}

// Resolve including the swap-tabled texture and rasterised inputs.
static AluSrc resolve_cc_full(const TevConfig *cfg, const TevPsAlloc *a,
                              uint32_t s, uint8_t sel, uint32_t ch)
{
    const TevStage *st = &cfg->stage[s];
    const uint8_t *tsw = cfg->swap[st->tswap];
    const uint8_t *rsw = cfg->swap[st->rswap];
    bool ras_zero = st->colorchan == GX_RAS_ZERO;
    AluSrc r;
    switch (sel) {
        case GX_CC_TEXC: r.sel = a->tex; r.chan = tsw[ch]; return r;
        case GX_CC_TEXA: r.sel = a->tex; r.chan = tsw[3];  return r;
        case GX_CC_RASC:
            if (ras_zero) { r.sel = R700_ALU_SRC_0; r.chan = 0; }
            else          { r.sel = a->ras; r.chan = rsw[ch]; }
            return r;
        case GX_CC_RASA:
            if (ras_zero) { r.sel = R700_ALU_SRC_0; r.chan = 0; }
            else          { r.sel = a->ras; r.chan = rsw[3]; }
            return r;
        default: return resolve_cc(a, sel, ch);
    }
}

// Alpha combiner input select to operand.
static AluSrc resolve_ca(const TevConfig *cfg, const TevPsAlloc *a, uint32_t s,
                         uint8_t sel)
{
    const TevStage *st = &cfg->stage[s];
    const uint8_t *tsw = cfg->swap[st->tswap];
    const uint8_t *rsw = cfg->swap[st->rswap];
    bool ras_zero = st->colorchan == GX_RAS_ZERO;
    AluSrc r = {R700_ALU_SRC_0, 0};
    switch (sel) {
        case GX_CA_APREV: r.sel = a->reg[0]; r.chan = 3; break;
        case GX_CA_A0:    r.sel = a->reg[1]; r.chan = 3; break;
        case GX_CA_A1:    r.sel = a->reg[2]; r.chan = 3; break;
        case GX_CA_A2:    r.sel = a->reg[3]; r.chan = 3; break;
        case GX_CA_TEXA:  r.sel = a->tex; r.chan = tsw[3]; break;
        case GX_CA_RASA:
            if (!ras_zero) { r.sel = a->ras; r.chan = rsw[3]; }
            break;
        case GX_CA_KONST: r.sel = a->konst; r.chan = 3; break;
        case GX_CA_ZERO:  break;
        default: break;
    }
    return r;
}

static float scale_value(uint8_t s)
{
    switch (s) {
        case GX_TS_2: return 2.0f;
        case GX_TS_4: return 4.0f;
        case GX_TS_HALF: return 0.5f;
        default: return 1.0f;
    }
}

static float bias_value(uint8_t b)
{
    switch (b) {
        case GX_TB_ADDHALF: return 0.5f;
        case GX_TB_SUBHALF: return -0.5f;
        default: return 0.0f;  // GX_TB_ZERO
    }
}

// Resolve a konst select for one output channel.
static AluSrc resolve_konst(uint8_t ksel, uint32_t ch, bool alpha,
                            uint32_t lit_chan, float *frac, bool *need_lit)
{
    AluSrc r = {R700_ALU_SRC_0, 0};
    if (ksel <= 7) {  // constant fraction (8-ksel)/8
        *frac = (float)(8 - ksel) / 8.0f;
        *need_lit = true;
        r.sel = R700_ALU_SRC_LITERAL;
        r.chan = lit_chan;
        return r;
    }
    if (!alpha && ksel >= 12 && ksel <= 15) {  // K0..K3 rgb
        r.sel = R700_ALU_SRC_CFILE(TEV_PS_KONST_CFILE_BASE + (ksel - 12));
        r.chan = ch;
        return r;
    }
    if (ksel >= 16 && ksel <= 31) {  // K0..K3 single channel R/G/B/A
        uint32_t idx = (ksel - 16) & 3u;
        uint32_t src_chan = (ksel - 16) >> 2;  // 0=R,1=G,2=B,3=A
        r.sel = R700_ALU_SRC_CFILE(TEV_PS_KONST_CFILE_BASE + idx);
        r.chan = src_chan;
        return r;
    }
    return r;
}

static bool stage_uses_konst(const TevStage *st)
{
    const TevCombiner *c = &st->color;
    const TevCombiner *al = &st->alpha;
    if (c->a == GX_CC_KONST || c->b == GX_CC_KONST || c->c == GX_CC_KONST ||
        c->d == GX_CC_KONST)
        return true;
    return al->a == GX_CA_KONST || al->b == GX_CA_KONST ||
           al->c == GX_CA_KONST || al->d == GX_CA_KONST;
}

// Append the four channel konst materialisation group into buf.
static uint32_t emit_konst_group(const TevStage *st, const TevPsAlloc *a,
                                 R700Inst64 *buf)
{
    uint32_t n = 0;
    float cfrac = 0.0f, afrac = 0.0f;
    bool need_lit = false;
    AluSrc csrc[3];
    for (uint32_t ch = 0; ch < 3; ++ch)
        csrc[ch] = resolve_konst(st->kcsel, ch, false, R700_CHAN_X, &cfrac, &need_lit);
    AluSrc asrc = resolve_konst(st->kasel, 3, true, R700_CHAN_Z, &afrac, &need_lit);

    for (uint32_t ch = 0; ch < 4; ++ch) {
        AluSrc s = (ch < 3) ? csrc[ch] : asrc;
        buf[n++] = r700_alu_op2(R700_OP2_MOV, a->konst, ch,
                                s.sel, s.chan, false,
                                R700_ALU_SRC_0, 0, false,
                                false, true, ch == 3);
    }
    if (need_lit) {
        buf[n++] = r700_alu_literal(f32_bits(cfrac), 0);  // lanes X, Y
        buf[n++] = r700_alu_literal(f32_bits(afrac), 0);  // lanes Z, W
    }
    return n;
}

// The arithmetic combiner over all four channels at once.
static uint32_t emit_stage_arith(const TevConfig *cfg, uint32_t s,
                                 const TevPsAlloc *a, R700Inst64 *buf)
{
    const TevStage *st = &cfg->stage[s];
    const TevCombiner *cc = &st->color;
    const TevCombiner *ca = &st->alpha;
    uint32_t n = 0;

    // PV = b - a
    for (uint32_t ch = 0; ch < 4; ++ch) {
        AluSrc av = (ch < 3) ? resolve_cc_full(cfg, a, s, cc->a, ch)
                             : resolve_ca(cfg, a, s, ca->a);
        AluSrc bv = (ch < 3) ? resolve_cc_full(cfg, a, s, cc->b, ch)
                             : resolve_ca(cfg, a, s, ca->b);
        buf[n++] = r700_alu_op2(R700_OP2_ADD, TEV_PS_PV_GPR, ch,
                                bv.sel, bv.chan, false,
                                av.sel, av.chan, true,   // - a
                                false, true, ch == 3);
    }
    // PV = c * (b - a) + a
    for (uint32_t ch = 0; ch < 4; ++ch) {
        AluSrc cv = (ch < 3) ? resolve_cc_full(cfg, a, s, cc->c, ch)
                             : resolve_ca(cfg, a, s, ca->c);
        AluSrc av = (ch < 3) ? resolve_cc_full(cfg, a, s, cc->a, ch)
                             : resolve_ca(cfg, a, s, ca->a);
        buf[n++] = r700_alu_op3(R700_OP3_MULADD, TEV_PS_PV_GPR, ch,
                                cv.sel, cv.chan, false,
                                R700_ALU_SRC_PV, ch, false,
                                av.sel, av.chan, false,
                                false, ch == 3);
    }
    // PV = d (op) lerp
    for (uint32_t ch = 0; ch < 4; ++ch) {
        AluSrc dv = (ch < 3) ? resolve_cc_full(cfg, a, s, cc->d, ch)
                             : resolve_ca(cfg, a, s, ca->d);
        bool sub = (ch < 3) ? (cc->op == GX_TEV_SUB) : (ca->op == GX_TEV_SUB);
        buf[n++] = r700_alu_op2(R700_OP2_ADD, TEV_PS_PV_GPR, ch,
                                dv.sel, dv.chan, false,
                                R700_ALU_SRC_PV, ch, sub,
                                false, true, ch == 3);
    }
    // dest = clamp( scale * PV + bias*scale )
    float cscale = scale_value(cc->scale);
    float ascale = scale_value(ca->scale);
    float cbias  = bias_value(cc->bias) * cscale;
    float abias  = bias_value(ca->bias) * ascale;
    for (uint32_t ch = 0; ch < 4; ++ch) {
        bool alpha = ch == 3;
        uint32_t dst = a->reg[alpha ? ca->dest : cc->dest];
        uint32_t sc_chan = alpha ? R700_CHAN_Z : R700_CHAN_X;
        uint32_t bi_chan = alpha ? R700_CHAN_W : R700_CHAN_Y;
        bool clamp = alpha ? ca->clamp : cc->clamp;
        buf[n++] = r700_alu_op3(R700_OP3_MULADD, dst, ch,
                                R700_ALU_SRC_LITERAL, sc_chan, false,
                                R700_ALU_SRC_PV, ch, false,
                                R700_ALU_SRC_LITERAL, bi_chan, false,
                                clamp, ch == 3);
    }
    buf[n++] = r700_alu_literal(f32_bits(cscale), f32_bits(cbias));  // X, Y
    buf[n++] = r700_alu_literal(f32_bits(ascale), f32_bits(abias));  // Z, W
    return n;
}

// The arithmetic combiner over the colour channels only (x/y/z).
static uint32_t emit_color_arith(const TevConfig *cfg, uint32_t s,
                                 const TevPsAlloc *a, R700Inst64 *buf)
{
    const TevCombiner *cc = &cfg->stage[s].color;
    uint32_t n = 0;
    for (uint32_t ch = 0; ch < 3; ++ch) {  // PV = b - a
        AluSrc av = resolve_cc_full(cfg, a, s, cc->a, ch);
        AluSrc bv = resolve_cc_full(cfg, a, s, cc->b, ch);
        buf[n++] = r700_alu_op2(R700_OP2_ADD, TEV_PS_PV_GPR, ch,
                                bv.sel, bv.chan, false, av.sel, av.chan, true,
                                false, true, ch == 2);
    }
    for (uint32_t ch = 0; ch < 3; ++ch) {  // PV = c * (b - a) + a
        AluSrc cv = resolve_cc_full(cfg, a, s, cc->c, ch);
        AluSrc av = resolve_cc_full(cfg, a, s, cc->a, ch);
        buf[n++] = r700_alu_op3(R700_OP3_MULADD, TEV_PS_PV_GPR, ch,
                                cv.sel, cv.chan, false,
                                R700_ALU_SRC_PV, ch, false,
                                av.sel, av.chan, false, false, ch == 2);
    }
    for (uint32_t ch = 0; ch < 3; ++ch) {  // PV = d (op) lerp
        AluSrc dv = resolve_cc_full(cfg, a, s, cc->d, ch);
        buf[n++] = r700_alu_op2(R700_OP2_ADD, TEV_PS_PV_GPR, ch,
                                dv.sel, dv.chan, false,
                                R700_ALU_SRC_PV, ch, cc->op == GX_TEV_SUB,
                                false, true, ch == 2);
    }
    float sc = scale_value(cc->scale);
    float bi = bias_value(cc->bias) * sc;
    uint32_t dst = a->reg[cc->dest];
    for (uint32_t ch = 0; ch < 3; ++ch)  // dest = clamp(scale*PV + bias*scale)
        buf[n++] = r700_alu_op3(R700_OP3_MULADD, dst, ch,
                                R700_ALU_SRC_LITERAL, R700_CHAN_X, false,
                                R700_ALU_SRC_PV, ch, false,
                                R700_ALU_SRC_LITERAL, R700_CHAN_Y, false,
                                cc->clamp, ch == 2);
    buf[n++] = r700_alu_literal(f32_bits(sc), f32_bits(bi));
    return n;
}

// The arithmetic combiner over the alpha channel only.
static uint32_t emit_alpha_arith(const TevConfig *cfg, uint32_t s,
                                 const TevPsAlloc *a, R700Inst64 *buf)
{
    const TevCombiner *ca = &cfg->stage[s].alpha;
    uint32_t n = 0;
    AluSrc av = resolve_ca(cfg, a, s, ca->a);
    AluSrc bv = resolve_ca(cfg, a, s, ca->b);
    buf[n++] = r700_alu_op2(R700_OP2_ADD, TEV_PS_PV_GPR, R700_CHAN_W,  // PV.w = b - a
                            bv.sel, bv.chan, false, av.sel, av.chan, true,
                            false, true, true);
    AluSrc cv = resolve_ca(cfg, a, s, ca->c);
    av = resolve_ca(cfg, a, s, ca->a);
    buf[n++] = r700_alu_op3(R700_OP3_MULADD, TEV_PS_PV_GPR, R700_CHAN_W,  // c*(b-a)+a
                            cv.sel, cv.chan, false,
                            R700_ALU_SRC_PV, R700_CHAN_W, false,
                            av.sel, av.chan, false, false, true);
    AluSrc dv = resolve_ca(cfg, a, s, ca->d);
    buf[n++] = r700_alu_op2(R700_OP2_ADD, TEV_PS_PV_GPR, R700_CHAN_W,  // d (op) lerp
                            dv.sel, dv.chan, false,
                            R700_ALU_SRC_PV, R700_CHAN_W, ca->op == GX_TEV_SUB,
                            false, true, true);
    float sc = scale_value(ca->scale);
    float bi = bias_value(ca->bias) * sc;
    buf[n++] = r700_alu_op3(R700_OP3_MULADD, a->reg[ca->dest], R700_CHAN_W,
                            R700_ALU_SRC_LITERAL, R700_CHAN_X, false,
                            R700_ALU_SRC_PV, R700_CHAN_W, false,
                            R700_ALU_SRC_LITERAL, R700_CHAN_Y, false,
                            ca->clamp, true);
    buf[n++] = r700_alu_literal(f32_bits(sc), f32_bits(bi));
    return n;
}

// Emit the comparison difference `a - b` that a CND select tests against zero.
// GX comparison combiner works on 8-bit integers, but here is approximated in
// the normalized float domain.
static uint32_t emit_diff(R700Inst64 *buf, const AluSrc ta[3], const AluSrc tb[3],
                          AluSrc ta_a, AluSrc tb_a, uint8_t mode, bool alpha_a8,
                          bool *per_channel)
{
    uint32_t n = 0;
    *per_channel = false;

    if (alpha_a8) {  // A8
        buf[n++] = r700_alu_op2(R700_OP2_ADD, TEV_PS_PV_GPR, R700_CHAN_X,
                                ta_a.sel, ta_a.chan, false,
                                tb_a.sel, tb_a.chan, true, false, true, true);
        return n;
    }
    if (mode == 0) {  // R8
        buf[n++] = r700_alu_op2(R700_OP2_ADD, TEV_PS_PV_GPR, R700_CHAN_X,
                                ta[0].sel, ta[0].chan, false,
                                tb[0].sel, tb[0].chan, true, false, true, true);
        return n;
    }
    if (mode == 3) {  // RGB8
        for (uint32_t ch = 0; ch < 3; ++ch)
            buf[n++] = r700_alu_op2(R700_OP2_ADD, TEV_PS_PV_GPR, ch,
                                    ta[ch].sel, ta[ch].chan, false,
                                    tb[ch].sel, tb[ch].chan, true,
                                    false, true, ch == 2);
        *per_channel = true;
        return n;
    }
    // GR16 / BGR24
    buf[n++] = r700_alu_op3(R700_OP3_MULADD, TEV_PS_PV_GPR, R700_CHAN_X,
                            ta[1].sel, ta[1].chan, false,
                            R700_ALU_SRC_LITERAL, R700_CHAN_X, false,
                            ta[0].sel, ta[0].chan, false, false, false);
    buf[n++] = r700_alu_op3(R700_OP3_MULADD, TEV_PS_PV_GPR, R700_CHAN_Y,
                            tb[1].sel, tb[1].chan, false,
                            R700_ALU_SRC_LITERAL, R700_CHAN_X, false,
                            tb[0].sel, tb[0].chan, false, false, true);
    buf[n++] = r700_alu_literal(f32_bits(256.0f), f32_bits(256.0f));
    if (mode == 2) {  // BGR24
        buf[n++] = r700_alu_op3(R700_OP3_MULADD, TEV_PS_PV_GPR, R700_CHAN_X,
                                ta[2].sel, ta[2].chan, false,
                                R700_ALU_SRC_LITERAL, R700_CHAN_X, false,
                                R700_ALU_SRC_PV, R700_CHAN_X, false, false, false);
        buf[n++] = r700_alu_op3(R700_OP3_MULADD, TEV_PS_PV_GPR, R700_CHAN_Y,
                                tb[2].sel, tb[2].chan, false,
                                R700_ALU_SRC_LITERAL, R700_CHAN_X, false,
                                R700_ALU_SRC_PV, R700_CHAN_Y, false, false, true);
        buf[n++] = r700_alu_literal(f32_bits(65536.0f), f32_bits(65536.0f));
    }
    // PV.x = dot(a) - dot(b)
    buf[n++] = r700_alu_op2(R700_OP2_ADD, TEV_PS_PV_GPR, R700_CHAN_X,
                            R700_ALU_SRC_PV, R700_CHAN_X, false,
                            R700_ALU_SRC_PV, R700_CHAN_Y, true, false, true, true);
    return n;
}

// GX comparison combiner
static uint32_t emit_color_compare(const TevConfig *cfg, uint32_t s,
                                   const TevPsAlloc *a, R700Inst64 *buf)
{
    const TevCombiner *cc = &cfg->stage[s].color;
    uint32_t cnd = (cc->op == GX_TEV_SUB) ? R700_OP3_CNDE : R700_OP3_CNDGT;
    AluSrc ta[3], tb[3];
    for (uint32_t ch = 0; ch < 3; ++ch) {
        ta[ch] = resolve_cc_full(cfg, a, s, cc->a, ch);
        tb[ch] = resolve_cc_full(cfg, a, s, cc->b, ch);
    }
    AluSrc unused = {R700_ALU_SRC_0, 0};
    bool per_channel;
    uint32_t n = emit_diff(buf, ta, tb, unused, unused, cc->scale, false,
                           &per_channel);
    for (uint32_t ch = 0; ch < 3; ++ch) {  // sel.ch = compare ? c.ch : 0
        AluSrc cv = resolve_cc_full(cfg, a, s, cc->c, ch);
        uint32_t dchan = per_channel ? ch : R700_CHAN_X;
        buf[n++] = r700_alu_op3(cnd, TEV_PS_PV_GPR, ch,
                                R700_ALU_SRC_PV, dchan, false,
                                cv.sel, cv.chan, false,
                                R700_ALU_SRC_0, 0, false, false, ch == 2);
    }
    uint32_t dst = a->reg[cc->dest];
    for (uint32_t ch = 0; ch < 3; ++ch) {  // dest.ch = d.ch + sel.ch
        AluSrc dv = resolve_cc_full(cfg, a, s, cc->d, ch);
        buf[n++] = r700_alu_op2(R700_OP2_ADD, dst, ch,
                                R700_ALU_SRC_PV, ch, false,
                                dv.sel, dv.chan, false, cc->clamp, true, ch == 2);
    }
    return n;
}

// GX comparison combiner for alpha
static uint32_t emit_alpha_compare(const TevConfig *cfg, uint32_t s,
                                   const TevPsAlloc *a, R700Inst64 *buf)
{
    const TevCombiner *cc = &cfg->stage[s].color;
    const TevCombiner *ca = &cfg->stage[s].alpha;
    uint32_t cnd = (ca->op == GX_TEV_SUB) ? R700_OP3_CNDE : R700_OP3_CNDGT;
    bool a8 = ca->scale == 3;
    AluSrc ta[3], tb[3];
    for (uint32_t ch = 0; ch < 3; ++ch) {
        ta[ch] = resolve_cc_full(cfg, a, s, cc->a, ch);
        tb[ch] = resolve_cc_full(cfg, a, s, cc->b, ch);
    }
    AluSrc ta_a = resolve_ca(cfg, a, s, ca->a);
    AluSrc tb_a = resolve_ca(cfg, a, s, ca->b);
    bool per_channel;
    uint32_t n = emit_diff(buf, ta, tb, ta_a, tb_a, ca->scale, a8, &per_channel);

    AluSrc cv = resolve_ca(cfg, a, s, ca->c);  // sel.w = compare ? c.a : 0
    buf[n++] = r700_alu_op3(cnd, TEV_PS_PV_GPR, R700_CHAN_W,
                            R700_ALU_SRC_PV, R700_CHAN_X, false,
                            cv.sel, cv.chan, false,
                            R700_ALU_SRC_0, 0, false, false, true);
    AluSrc dv = resolve_ca(cfg, a, s, ca->d);  // dest.w = d.a + sel.w
    buf[n++] = r700_alu_op2(R700_OP2_ADD, a->reg[ca->dest], R700_CHAN_W,
                            R700_ALU_SRC_PV, R700_CHAN_W, false,
                            dv.sel, dv.chan, false, ca->clamp, true, true);
    return n;
}

// Emit one stage's combiner into the ALU body buffer.
static uint32_t emit_stage(const TevConfig *cfg, uint32_t s, const TevPsAlloc *a,
                           R700Inst64 *buf)
{
    const TevStage *st = &cfg->stage[s];
    uint32_t n = 0;

    if (stage_uses_konst(st)) n += emit_konst_group(st, a, buf + n);

    bool ccmp = st->color.bias == GX_TB_COMPARE;
    bool acmp = st->alpha.bias == GX_TB_COMPARE;

    if (!ccmp && !acmp) {
        n += emit_stage_arith(cfg, s, a, buf + n);
    } else {
        n += ccmp ? emit_color_compare(cfg, s, a, buf + n)
                  : emit_color_arith(cfg, s, a, buf + n);
        n += acmp ? emit_alpha_compare(cfg, s, a, buf + n)
                  : emit_alpha_arith(cfg, s, a, buf + n);
    }
    return n;
}

// Emit the reg-file initialisation clause
static uint32_t emit_reg_init(const TevPsAlloc *a, R700Inst64 *buf)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < 4; ++i)
        for (uint32_t ch = 0; ch < 4; ++ch)
            buf[n++] = r700_alu_op2(R700_OP2_MOV, a->reg[i], ch,
                                    R700_ALU_SRC_CFILE(TEV_PS_REG_CFILE_BASE + i),
                                    ch, false, R700_ALU_SRC_0, 0, false,
                                    false, true, ch == 3);
    return n;
}

size_t tev_shader_gen_ps(uint8_t *buf, size_t cap, const TevConfig *cfg)
{
    if (!buf || !cfg || cfg->num_stages == 0 ||
        cfg->num_stages > GX_TEV_MAX_STAGES)
        return 0;

    TevPsAlloc a;
    plan_alloc(cfg, &a);

    R700Program p;
    if (!r700_prog_begin(&p, buf, cap, TEV_PS_MAX_CF)) return 0;

    // Reg-file init
    R700Inst64 body[R700_PROG_MAX_CLAUSE];
    uint32_t n = emit_reg_init(&a, body);
    if (!r700_prog_alu_clause(&p, body, n, true)) return 0;

    for (uint32_t s = 0; s < cfg->num_stages; ++s) {
        const TevStage *st = &cfg->stage[s];
        if (st->tex_enable) {
            uint32_t coord = a.coord_gpr[st->texcoord & 7u];
            R700Inst128 tex = r700_tex_sample(st->texmap, st->texmap, coord,
                                              a.tex, kCoord2D, kSelXYZW);
            if (!r700_prog_tex_clause(&p, &tex, 1, true, true)) return 0;
        }
        n = emit_stage(cfg, s, &a, body);
        if (n > R700_PROG_MAX_CLAUSE) return 0;
        if (!r700_prog_alu_clause(&p, body, n, true)) return 0;
    }

    // Export PREV as the pixel colour.
    r700_prog_cf(&p, r700_cf_export(R700_EXPORT_PIXEL, 0, a.reg[0], kSelXYZW,
                                    true, true, R700_CF_EXPORT_DONE));
    return r700_prog_finalize(&p);
}

void tev_shader_gen_ps_shape(const TevConfig *cfg, Gx2PsShape *out)
{
    memset(out, 0, sizeof(*out));
    TevPsAlloc a;
    plan_alloc(cfg, &a);

    out->input_semantics[0] = 0;  // rasterised colour
    uint32_t inputs = 1;
    for (uint32_t tc = 0; tc < 8; ++tc)
        if (a.coord_used[tc]) {
            out->input_semantics[inputs] = (uint8_t)inputs;  // consecutive param ids
            ++inputs;
        }
    out->num_inputs = inputs;
    out->num_gprs = a.num_gprs;
    out->stack_size = 0;
    out->num_color_exports = 1;
}

// Self test

static uint32_t rd_le32(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

// Walk the CF section
static bool walk_cf(const uint8_t *prog, size_t size, uint32_t *tex_clauses,
                    uint32_t *alu_clauses, uint32_t *export_gpr)
{
    *tex_clauses = *alu_clauses = 0;
    *export_gpr = 0xFFFFFFFFu;
    for (size_t i = 0;; ++i) {
        if ((i + 1) * 8 > size) return false;
        uint32_t w0 = rd_le32(prog + i * 8);
        uint32_t w1 = rd_le32(prog + i * 8 + 4);

        if (w1 & (1u << 29)) {          // CF_ALU
            ++*alu_clauses;
            continue;
        }
        uint32_t inst = (w1 >> 23) & 0x7Fu;
        if (inst == R700_CF_TEX) {
            ++*tex_clauses;
        } else if (inst == R700_CF_EXPORT_DONE) {
            if (((w0 >> 13) & 3u) == R700_EXPORT_PIXEL)
                *export_gpr = (w0 >> 15) & 0x7Fu;
        }
        if (w1 & (1u << 21)) return true;  // END_OF_PROGRAM
    }
}

bool tev_shader_gen_selftest(void)
{
    static uint8_t ps[8192];

    // PREV = TEXC * RASC, alpha likewise.
    TevConfig cfg;
    gx_tev_reset(&cfg);
    cfg.num_stages = 1;
    TevStage *s0 = &cfg.stage[0];
    s0->color = (TevCombiner){.a = GX_CC_ZERO, .b = GX_CC_RASC, .c = GX_CC_TEXC,
                              .d = GX_CC_ZERO, .bias = GX_TB_ZERO, .op = GX_TEV_ADD,
                              .clamp = true, .scale = GX_TS_1, .dest = GX_TEVOUT_PREV};
    s0->alpha = (TevCombiner){.a = GX_CA_ZERO, .b = GX_CA_RASA, .c = GX_CA_TEXA,
                              .d = GX_CA_ZERO, .bias = GX_TB_ZERO, .op = GX_TEV_ADD,
                              .clamp = true, .scale = GX_TS_1, .dest = GX_TEVOUT_PREV};
    s0->tex_enable = true;
    s0->texmap = 0;
    s0->texcoord = 0;
    s0->colorchan = GX_RAS_COLOR0;

    size_t n = tev_shader_gen_ps(ps, sizeof(ps), &cfg);
    if (n == 0) return false;

    uint32_t tex_c, alu_c, exp_gpr;
    if (!walk_cf(ps, n, &tex_c, &alu_c, &exp_gpr)) return false;
    // reg-init + one stage
    if (tex_c != 1 || alu_c != 2) return false;

    Gx2PsShape shape;
    tev_shader_gen_ps_shape(&cfg, &shape);
    // R0 colour + R1 texcoord interpolants
    if (shape.num_inputs != 2) return false;
    if (shape.num_gprs != 8) return false;   // 2 interpolants + 4 regs + 2 scratch
    if (exp_gpr != 2) return false;          // PREV lives at R2 here

    Gx2PsRegs pr;
    if (!gx2_ps_regs(&pr, &shape)) return false;

    // A three-stage config with konst + two textures must build and grow.
    gx_tev_reset(&cfg);
    cfg.num_stages = 3;
    for (uint32_t s = 0; s < 3; ++s) {
        TevStage *st = &cfg.stage[s];
        st->color = (TevCombiner){.a = GX_CC_CPREV, .b = GX_CC_TEXC, .c = GX_CC_KONST,
                                  .d = GX_CC_ZERO, .bias = GX_TB_ADDHALF,
                                  .op = GX_TEV_ADD, .clamp = true, .scale = GX_TS_2,
                                  .dest = GX_TEVOUT_PREV};
        st->alpha = (TevCombiner){.a = GX_CA_APREV, .b = GX_CA_TEXA, .c = GX_CA_KONST,
                                  .d = GX_CA_ZERO, .bias = GX_TB_ZERO, .op = GX_TEV_ADD,
                                  .clamp = true, .scale = GX_TS_1, .dest = GX_TEVOUT_PREV};
        st->tex_enable = true;
        st->texmap = (uint8_t)s;
        st->texcoord = (uint8_t)(s & 1u);
        st->colorchan = GX_RAS_COLOR0;
        st->kcsel = 12;  // K0 rgb
        st->kasel = 28;  // K0 alpha
    }
    n = tev_shader_gen_ps(ps, sizeof(ps), &cfg);
    if (n == 0) return false;
    if (!walk_cf(ps, n, &tex_c, &alu_c, &exp_gpr)) return false;
    if (tex_c != 3 || alu_c != 4) return false;

    tev_shader_gen_ps_shape(&cfg, &shape);
    if (shape.num_inputs != 3) return false;      // colour + 2 texcoords
    if (exp_gpr != 3) return false;               // PREV at R3
    if (shape.num_gprs != 9) return false;        // 3 interpolants + 4 regs + 2 scratch
    if (!gx2_ps_regs(&pr, &shape)) return false;

    gx_tev_reset(&cfg);
    cfg.num_stages = 1;
    TevStage *sc = &cfg.stage[0];
    sc->color = (TevCombiner){.a = GX_CC_TEXC, .b = GX_CC_CPREV, .c = GX_CC_C0,
                              .d = GX_CC_CPREV, .bias = GX_TB_COMPARE,
                              .op = GX_TEV_ADD /*GT*/, .clamp = true,
                              .scale = 1 /*GR16*/, .dest = GX_TEVOUT_PREV};
    sc->alpha = (TevCombiner){.a = GX_CA_TEXA, .b = GX_CA_APREV, .c = GX_CA_A0,
                              .d = GX_CA_APREV, .bias = GX_TB_COMPARE,
                              .op = GX_TEV_SUB /*EQ*/, .clamp = true,
                              .scale = 3 /*A8*/, .dest = GX_TEVOUT_PREV};
    sc->tex_enable = true;
    sc->texmap = 0;
    sc->texcoord = 0;
    sc->colorchan = GX_RAS_COLOR0;

    n = tev_shader_gen_ps(ps, sizeof(ps), &cfg);
    if (n == 0) return false;
    if (!walk_cf(ps, n, &tex_c, &alu_c, &exp_gpr)) return false;
    if (tex_c != 1 || alu_c != 2) return false;   // reg-init + one compare stage
    if (exp_gpr != 2) return false;               // PREV at R2

    return true;
}
