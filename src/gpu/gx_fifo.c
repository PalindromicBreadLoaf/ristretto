// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu/gx_fifo.h"

#include <string.h>

static uint16_t rd_be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static uint32_t rd_be24(const uint8_t *p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}
static uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// GX depth-list recursion limit
#define GX_DL_MAX_DEPTH 4

static uint32_t elem_size(uint32_t fmt) {
    if (fmt <= 1) return 1;
    if (fmt <= 3) return 2;
    return 4;
}

// VertexComponentFormat: 0 NotPresent, 1 Direct, 2 Index8, 3 Index16.
static uint32_t pos_size(uint32_t vcf, uint32_t fmt, uint32_t xyz) {
    switch (vcf) {
    case 1:  return elem_size(fmt) * (xyz ? 3u : 2u);
    case 2:  return 1;
    case 3:  return 2;
    default: return 0;
    }
}

static uint32_t tex_size(uint32_t vcf, uint32_t fmt, uint32_t st) {
    switch (vcf) {
    case 1:  return elem_size(fmt) * (st ? 2u : 1u);
    case 2:  return 1;
    case 3:  return 2;
    default: return 0;
    }
}

static uint32_t color_size(uint32_t vcf, uint32_t colfmt) {
    // Direct sizes by ColorFormat: 565=2 888=3 888x=4 4444=2 6666=3 8888=4.
    static const uint32_t direct[6] = {2, 3, 4, 2, 3, 4};
    switch (vcf) {
    case 1:  return colfmt < 6 ? direct[colfmt] : 0;
    case 2:  return 1;
    case 3:  return 2;
    default: return 0;
    }
}

static uint32_t normal_size(uint32_t vcf, uint32_t fmt, uint32_t ntb, uint32_t index3) {
    switch (vcf) {
    case 1:  return elem_size(fmt) * (ntb ? 9u : 3u);
    case 2:  return ntb ? (index3 ? 3u : 1u) : 1u;  // Index8
    case 3:  return ntb ? (index3 ? 6u : 2u) : 2u;  // Index16
    default: return 0;
    }
}

// Pull the (elements, format) VAT fields for texture coordinate `i` (0..7).
static void tex_vat(uint32_t g0, uint32_t g1, uint32_t g2, unsigned i,
                    uint32_t *elem, uint32_t *fmt) {
    switch (i) {
    case 0: *elem = (g0 >> 21) & 1; *fmt = (g0 >> 22) & 7; break;
    case 1: *elem = (g1 >> 0)  & 1; *fmt = (g1 >> 1)  & 7; break;
    case 2: *elem = (g1 >> 9)  & 1; *fmt = (g1 >> 10) & 7; break;
    case 3: *elem = (g1 >> 18) & 1; *fmt = (g1 >> 19) & 7; break;
    case 4: *elem = (g1 >> 27) & 1; *fmt = (g1 >> 28) & 7; break;
    case 5: *elem = (g2 >> 5)  & 1; *fmt = (g2 >> 6)  & 7; break;
    case 6: *elem = (g2 >> 14) & 1; *fmt = (g2 >> 15) & 7; break;
    default:*elem = (g2 >> 23) & 1; *fmt = (g2 >> 24) & 7; break;
    }
}

void gx_fifo_state_reset(GXFifoState *state) {
    memset(state, 0, sizeof(*state));
}

void gx_fifo_apply_cp(GXFifoState *state, uint8_t sub_command, uint32_t value) {
    const uint8_t group = sub_command & GX_CP_COMMAND_MASK;
    const uint8_t idx = sub_command & GX_CP_INDEX_MASK;
    switch (group) {
    case GX_CP_VCD_LO: state->desc_lo = value; break;
    case GX_CP_VCD_HI: state->desc_hi = value; break;
    case GX_CP_VAT_A:  state->vat[idx].g0 = value; break;
    case GX_CP_VAT_B:  state->vat[idx].g1 = value; break;
    case GX_CP_VAT_C:  state->vat[idx].g2 = value; break;
    default: break;
    }
}

