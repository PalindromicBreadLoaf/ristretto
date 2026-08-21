// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu/r700_emit.h"

#include <string.h>

// Instruction encoders

R700Inst64 r700_cf(uint32_t addr, uint32_t inst, uint32_t count,
                   bool valid_pixel_mode, bool end_of_program, bool barrier)
{
    uint32_t cf = count ? count - 1u : 0u;
    R700Inst64 out;
    out.w0 = addr & 0x00FFFFFFu;
    out.w1 = ((cf & 0x7u) << 10)
           | (((cf >> 3) & 0x1u) << 19)
           | ((uint32_t)end_of_program << 21)
           | ((uint32_t)valid_pixel_mode << 22)
           | ((inst & 0x7Fu) << 23)
           | ((uint32_t)barrier << 31);
    return out;
}

R700Inst64 r700_cf_alu(uint32_t addr, uint32_t slots, bool barrier)
{
    uint32_t cf = slots ? slots - 1u : 0u;
    R700Inst64 out;
    out.w0 = addr & 0x003FFFFFu;  // kcache banks/modes left zero
    out.w1 = ((cf & 0x7Fu) << 18)
           | ((uint32_t)R700_CF_ALU << 26)
           | ((uint32_t)barrier << 31);
    return out;
}

R700Inst64 r700_cf_export(uint32_t type, uint32_t array_base, uint32_t src_gpr,
                          const uint8_t sel[4], bool end_of_program,
                          bool barrier, uint32_t inst)
{
    uint32_t swz = (sel[0] & 0x7u)
                 | ((sel[1] & 0x7u) << 3)
                 | ((sel[2] & 0x7u) << 6)
                 | ((sel[3] & 0x7u) << 9);
    R700Inst64 out;
    out.w0 = (array_base & 0x1FFFu)
           | ((type & 0x3u) << 13)
           | ((src_gpr & 0x7Fu) << 15);
    out.w1 = swz
           | ((uint32_t)end_of_program << 21)
           | ((inst & 0x7Fu) << 23)
           | ((uint32_t)barrier << 31);
    return out;
}

R700Inst64 r700_alu_op2(uint32_t op, uint32_t dst_gpr, uint32_t dst_chan,
                        uint32_t s0_sel, uint32_t s0_chan, bool s0_neg,
                        uint32_t s1_sel, uint32_t s1_chan, bool s1_neg,
                        bool clamp, bool write_mask, bool last)
{
    R700Inst64 out;
    out.w0 = (s0_sel & 0x1FFu)
           | ((s0_chan & 0x3u) << 10)
           | ((uint32_t)s0_neg << 12)
           | ((s1_sel & 0x1FFu) << 13)
           | ((s1_chan & 0x3u) << 23)
           | ((uint32_t)s1_neg << 25)
           | ((uint32_t)last << 31);
    out.w1 = ((uint32_t)write_mask << 4)
           | ((op & 0x7FFu) << 7)
           | ((dst_gpr & 0x7Fu) << 21)
           | ((dst_chan & 0x3u) << 29)
           | ((uint32_t)clamp << 31);
    return out;
}

R700Inst64 r700_alu_op3(uint32_t op, uint32_t dst_gpr, uint32_t dst_chan,
                        uint32_t s0_sel, uint32_t s0_chan, bool s0_neg,
                        uint32_t s1_sel, uint32_t s1_chan, bool s1_neg,
                        uint32_t s2_sel, uint32_t s2_chan, bool s2_neg,
                        bool clamp, bool last)
{
    R700Inst64 out;
    out.w0 = (s0_sel & 0x1FFu)
           | ((s0_chan & 0x3u) << 10)
           | ((uint32_t)s0_neg << 12)
           | ((s1_sel & 0x1FFu) << 13)
           | ((s1_chan & 0x3u) << 23)
           | ((uint32_t)s1_neg << 25)
           | ((uint32_t)last << 31);
    out.w1 = (s2_sel & 0x1FFu)
           | ((s2_chan & 0x3u) << 10)
           | ((uint32_t)s2_neg << 12)
           | ((op & 0x1Fu) << 13)
           | ((dst_gpr & 0x7Fu) << 21)
           | ((dst_chan & 0x3u) << 29)
           | ((uint32_t)clamp << 31);
    return out;
}

R700Inst64 r700_alu_literal(uint32_t x_bits, uint32_t y_bits)
{
    R700Inst64 out;
    out.w0 = x_bits;
    out.w1 = y_bits;
    return out;
}

