// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu/gx_vertex.h"

#include <string.h>

// VertexComponentFormat
enum { VCF_NONE = 0, VCF_DIRECT = 1, VCF_INDEX8 = 2, VCF_INDEX16 = 3 };

// ComponentFormat (VAT): 0 u8, 1 s8, 2 u16, 3 s16, 4 f32.
static uint32_t comp_bytes(uint32_t fmt) {
    if (fmt <= 1) return 1;
    if (fmt <= 3) return 2;
    return 4;
}

// ColorFormat direct byte sizes: 565=2 888=3 888x=4 4444=2 6666=3 8888=4.
static uint32_t color_bytes(uint32_t comp) {
    static const uint32_t sz[6] = {2, 3, 4, 2, 3, 4};
    return comp < 6 ? sz[comp] : 0;
}

// Direct GX attributes are packed, so multi-byte values need not be naturally aligned.
static uint32_t rd_be16(const uint8_t *p) {
    const volatile uint8_t *q = (const volatile uint8_t *)p;
    return (uint32_t)((q[0] << 8) | q[1]);
}
static uint32_t rd_be24(const uint8_t *p) {
    const volatile uint8_t *q = (const volatile uint8_t *)p;
    return ((uint32_t)q[0] << 16) | ((uint32_t)q[1] << 8) | q[2];
}
static float read_f32_be(const uint8_t *p) {
    const volatile uint8_t *q = (const volatile uint8_t *)p;
    uint32_t u = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) |
                 ((uint32_t)q[2] << 8) | q[3];
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

// Dequantise one scalar component.
static float read_scalar(const uint8_t *p, uint32_t fmt, uint32_t frac, bool byte_dequant) {
    static const float dequant_scale[32] = {
        1.0f, 0.5f, 0.25f, 0.125f, 0.0625f, 0.03125f, 0.015625f, 0.0078125f,
        0.00390625f, 0.001953125f, 0.0009765625f, 0.00048828125f, 0.000244140625f,
        0.0001220703125f, 0.00006103515625f, 0.000030517578125f, 0.0000152587890625f,
        0.00000762939453125f, 0.000003814697265625f, 0.0000019073486328125f,
        0.00000095367431640625f, 0.000000476837158203125f, 0.0000002384185791015625f,
        0.00000011920928955078125f, 0.000000059604644775390625f,
        0.0000000298023223876953125f, 0.00000001490116119384765625f,
        0.000000007450580596923828125f, 0.0000000037252902984619140625f,
        0.00000000186264514923095703125f, 0.000000000931322574615478515625f,
        0.0000000004656612873077392578125f,
    };
    if (fmt >= 4) return read_f32_be(p);

    const float scale = dequant_scale[frac & 31];
    switch (fmt) {
    case 0: return byte_dequant ? (uint8_t)p[0] * scale : (float)(uint8_t)p[0];
    case 1: return byte_dequant ? (int8_t)p[0] * scale : (float)(int8_t)p[0];
    case 2: return (uint16_t)rd_be16(p) * scale;
    case 3: return (int16_t)rd_be16(p) * scale;
    default: return 0.0f;
    }
}

// Expand one GX colour value to RGBA floats.
static void read_color(const uint8_t *p, uint32_t comp, float out[4]) {
    switch (comp) {
    case 0: {  // RGB565
        uint32_t v = rd_be16(p);
        out[0] = ((v >> 11) & 0x1F) * (1.0f / 31.0f);
        out[1] = ((v >> 5) & 0x3F) * (1.0f / 63.0f);
        out[2] = (v & 0x1F) * (1.0f / 31.0f);
        out[3] = 1.0f;
        break;
    }
    case 1:  // RGB888
    case 2:  // RGB888x
        out[0] = p[0] * (1.0f / 255.0f);
        out[1] = p[1] * (1.0f / 255.0f);
        out[2] = p[2] * (1.0f / 255.0f);
        out[3] = 1.0f;
        break;
    case 3: {  // RGBA4444
        uint32_t v = rd_be16(p);
        out[0] = ((v >> 12) & 0xF) * (1.0f / 15.0f);
        out[1] = ((v >> 8) & 0xF) * (1.0f / 15.0f);
        out[2] = ((v >> 4) & 0xF) * (1.0f / 15.0f);
        out[3] = (v & 0xF) * (1.0f / 15.0f);
        break;
    }
    case 4: {  // RGBA6666
        uint32_t v = rd_be24(p);
        out[0] = ((v >> 18) & 0x3F) * (1.0f / 63.0f);
        out[1] = ((v >> 12) & 0x3F) * (1.0f / 63.0f);
        out[2] = ((v >> 6) & 0x3F) * (1.0f / 63.0f);
        out[3] = (v & 0x3F) * (1.0f / 63.0f);
        break;
    }
    default:  // RGBA8888
        out[0] = p[0] * (1.0f / 255.0f);
        out[1] = p[1] * (1.0f / 255.0f);
        out[2] = p[2] * (1.0f / 255.0f);
        out[3] = p[3] * (1.0f / 255.0f);
        break;
    }
}