uint32_t gx_fifo_vertex_size(const GXFifoState *state, uint8_t vat) {
    const uint32_t lo = state->desc_lo;
    const uint32_t hi = state->desc_hi;
    const uint32_t g0 = state->vat[vat & 7].g0;
    const uint32_t g1 = state->vat[vat & 7].g1;
    const uint32_t g2 = state->vat[vat & 7].g2;

    // PosMatIdx (bit 0) + 8 TexMatIdx (bits 1..8).
    uint32_t size = (uint32_t)__builtin_popcount(lo & 0x1FF);

    size += pos_size((lo >> 9) & 3, (g0 >> 1) & 7, g0 & 1);
    size += normal_size((lo >> 11) & 3, (g0 >> 10) & 7, (g0 >> 9) & 1, (g0 >> 31) & 1);

    const uint32_t col_comp[2] = {(g0 >> 14) & 7, (g0 >> 18) & 7};
    for (unsigned i = 0; i < 2; i++)
        size += color_size((lo >> (13 + 2 * i)) & 3, col_comp[i]);

    for (unsigned i = 0; i < 8; i++) {
        uint32_t elem, fmt;
        tex_vat(g0, g1, g2, i, &elem, &fmt);
        size += tex_size((hi >> (2 * i)) & 3, fmt, elem);
    }

    return size;
}

static size_t run_impl(GXFifoState *state, const uint8_t *data, size_t avail,
                       const GXFifoSink *sink, void *user, int depth) {
    size_t pos = 0;
    while (pos < avail) {
        const uint8_t *d = data + pos;
        const size_t rem = avail - pos;
        const uint8_t op = d[0];

        if (op == GX_OPCODE_NOP) {
            size_t n = 1;
            while (n < rem && d[n] == GX_OPCODE_NOP) n++;
            if (sink && sink->on_nop) sink->on_nop(user, (uint32_t)n);
            pos += n;
            continue;
        }

        switch (op) {
        case GX_OPCODE_LOAD_CP_REG: {
            if (rem < 6) return pos;
            const uint8_t sub = d[1];
            const uint32_t val = rd_be32(&d[2]);
            gx_fifo_apply_cp(state, sub, val);
            if (sink && sink->on_cp) sink->on_cp(user, sub, val);
            pos += 6;
            continue;
        }
        case GX_OPCODE_LOAD_XF_REG: {
            if (rem < 5) return pos;
            const uint32_t cmd2 = rd_be32(&d[1]);
            const uint16_t address = cmd2 & 0xFFFF;
            const uint8_t count = ((cmd2 >> 16) & 0xF) + 1;
            const size_t total = 5 + (size_t)count * 4;
            if (rem < total) return pos;
            if (sink && sink->on_xf) sink->on_xf(user, address, count, &d[5]);
            pos += total;
            continue;
        }
        case GX_OPCODE_LOAD_INDX_A:
        case GX_OPCODE_LOAD_INDX_B:
        case GX_OPCODE_LOAD_INDX_C:
        case GX_OPCODE_LOAD_INDX_D: {
            if (rem < 5) return pos;
            const uint32_t val = rd_be32(&d[1]);
            const uint32_t index = val >> 16;
            const uint16_t address = val & 0xFFF;
            const uint8_t size = ((val >> 12) & 0xF) + 1;
            const uint8_t array = (op / 8) + 8;
            if (sink && sink->on_indexed) sink->on_indexed(user, array, index, address, size);
            pos += 5;
            continue;
        }
        case GX_OPCODE_CALL_DL: {
            if (rem < 9) return pos;
            const uint32_t addr = rd_be32(&d[1]) & ~31u;
            const uint32_t size = rd_be32(&d[5]) & ~31u;
            if (sink && sink->on_display_list) sink->on_display_list(user, addr, size);
            if (sink && sink->resolve_dl && depth < GX_DL_MAX_DEPTH) {
                const uint8_t *dl = sink->resolve_dl(user, addr, size);
                if (dl) run_impl(state, dl, size, sink, user, depth + 1);
            }
            pos += 9;
            continue;
        }
        case GX_OPCODE_LOAD_BP_REG: {
            if (rem < 5) return pos;
            const uint8_t cmd = d[1];
            const uint32_t val = rd_be24(&d[2]);
            if (sink && sink->on_bp) sink->on_bp(user, cmd, val);
            pos += 5;
            continue;
        }
        default:
            if (op >= GX_OPCODE_PRIM_START && op <= GX_OPCODE_PRIM_END) {
                if (rem < 3) return pos;
                const GXPrimitive prim = (GXPrimitive)((op & 0x78) >> 3);
                const uint8_t vat = op & 0x07;
                const uint16_t num_vertices = rd_be16(&d[1]);
                const uint32_t vertex_size = gx_fifo_vertex_size(state, vat);
                const size_t total = 3 + (size_t)num_vertices * vertex_size;
                if (rem < total) return pos;
                if (sink && sink->on_primitive)
                    sink->on_primitive(user, prim, vat, vertex_size, num_vertices, &d[3]);
                pos += total;
                continue;
            }
            if (sink && sink->on_unknown) sink->on_unknown(user, op);
            pos += 1;
            continue;
        }
    }
    return pos;
}