R700Inst128 r700_tex_sample(uint32_t resource_id, uint32_t sampler_id,
                            uint32_t src_gpr, uint32_t dst_gpr,
                            const uint8_t coord_sel[4], const uint8_t dst_sel[4])
{
    R700Inst128 out;
    out.w[0] = (uint32_t)R700_TEX_SAMPLE
             | ((resource_id & 0xFFu) << 8)
             | ((src_gpr & 0x7Fu) << 16);
    out.w[1] = (dst_gpr & 0x7Fu)
             | ((dst_sel[0] & 0x7u) << 9)
             | ((dst_sel[1] & 0x7u) << 12)
             | ((dst_sel[2] & 0x7u) << 15)
             | ((dst_sel[3] & 0x7u) << 18)
             | (0xFu << 28);  // all coords normalized
    // WORD2: SAMPLER_ID[19:15], SRC_SEL_{X,Y,Z,W} at 20/23/26/29.
    out.w[2] = ((sampler_id & 0x1Fu) << 15)
             | ((coord_sel[0] & 0x7u) << 20)
             | ((coord_sel[1] & 0x7u) << 23)
             | ((coord_sel[2] & 0x7u) << 26)
             | ((coord_sel[3] & 0x7u) << 29);
    out.w[3] = 0;
    return out;
}

// Program assembler

static uint32_t align_up(uint32_t v, uint32_t a) { return (v + (a - 1)) & ~(a - 1); }

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

bool r700_prog_begin(R700Program *p, uint8_t *buf, size_t cap, uint32_t max_cf)
{
    if (max_cf > R700_PROG_MAX_CF) return false;
    uint32_t cf_bytes = align_up(max_cf * 8u, 16u);
    if (cf_bytes > cap) return false;
    memset(p, 0, sizeof(*p));
    memset(buf, 0, cap);
    p->buf = buf;
    p->cap = cap;
    p->body_offset = cf_bytes;
    return true;
}

uint32_t r700_prog_cf(R700Program *p, R700Inst64 word)
{
    if (p->cf_count >= R700_PROG_MAX_CF) { p->overflow = true; return 0; }
    uint32_t idx = p->cf_count++;
    p->cf[idx] = word;
    return idx;
}

// Write a clause body at an aligned offset and record it for CF-address
// resolution.
static uint32_t append_body(R700Program *p, const void *body, uint32_t bytes,
                            uint32_t align)
{
    uint32_t off = align_up(p->body_offset, align);
    if ((size_t)off + bytes > p->cap || p->clause_count >= R700_PROG_MAX_CLAUSE) {
        p->overflow = true;
        return UINT32_MAX;
    }
    memcpy(p->buf + off, body, bytes);
    p->body_offset = off + bytes;
    p->clause_count++;
    return off;
}

uint32_t r700_prog_alu_body(R700Program *p, const R700Inst64 *body, uint32_t slots)
{
    uint8_t words[R700_PROG_MAX_CLAUSE * 8];
    if (slots * 8u > sizeof(words)) { p->overflow = true; return UINT32_MAX; }
    for (uint32_t i = 0; i < slots; ++i) {
        put32(words + i * 8, body[i].w0);
        put32(words + i * 8 + 4, body[i].w1);
    }
    uint32_t off = append_body(p, words, slots * 8u, 16u);
    return off == UINT32_MAX ? UINT32_MAX : off / 8u;
}

uint32_t r700_prog_tex_body(R700Program *p, const R700Inst128 *body, uint32_t count)
{
    uint8_t words[R700_PROG_MAX_CLAUSE * 16];
    if (count * 16u > sizeof(words)) { p->overflow = true; return UINT32_MAX; }
    for (uint32_t i = 0; i < count; ++i)
        for (uint32_t w = 0; w < 4; ++w)
            put32(words + i * 16 + w * 4, body[i].w[w]);
    uint32_t off = append_body(p, words, count * 16u, 16u);
    return off == UINT32_MAX ? UINT32_MAX : off / 8u;
}

bool r700_prog_alu_clause(R700Program *p, const R700Inst64 *body, uint32_t slots,
                          bool barrier)
{
    uint32_t addr = r700_prog_alu_body(p, body, slots);
    if (addr == UINT32_MAX) return false;
    r700_prog_cf(p, r700_cf_alu(addr, slots, barrier));
    return !p->overflow;
}

bool r700_prog_tex_clause(R700Program *p, const R700Inst128 *body, uint32_t count,
                          bool valid_pixel_mode, bool barrier)
{
    uint32_t addr = r700_prog_tex_body(p, body, count);
    if (addr == UINT32_MAX) return false;
    r700_prog_cf(p, r700_cf(addr, R700_CF_TEX, count, valid_pixel_mode,
                            false, barrier));
    return !p->overflow;
}

size_t r700_prog_finalize(R700Program *p)
{
    if (p->overflow) return 0;
    for (uint32_t i = 0; i < p->cf_count; ++i) {
        put32(p->buf + i * 8, p->cf[i].w0);
        put32(p->buf + i * 8 + 4, p->cf[i].w1);
    }
    return p->body_offset;
}