static void tex_vat(uint32_t g0, uint32_t g1, uint32_t g2, unsigned i, uint32_t *elem,
                    uint32_t *fmt, uint32_t *frac) {
    switch (i) {
    case 0: *elem = (g0 >> 21) & 1; *fmt = (g0 >> 22) & 7; *frac = (g0 >> 25) & 0x1F; break;
    case 1: *elem = (g1 >> 0) & 1;  *fmt = (g1 >> 1) & 7;  *frac = (g1 >> 4) & 0x1F;  break;
    case 2: *elem = (g1 >> 9) & 1;  *fmt = (g1 >> 10) & 7; *frac = (g1 >> 13) & 0x1F; break;
    case 3: *elem = (g1 >> 18) & 1; *fmt = (g1 >> 19) & 7; *frac = (g1 >> 22) & 0x1F; break;
    case 4: *elem = (g1 >> 27) & 1; *fmt = (g1 >> 28) & 7; *frac = (g2 >> 0) & 0x1F;  break;
    case 5: *elem = (g2 >> 5) & 1;  *fmt = (g2 >> 6) & 7;  *frac = (g2 >> 9) & 0x1F;  break;
    case 6: *elem = (g2 >> 14) & 1; *fmt = (g2 >> 15) & 7; *frac = (g2 >> 18) & 0x1F; break;
    default:*elem = (g2 >> 23) & 1; *fmt = (g2 >> 24) & 7; *frac = (g2 >> 27) & 0x1F; break;
    }
}

// Bytes one normal component group occupies in the vertex payload.
static uint32_t normal_payload_bytes(uint32_t vcf, uint32_t fmt, uint32_t nbt, uint32_t index3) {
    switch (vcf) {
    case VCF_DIRECT:  return comp_bytes(fmt) * (nbt ? 9u : 3u);
    case VCF_INDEX8:  return nbt ? (index3 ? 3u : 1u) : 1u;
    case VCF_INDEX16: return nbt ? (index3 ? 6u : 2u) : 2u;
    default: return 0;
    }
}

void gx_vertex_layout(const GXFifoState *st, uint8_t vat, GXVertexLayout *out) {
    memset(out, 0, sizeof(*out));
    const uint32_t lo = st->desc_lo;
    const uint32_t hi = st->desc_hi;
    const uint32_t g0 = st->vat[vat & 7].g0;

    out->has_pos_mtx_idx = lo & 1u;
    out->pos_3d          = g0 & 1u;
    out->has_color0      = ((lo >> 13) & 3) != VCF_NONE;
    out->has_color1      = ((lo >> 15) & 3) != VCF_NONE;
    for (unsigned i = 0; i < 8; ++i) {
        if (((hi >> (2 * i)) & 3) != VCF_NONE) {
            out->tex_present[i] = true;
            out->num_texcoords++;
        }
    }
}

// One attribute read cursor over the primitive payload.
typedef struct {
    const uint8_t *base;
    size_t off;
    size_t avail;
    bool   err;
} Cursor;

// Return a pointer to `n` payload bytes at the cursor.
static const uint8_t *cur_take(Cursor *c, size_t n) {
    if (c->err || c->off + n > c->avail) {
        c->err = true;
        return NULL;
    }
    const uint8_t *p = c->base + c->off;
    c->off += n;
    return p;
}