size_t gx_fifo_run(GXFifoState *state, const uint8_t *data, size_t avail,
                   const GXFifoSink *sink, void *user) {
    return run_impl(state, data, avail, sink, user, 0);
}

// Self test
typedef struct {
    uint32_t nop_runs, total_nops;
    uint32_t cp, xf, bp, indexed, prim, dl, unknown;
    uint32_t prim_vsize, prim_nverts, indexed_array;
    const uint8_t *dl_buf;
    uint32_t dl_len;
} GXTestStats;

static void t_nop(void *u, uint32_t c) { GXTestStats *s = u; s->nop_runs++; s->total_nops += c; }
static void t_cp(void *u, uint8_t sub, uint32_t v) { (void)sub; (void)v; ((GXTestStats *)u)->cp++; }
static void t_xf(void *u, uint16_t a, uint8_t c, const uint8_t *d) { (void)a; (void)c; (void)d; ((GXTestStats *)u)->xf++; }
static void t_bp(void *u, uint8_t c, uint32_t v) { (void)c; (void)v; ((GXTestStats *)u)->bp++; }
static void t_indexed(void *u, uint8_t arr, uint32_t i, uint16_t a, uint8_t sz) {
    (void)i; (void)a; (void)sz;
    GXTestStats *s = u; s->indexed++; s->indexed_array = arr;
}
static void t_prim(void *u, GXPrimitive p, uint8_t vat, uint32_t vs, uint16_t nv, const uint8_t *d) {
    (void)p; (void)vat; (void)d;
    GXTestStats *s = u; s->prim++; s->prim_vsize = vs; s->prim_nverts = nv;
}
static void t_dl(void *u, uint32_t a, uint32_t sz) { (void)a; (void)sz; ((GXTestStats *)u)->dl++; }
static void t_unknown(void *u, uint8_t op) { (void)op; ((GXTestStats *)u)->unknown++; }
static const uint8_t *t_resolve(void *u, uint32_t addr, uint32_t sz) {
    (void)addr; (void)sz;
    return ((GXTestStats *)u)->dl_buf;
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

int gx_fifo_selftest(void) {
    // Vertex format: PosMatIdx + Position(Direct,Float,XYZ=12) + Color0(Direct,RGBA8888=4)
    // + Tex0(Direct,Float,ST=8)
    const uint32_t vcd_lo = 0x00002201;
    const uint32_t vcd_hi = 0x00000001;
    const uint32_t vat_g0 = 0x01216009;
    const uint32_t expect_vsize = 25;
    const uint16_t nverts = 3;

    // Independent size check
    GXFifoState st;
    gx_fifo_state_reset(&st);
    gx_fifo_apply_cp(&st, GX_CP_VCD_LO, vcd_lo);
    gx_fifo_apply_cp(&st, GX_CP_VCD_HI, vcd_hi);
    gx_fifo_apply_cp(&st, GX_CP_VAT_A, vat_g0);
    if (gx_fifo_vertex_size(&st, 0) != expect_vsize) return 0;

    // One BP load then NOP fill.
    uint8_t dl[32];
    memset(dl, GX_OPCODE_NOP, sizeof(dl));
    dl[0] = GX_OPCODE_LOAD_BP_REG;
    dl[1] = 0x30;
    dl[2] = 0xAA; dl[3] = 0xBB; dl[4] = 0xCC;
    const uint32_t dl_nops = (uint32_t)sizeof(dl) - 5;

    uint8_t fifo[160];
    size_t n = 0;
    fifo[n++] = GX_OPCODE_NOP;
    fifo[n++] = GX_OPCODE_NOP;

    fifo[n++] = GX_OPCODE_LOAD_CP_REG; fifo[n++] = GX_CP_VCD_LO; put_be32(&fifo[n], vcd_lo); n += 4;
    fifo[n++] = GX_OPCODE_LOAD_CP_REG; fifo[n++] = GX_CP_VCD_HI; put_be32(&fifo[n], vcd_hi); n += 4;
    fifo[n++] = GX_OPCODE_LOAD_CP_REG; fifo[n++] = GX_CP_VAT_A;  put_be32(&fifo[n], vat_g0); n += 4;

    // XF load
    fifo[n++] = GX_OPCODE_LOAD_XF_REG; put_be32(&fifo[n], 0x00011000); n += 4;
    for (int i = 0; i < 8; i++) fifo[n++] = (uint8_t)i;

    // BP load
    fifo[n++] = GX_OPCODE_LOAD_BP_REG; fifo[n++] = 0x28;
    fifo[n++] = 0x12; fifo[n++] = 0x34; fifo[n++] = 0x56;

    // Indexed XF load
    fifo[n++] = GX_OPCODE_LOAD_INDX_A; put_be32(&fifo[n], 0x00025008); n += 4;

    // Triangle strip primitive
    fifo[n++] = 0x98; fifo[n++] = 0x00; fifo[n++] = (uint8_t)nverts;
    for (uint32_t i = 0; i < nverts * expect_vsize; i++) fifo[n++] = 0xEE;

    // Call display list
    fifo[n++] = GX_OPCODE_CALL_DL; put_be32(&fifo[n], 0x00000100); n += 4;
    put_be32(&fifo[n], sizeof(dl)); n += 4;

    const size_t expect_bytes = 2 + 6 + 6 + 6 + 13 + 5 + 5 + (3 + nverts * expect_vsize) + 9;

    GXTestStats stats;
    memset(&stats, 0, sizeof(stats));
    stats.dl_buf = dl;
    stats.dl_len = sizeof(dl);

    const GXFifoSink sink = {
        .on_nop = t_nop, .on_cp = t_cp, .on_xf = t_xf, .on_bp = t_bp,
        .on_indexed = t_indexed, .on_primitive = t_prim, .on_display_list = t_dl,
        .on_unknown = t_unknown, .resolve_dl = t_resolve,
    };

    GXFifoState state;
    gx_fifo_state_reset(&state);
    const size_t consumed = gx_fifo_run(&state, fifo, n, &sink, &stats);

    if (consumed != expect_bytes) return 0;
    if (stats.nop_runs != 2 || stats.total_nops != 2 + dl_nops) return 0;
    if (stats.cp != 3 || stats.xf != 1) return 0;
    if (stats.bp != 2) return 0;
    if (stats.indexed != 1 || stats.indexed_array != 12) return 0;
    if (stats.prim != 1 || stats.prim_vsize != expect_vsize || stats.prim_nverts != nverts) return 0;
    if (stats.dl != 1 || stats.unknown != 0) return 0;

    // A primitive header with a missing vertex payload
    uint8_t trunc[4] = {0x98, 0x00, 0x03, 0x00};
    GXFifoState st2;
    gx_fifo_state_reset(&st2);
    gx_fifo_apply_cp(&st2, GX_CP_VCD_LO, vcd_lo);
    gx_fifo_apply_cp(&st2, GX_CP_VCD_HI, vcd_hi);
    gx_fifo_apply_cp(&st2, GX_CP_VAT_A, vat_g0);
    if (gx_fifo_run(&st2, trunc, sizeof(trunc), NULL, NULL) != 0) return 0;

    return 1;
}