// Self test
static bool expect64(R700Inst64 got, uint32_t w0, uint32_t w1)
{
    return got.w0 == w0 && got.w1 == w1;
}

bool r700_emit_selftest(void)
{
    static const uint8_t sel_xyzw[4] = {0, 1, 2, 3};

    // CALL_FS (vertex program CF[0]).
    if (!expect64(r700_cf(0, R700_CF_CALL_FS, 0, false, false, false),
                  0x00000000, 0x09800000))
        return false;

    // TEX clause start
    if (!expect64(r700_cf(0x30, R700_CF_TEX, 1, true, false, true),
                  0x00000030, 0x80c00000))
        return false;

    // ALU clause start
    if (!expect64(r700_cf_alu(0x20, 4, true), 0x00000020, 0xa00c0000))
        return false;

    // Pixel export
    if (!expect64(r700_cf_export(R700_EXPORT_PIXEL, 0, 0, sel_xyzw, true, true,
                                 R700_CF_EXPORT_DONE),
                  0x00000000, 0x94200688))
        return false;

    // Vertex position export
    if (!expect64(r700_cf_export(R700_EXPORT_POS, 60, 1, sel_xyzw, false, true,
                                 R700_CF_EXPORT_DONE),
                  0x0000a03c, 0x94000688))
        return false;

    // The four modulate MULs
    // gpr0.c = gpr0.c * gpr1.c for c in x,y,z,w.
    static const uint32_t mul_golden[4][2] = {
        {0x00002000, 0x00000090},
        {0x00802400, 0x20000090},
        {0x01002800, 0x40000090},
        {0x81802c00, 0x60000090},
    };
    for (uint32_t c = 0; c < 4; ++c) {
        R700Inst64 alu = r700_alu_op2(R700_OP2_MUL, 0, c, 0, c, false, 1, c,
                                      false, false, true, c == 3);
        if (!expect64(alu, mul_golden[c][0], mul_golden[c][1]))
            return false;
    }

    // OP3 MULADD
    // R1.x = R1.x * cfile[0].x + PV.x
    static const uint8_t sel_xy0x[4] = {R700_SEL_X, R700_SEL_Y, R700_SEL_0, R700_SEL_X};
    R700Inst64 madd = r700_alu_op3(R700_OP3_MULADD, 1, R700_CHAN_X,
                                   1, R700_CHAN_X, false,
                                   R700_ALU_SRC_CFILE(0), R700_CHAN_X, false,
                                   R700_ALU_SRC_PV, R700_CHAN_X, false,
                                   false, false);
    if (!expect64(madd, 0x00200001, 0x002200fe)) return false;

    // MUL R0.x = literal.x * cfile[3].x, write_mask 0.
    R700Inst64 wterm = r700_alu_op2(R700_OP2_MUL, 0, R700_CHAN_X,
                                    R700_ALU_SRC_LITERAL, R700_CHAN_X, false,
                                    R700_ALU_SRC_CFILE(3), R700_CHAN_X, false,
                                    false, false, false);
    if (!expect64(wterm, 0x002060fd, 0x00000080)) return false;

    // TEX SAMPLE
    R700Inst128 tex = r700_tex_sample(0, 0, 1, 1, sel_xy0x, sel_xyzw);
    if (tex.w[0] != 0x00010010 || tex.w[1] != 0xf00d1001 ||
        tex.w[2] != 0x10800000 || tex.w[3] != 0x00000000)
        return false;

    // Assemble a minimal pixel program and check clause addresses resolve.
    static uint8_t buf[512];
    R700Program prog;
    if (!r700_prog_begin(&prog, buf, sizeof(buf), 8)) return false;
    R700Inst128 texbody[1] = {tex};
    R700Inst64 mulbody[4];
    for (uint32_t c = 0; c < 4; ++c)
        mulbody[c] = r700_alu_op2(R700_OP2_MUL, 0, c, 0, c, false, 1, c, false,
                                  false, true, c == 3);
    if (!r700_prog_tex_clause(&prog, texbody, 1, true, true)) return false;
    if (!r700_prog_alu_clause(&prog, mulbody, 4, true)) return false;
    r700_prog_cf(&prog, r700_cf_export(R700_EXPORT_PIXEL, 0, 0, sel_xyzw, true,
                                       true, R700_CF_EXPORT_DONE));
    size_t size = r700_prog_finalize(&prog);
    if (size == 0) return false;

    uint32_t cf0_addr = (buf[0] | (buf[1] << 8) | (buf[2] << 16)) & 0x00FFFFFFu;
    if (cf0_addr != 0x40 / 8) return false;

    return true;
}