// Resolve a component's source bytes.
static const uint8_t *resolve_component(Cursor *c, uint32_t vcf, uint32_t data_bytes,
                                        uint32_t array, const GXArrayTable *arr,
                                        GXGuestRead read, void *user) {
    if (vcf == VCF_DIRECT) {
        return cur_take(c, data_bytes);
    }
    uint32_t index;
    if (vcf == VCF_INDEX8) {
        const uint8_t *ip = cur_take(c, 1);
        if (!ip) return NULL;
        index = ip[0];
    } else {  // VCF_INDEX16
        const uint8_t *ip = cur_take(c, 2);
        if (!ip) return NULL;
        index = rd_be16(ip);
    }
    if (!read) { c->err = true; return NULL; }
    uint32_t ea = arr->base[array] + index * arr->stride[array];
    const uint8_t *p = read(user, ea, data_bytes);
    if (!p) c->err = true;
    return p;
}

size_t gx_vertex_load(const GXFifoState *st, uint8_t vat, const GXArrayTable *arr,
                      GXGuestRead read, void *user, const uint8_t *src, size_t avail,
                      uint32_t num_vertices, float *pos_out, float *col0_out,
                      float *col1_out, float *tex_out[8], uint8_t *pmi_out) {
    if (!st || !arr || !src) return 0;

    const uint32_t lo = st->desc_lo;
    const uint32_t hi = st->desc_hi;
    const uint32_t g0 = st->vat[vat & 7].g0;
    const uint32_t g1 = st->vat[vat & 7].g1;
    const uint32_t g2 = st->vat[vat & 7].g2;

    const uint32_t pos_vcf   = (lo >> 9) & 3;
    const uint32_t nrm_vcf   = (lo >> 11) & 3;
    const uint32_t col_vcf[2] = {(lo >> 13) & 3, (lo >> 15) & 3};

    const uint32_t pos_elems = (g0 & 1) ? 3u : 2u;
    const uint32_t pos_fmt   = (g0 >> 1) & 7;
    const uint32_t pos_frac  = (g0 >> 4) & 0x1F;
    const uint32_t nrm_fmt   = (g0 >> 10) & 7;
    const uint32_t nrm_nbt   = (g0 >> 9) & 1;
    const uint32_t nrm_idx3  = (g0 >> 31) & 1;
    const uint32_t col_comp[2] = {(g0 >> 14) & 7, (g0 >> 18) & 7};
    const bool     byte_dequant = (g0 >> 30) & 1;

    if (pos_vcf == VCF_NONE) return 0;

    Cursor c = {.base = src, .off = 0, .avail = avail, .err = false};

    for (uint32_t v = 0; v < num_vertices; ++v) {
        if (lo & 1u) {
            const uint8_t *p = cur_take(&c, 1);
            if (!p) return 0;
            if (pmi_out) pmi_out[v] = p[0];
        } else if (pmi_out) {
            pmi_out[v] = 0;
        }
        for (unsigned t = 0; t < 8; ++t) {
            if ((lo >> (1 + t)) & 1u) {
                if (!cur_take(&c, 1)) return 0;
            }
        }

        {
            const uint32_t cb = comp_bytes(pos_fmt);
            const uint8_t *p = resolve_component(&c, pos_vcf, cb * pos_elems,
                                                 GX_ARRAY_POSITION, arr, read, user);
            if (!p) return 0;
            if (pos_out) {
                float *o = pos_out + (size_t)v * 3;
                o[0] = read_scalar(p, pos_fmt, pos_frac, byte_dequant);
                o[1] = read_scalar(p + cb, pos_fmt, pos_frac, byte_dequant);
                o[2] = pos_elems == 3 ? read_scalar(p + 2 * cb, pos_fmt, pos_frac, byte_dequant)
                                      : 0.0f;
            }
        }

        if (nrm_vcf != VCF_NONE) {
            if (!cur_take(&c, normal_payload_bytes(nrm_vcf, nrm_fmt, nrm_nbt, nrm_idx3)))
                return 0;
        }

        // Colours.
        for (unsigned k = 0; k < 2; ++k) {
            if (col_vcf[k] == VCF_NONE) continue;
            const uint32_t cb = color_bytes(col_comp[k]);
            if (cb == 0) return 0;
            const uint8_t *p = resolve_component(&c, col_vcf[k], cb, GX_ARRAY_COLOR0 + k,
                                                 arr, read, user);
            if (!p) return 0;
            float *o = k == 0 ? col0_out : col1_out;
            if (o) read_color(p, col_comp[k], o + (size_t)v * 4);
        }

        // Texcoords.
        for (unsigned t = 0; t < 8; ++t) {
            const uint32_t vcf = (hi >> (2 * t)) & 3;
            if (vcf == VCF_NONE) continue;
            uint32_t elem, fmt, frac;
            tex_vat(g0, g1, g2, t, &elem, &fmt, &frac);
            const uint32_t st_elems = elem ? 2u : 1u;
            const uint32_t cb = comp_bytes(fmt);
            const uint8_t *p = resolve_component(&c, vcf, cb * st_elems,
                                                 GX_ARRAY_TEXCOORD0 + t, arr, read, user);
            if (!p) return 0;
            if (tex_out && tex_out[t]) {
                float *o = tex_out[t] + (size_t)v * 2;
                o[0] = read_scalar(p, fmt, frac, byte_dequant);
                o[1] = st_elems == 2 ? read_scalar(p + cb, fmt, frac, byte_dequant) : 0.0f;
            }
        }

        if (c.err) return 0;
    }

    return c.off;
}

// Self test
static const uint8_t *test_read(void *user, uint32_t ea, uint32_t len) {
    const uint8_t *arrays = user;
    if (ea < 0x80010000u) return NULL;
    uint32_t off = ea - 0x80010000u;
    if (off + len > 0x1000u) return NULL;
    return arrays + off;
}

static void put_be16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = (uint8_t)v; }
static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v;
}
static void put_f(uint8_t *p, float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    put_be32(p, u);
}

int gx_vertex_selftest(void) {
    // Format A
    {
        GXFifoState st;
        gx_fifo_state_reset(&st);
        gx_fifo_apply_cp(&st, GX_CP_VCD_LO, 0x00002201);
        gx_fifo_apply_cp(&st, GX_CP_VCD_HI, 0x00000001);
        gx_fifo_apply_cp(&st, GX_CP_VAT_A, 0x01216009);

        GXVertexLayout lay;
        gx_vertex_layout(&st, 0, &lay);
        if (!lay.pos_3d || !lay.has_color0 || lay.has_color1 || !lay.has_pos_mtx_idx ||
            lay.num_texcoords != 1 || !lay.tex_present[0]) {
            return 0;
        }

        uint8_t vtx[25 * 2];
        // Vertex 0
        size_t n = 0;
        vtx[n++] = 7;                              // PosMatIdx
        put_f(&vtx[n], 1.5f); n += 4;
        put_f(&vtx[n], -2.0f); n += 4;
        put_f(&vtx[n], 3.25f); n += 4;
        vtx[n++] = 255; vtx[n++] = 128; vtx[n++] = 64; vtx[n++] = 255;  // RGBA
        put_f(&vtx[n], 0.25f); n += 4;
        put_f(&vtx[n], 0.75f); n += 4;
        // Vertex 1
        vtx[n++] = 0;
        put_f(&vtx[n], 10.0f); n += 4;
        put_f(&vtx[n], 20.0f); n += 4;
        put_f(&vtx[n], 30.0f); n += 4;
        vtx[n++] = 0; vtx[n++] = 255; vtx[n++] = 0; vtx[n++] = 255;
        put_f(&vtx[n], 1.0f); n += 4;
        put_f(&vtx[n], 0.0f); n += 4;

        float pos[6], col[8], tex0[4];
        float *tex[8] = {tex0, 0, 0, 0, 0, 0, 0, 0};
        uint8_t pmi[2];
        GXArrayTable arr = {0};

        size_t consumed = gx_vertex_load(&st, 0, &arr, NULL, NULL, vtx, n, 2, NULL, NULL,
                                         NULL, NULL, NULL);
        if (consumed != 50) {
            return 0;
        }

        consumed = gx_vertex_load(&st, 0, &arr, NULL, NULL, vtx, n, 2, NULL, NULL,
                                  NULL, NULL, pmi);
        if (consumed != 50 || pmi[0] != 7 || pmi[1] != 0) {
            return 0;
        }

        consumed = gx_vertex_load(&st, 0, &arr, NULL, NULL, vtx, n, 2, pos, NULL,
                                  NULL, NULL, NULL);
        if (consumed != 50 || pos[0] != 1.5f || pos[1] != -2.0f || pos[2] != 3.25f ||
            pos[3] != 10.0f || pos[5] != 30.0f) {
            return 0;
        }

        consumed = gx_vertex_load(&st, 0, &arr, NULL, NULL, vtx, n, 2, NULL, col,
                                  NULL, NULL, NULL);
        float c1 = 128.0f / 255.0f, c2 = 64.0f / 255.0f;
        if (consumed != 50 || col[0] != 1.0f || col[3] != 1.0f ||
            col[1] < c1 - 1e-6f || col[1] > c1 + 1e-6f ||
            col[2] < c2 - 1e-6f || col[2] > c2 + 1e-6f) {
            return 0;
        }

        consumed = gx_vertex_load(&st, 0, &arr, NULL, NULL, vtx, n, 2, NULL, NULL,
                                  NULL, tex, NULL);
        if (consumed != 50 || tex0[0] != 0.25f || tex0[1] != 0.75f ||
            tex0[2] != 1.0f || tex0[3] != 0.0f) {
            return 0;
        }

        consumed = gx_vertex_load(&st, 0, &arr, NULL, NULL, vtx, n, 2, pos, col,
                                  NULL, tex, pmi);
        if (consumed != 50 || pmi[0] != 7 || pmi[1] != 0 ||
            pos[0] != 1.5f || pos[1] != -2.0f || pos[2] != 3.25f ||
            pos[3] != 10.0f || pos[5] != 30.0f || col[0] != 1.0f || col[3] != 1.0f) {
            return 0;
        }
        if (col[1] < c1 - 1e-6f || col[1] > c1 + 1e-6f ||
            col[2] < c2 - 1e-6f || col[2] > c2 + 1e-6f ||
            tex0[0] != 0.25f || tex0[1] != 0.75f || tex0[2] != 1.0f || tex0[3] != 0.0f) {
            return 0;
        }
    }

    // Format B
    {
        GXFifoState st;
        gx_fifo_state_reset(&st);
        // Position = Index16 (3<<9), Color0 = Direct (1<<13).
        gx_fifo_apply_cp(&st, GX_CP_VCD_LO, (3u << 9) | (1u << 13));
        gx_fifo_apply_cp(&st, GX_CP_VCD_HI, 0);
        // VAT
        uint32_t g0 = 1u | (3u << 1) | (2u << 4);
        gx_fifo_apply_cp(&st, GX_CP_VAT_A, g0);

        static uint8_t pool[0x1000];
        memset(pool, 0, sizeof(pool));
        put_be16(&pool[5 * 6 + 0], (uint16_t)(int16_t)(4 << 2));   // 4.0
        put_be16(&pool[5 * 6 + 2], (uint16_t)(int16_t)(-3 * 4));   // -3.0
        put_be16(&pool[5 * 6 + 4], (uint16_t)(int16_t)(1 << 2));   // 1.0

        GXArrayTable arr = {0};
        arr.base[GX_ARRAY_POSITION]   = 0x80010000u;
        arr.stride[GX_ARRAY_POSITION] = 6;

        uint8_t vtx[8];
        size_t n = 0;
        put_be16(&vtx[n], 5); n += 2;              // position index
        put_be16(&vtx[n], 0xF800); n += 2;         // RGB565 pure red
        float pos[3], col[4];
        float *tex[8] = {0};
        size_t consumed = gx_vertex_load(&st, 0, &arr, test_read, pool, vtx, n, 1, pos,
                                         col, NULL, tex, NULL);
        if (consumed != 4 || pos[0] != 4.0f || pos[1] != -3.0f || pos[2] != 1.0f ||
            col[0] != 1.0f || col[1] != 0.0f || col[2] != 0.0f || col[3] != 1.0f) {
            return 0;
        }
    }

    {
        GXFifoState st;
        gx_fifo_state_reset(&st);
        gx_fifo_apply_cp(&st, GX_CP_VCD_LO, (1u << 9));  // Position Direct
        gx_fifo_apply_cp(&st, GX_CP_VCD_HI, 0);
        gx_fifo_apply_cp(&st, GX_CP_VAT_A, 1u | (4u << 1));  // 3 elems, f32
        uint8_t vtx[6] = {0};                                // needs 12
        float pos[3];
        float *tex[8] = {0};
        GXArrayTable arr = {0};
        size_t consumed = gx_vertex_load(&st, 0, &arr, NULL, NULL, vtx, sizeof(vtx), 1,
                                         pos, NULL, NULL, tex, NULL);
        if (consumed != 0) {
            return 0;
        }
    }

    return 1;
}
