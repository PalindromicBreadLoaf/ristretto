// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu/gx_draw.h"

#include <string.h>

#include <gx2/draw.h>
#include <gx2/enum.h>
#include <gx2/registers.h>
#include <gx2/sampler.h>
#include <gx2/shaders.h>
#include <gx2/utils.h>
#include <gx2r/draw.h>

#define GX_DRAW_DRY_MAX 256

// state folding

static void on_cp(void *user, uint8_t sub, uint32_t value) {
    GXDrawPipeline *p = user;
    const uint8_t group = sub & GX_CP_COMMAND_MASK;
    const uint8_t idx = sub & 0x0F;
    switch (group) {
    case GX_CP_MATINDEX_A:
        // Low 6 bits are the position/normal geometry matrix row index.
        p->geom_mtx_index = value & 0x3F;
        break;
    case GX_CP_ARRAY_BASE:
        p->arr.base[idx] = value;
        break;
    case GX_CP_ARRAY_STRIDE:
        p->arr.stride[idx] = value;
        break;
    default:
        break;
    }
}

static void on_xf(void *user, uint16_t address, uint8_t count, const uint8_t *data) {
    GXDrawPipeline *p = user;
    gx_xf_apply_xf(&p->xf, address, count, data);
}

static void on_bp(void *user, uint8_t command, uint32_t value) {
    GXDrawPipeline *p = user;
    gx_state_apply_bp(&p->render, command, value);
    gx_tev_apply_bp(&p->tev, command, value);
}

static const uint8_t *resolve_dl(void *user, uint32_t addr, uint32_t size) {
    GXDrawPipeline *p = user;
    if (!p->cb.read_guest) return NULL;
    return p->cb.read_guest(p->cb.user, addr, size);
}

// shader signature

// Enumerate the distinct texcoord slots the TEV samples, ascending.
static uint32_t distinct_texcoords(const TevConfig *tev, uint8_t slots[8]) {
    bool used[8] = {false};
    for (uint32_t s = 0; s < tev->num_stages; ++s) {
        if (!tev->stage[s].tex_enable) continue;
        used[tev->stage[s].texcoord & 7u] = true;
    }
    uint32_t n = 0;
    for (uint32_t tc = 0; tc < 8; ++tc)
        if (used[tc]) slots[n++] = (uint8_t)tc;
    return n;
}

static void build_sig(const GXDrawPipeline *p, bool transform, GXDrawShaderSig *out) {
    memset(out, 0, sizeof(*out));
    out->valid = true;
    out->transform = transform;
    out->num_stages = p->tev.num_stages;
    memcpy(out->stage, p->tev.stage, sizeof(out->stage));
    memcpy(out->swap, p->tev.swap, sizeof(out->swap));
    out->num_texcoords = distinct_texcoords(&p->tev, out->tex_slot);
}

// GX2 helpers

static GX2PrimitiveMode prim_mode(GXPrimitive prim) {
    switch (prim) {
    case GX_PRIM_QUADS:
    case GX_PRIM_QUADS_2:        return GX2_PRIMITIVE_MODE_QUADS;
    case GX_PRIM_TRIANGLES:      return GX2_PRIMITIVE_MODE_TRIANGLES;
    case GX_PRIM_TRIANGLE_STRIP: return GX2_PRIMITIVE_MODE_TRIANGLE_STRIP;
    case GX_PRIM_TRIANGLE_FAN:   return GX2_PRIMITIVE_MODE_TRIANGLE_FAN;
    case GX_PRIM_LINES:          return GX2_PRIMITIVE_MODE_LINES;
    case GX_PRIM_LINE_STRIP:     return GX2_PRIMITIVE_MODE_LINE_STRIP;
    default:                     return GX2_PRIMITIVE_MODE_POINTS;
    }
}

static bool ensure_buffer(GXDrawPipeline *p, int idx, uint32_t elem_size,
                          uint32_t elem_count) {
    GX2RBuffer *b = &p->buffer[idx];
    if (p->buffer_cap[idx] >= elem_count && b->elemSize == elem_size) return true;
    if (p->buffer_cap[idx]) {
        GX2RDestroyBufferEx(b, 0);
        p->buffer_cap[idx] = 0;
    }
    memset(b, 0, sizeof(*b));
    b->flags = GX2R_RESOURCE_BIND_VERTEX_BUFFER | GX2R_RESOURCE_USAGE_CPU_READ |
               GX2R_RESOURCE_USAGE_CPU_WRITE | GX2R_RESOURCE_USAGE_GPU_READ;
    b->elemSize  = elem_size;
    b->elemCount = elem_count;
    if (!GX2RCreateBuffer(b)) {
        memset(b, 0, sizeof(*b));
        return false;
    }
    p->buffer_cap[idx] = elem_count;
    return true;
}

// Decode targets
static float g_dry_pos[GX_DRAW_DRY_MAX * 3];
static float g_dry_col[GX_DRAW_DRY_MAX * 4];
static float g_dry_tex[8][GX_DRAW_DRY_MAX * 2];

static void draw_primitive(GXDrawPipeline *p, GXPrimitive prim, uint8_t vat,
                           uint16_t num_vertices, const uint8_t *vertex_data,
                           uint32_t vertex_size) {
    if (num_vertices == 0) return;

    GXVertexLayout layout;
    gx_vertex_layout(&p->fifo, vat, &layout);

    const bool transform = layout.pos_3d;

    GXDrawShaderSig sig;
    build_sig(p, transform, &sig);

    const uint32_t ntc = sig.num_texcoords;
    const uint8_t *slots = sig.tex_slot;

    if (p->dry_run) {
        if (num_vertices > GX_DRAW_DRY_MAX) return;
        float *tex[8] = {0};
        for (uint32_t k = 0; k < ntc; ++k) {
            uint8_t s = slots[k];
            tex[s] = layout.tex_present[s] ? g_dry_tex[s] : NULL;
        }
        size_t used = gx_vertex_load(&p->fifo, vat, &p->arr, p->cb.read_guest,
                                     p->cb.user, vertex_data,
                                     (size_t)num_vertices * vertex_size, num_vertices,
                                     g_dry_pos, layout.has_color0 ? g_dry_col : NULL,
                                     NULL, tex, NULL);
        if (used == 0) return;
        p->prims++;
        p->verts += num_vertices;
        return;
    }

    if (!ensure_buffer(p, GX_DRAW_BUF_POS, 3 * sizeof(float), num_vertices)) return;
    if (!ensure_buffer(p, GX_DRAW_BUF_COL0, 4 * sizeof(float), num_vertices)) return;
    for (uint32_t k = 0; k < ntc; ++k)
        if (!ensure_buffer(p, GX_DRAW_BUF_TEX0 + slots[k], 2 * sizeof(float),
                           num_vertices))
            return;

    float *pos = GX2RLockBufferEx(&p->buffer[GX_DRAW_BUF_POS], 0);
    float *col = GX2RLockBufferEx(&p->buffer[GX_DRAW_BUF_COL0], 0);
    float *tex[8] = {0};
    float *tex_locked[8] = {0};
    for (uint32_t k = 0; k < ntc; ++k) {
        uint8_t s = slots[k];
        tex_locked[s] = GX2RLockBufferEx(&p->buffer[GX_DRAW_BUF_TEX0 + s], 0);
        tex[s] = layout.tex_present[s] ? tex_locked[s] : NULL;
    }

    size_t used = gx_vertex_load(&p->fifo, vat, &p->arr, p->cb.read_guest, p->cb.user,
                                 vertex_data, (size_t)num_vertices * vertex_size,
                                 num_vertices, pos, layout.has_color0 ? col : NULL,
                                 NULL, tex, NULL);

    if (used != 0) {
        if (!layout.has_color0)
            for (uint32_t v = 0; v < num_vertices; ++v) {
                col[v * 4 + 0] = col[v * 4 + 1] = col[v * 4 + 2] = col[v * 4 + 3] = 1.0f;
            }
        for (uint32_t k = 0; k < ntc; ++k) {
            uint8_t s = slots[k];
            if (!layout.tex_present[s])
                memset(tex_locked[s], 0, (size_t)num_vertices * 2 * sizeof(float));
        }
    }

    GX2RUnlockBufferEx(&p->buffer[GX_DRAW_BUF_POS], 0);
    GX2RUnlockBufferEx(&p->buffer[GX_DRAW_BUF_COL0], 0);
    for (uint32_t k = 0; k < ntc; ++k)
        GX2RUnlockBufferEx(&p->buffer[GX_DRAW_BUF_TEX0 + slots[k]], 0);

    if (used == 0) return;

    if (!p->sig.valid || memcmp(&p->sig, &sig, sizeof(sig)) != 0) {
        // location 0 position, 1 colour, 2+k the k-th texcoord.
        GX2AttribStream attribs[2 + 8];
        uint32_t na = 0;
        attribs[na++] = (GX2AttribStream){
            .location = 0, .buffer = GX_DRAW_BUF_POS, .offset = 0,
            .format = GX2_ATTRIB_FORMAT_FLOAT_32_32_32,
            .type = GX2_ATTRIB_INDEX_PER_VERTEX, .aluDivisor = 0,
            .mask = GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_Z, GX2_SQ_SEL_1),
            .endianSwap = GX2_ENDIAN_SWAP_DEFAULT};
        attribs[na++] = (GX2AttribStream){
            .location = 1, .buffer = GX_DRAW_BUF_COL0, .offset = 0,
            .format = GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32,
            .type = GX2_ATTRIB_INDEX_PER_VERTEX, .aluDivisor = 0,
            .mask = GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_Z, GX2_SQ_SEL_W),
            .endianSwap = GX2_ENDIAN_SWAP_DEFAULT};
        for (uint32_t k = 0; k < ntc; ++k) {
            attribs[na++] = (GX2AttribStream){
                .location = 2 + k, .buffer = 2 + k, .offset = 0,
                .format = GX2_ATTRIB_FORMAT_FLOAT_32_32,
                .type = GX2_ATTRIB_INDEX_PER_VERTEX, .aluDivisor = 0,
                .mask = GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_0, GX2_SQ_SEL_1),
                .endianSwap = GX2_ENDIAN_SWAP_DEFAULT};
        }

        if (p->sig.valid) gx2_bind_free(&p->bound);
        if (!gx2_bind_build_tev_ex(&p->bound, &p->tev, transform, attribs, na)) {
            p->sig.valid = false;
            return;
        }
        p->sig = sig;
    }

    // Record the draw for replay inside the render pass.
    if (p->nrecords >= GX_DRAW_MAX_RECORDS) return;
    GXDrawRecord *rec = &p->record[p->nrecords++];
    rec->mode  = prim_mode(prim);
    rec->count = num_vertices;
    rec->ntc   = ntc;
    memcpy(rec->slots, slots, sizeof(rec->slots));
    if (transform)
        gx_xf_build_vs_cfile(&p->xf, p->geom_mtx_index, rec->vs_cfile);
    else
        gx_xf_build_vs_cfile(&p->xf, 0, rec->vs_cfile);  // 2D passthrough
    gx_state_depth(&p->render, &rec->depth);
    gx_state_blend(&p->render, &rec->blend);
    gx_state_cull(&p->render, &rec->cull);

    p->prims++;
    p->verts += num_vertices;
}

// Bind depth/blend/cull from a record's snapshot.
static void apply_state(const GXDrawRecord *rec) {
    GX2SetDepthOnlyControl(rec->depth.test_enable, rec->depth.write_enable,
                           rec->depth.func);
    GX2SetColorControl(rec->blend.logic_op_enable ? rec->blend.logic_op
                                                  : GX2_LOGIC_OP_COPY,
                       rec->blend.blend_enable ? 1u : 0u, TRUE,
                       (rec->blend.color_update || rec->blend.alpha_update) ? TRUE
                                                                           : FALSE);
    GX2SetBlendControl(GX2_RENDER_TARGET_0, rec->blend.src_color, rec->blend.dst_color,
                       rec->blend.color_combine, TRUE, rec->blend.src_alpha,
                       rec->blend.dst_alpha, rec->blend.alpha_combine);
    GX2SetCullOnlyControl(rec->cull.front_face, rec->cull.cull_front,
                          rec->cull.cull_back);
}

void gx_draw_replay(GXDrawPipeline *p) {
    if (!p || !p->sig.valid) return;

    GX2SetFetchShader(&p->bound.fs);
    GX2SetVertexShader(&p->bound.vs);
    GX2SetPixelShader(&p->bound.ps);
    gx2_bind_set_tev_uniforms(&p->tev);

    for (uint32_t i = 0; i < p->bound.ps_sampler_count; ++i) {
        uint32_t loc = p->bound.ps_samplers[i].location;
        GX2Texture *t = p->cb.get_texture ? p->cb.get_texture(p->cb.user, (uint8_t)loc)
                                          : NULL;
        GX2Sampler *s = p->cb.get_sampler ? p->cb.get_sampler(p->cb.user, (uint8_t)loc)
                                          : NULL;
        if (t) GX2SetPixelTexture(t, loc);
        if (s) GX2SetPixelSampler(s, loc);
    }

    for (uint32_t r = 0; r < p->nrecords; ++r) {
        const GXDrawRecord *rec = &p->record[r];
        GX2SetVertexUniformReg(0, 16, (void *)rec->vs_cfile);
        apply_state(rec);
        GX2RSetAttributeBuffer(&p->buffer[GX_DRAW_BUF_POS], 0,
                               p->buffer[GX_DRAW_BUF_POS].elemSize, 0);
        GX2RSetAttributeBuffer(&p->buffer[GX_DRAW_BUF_COL0], 1,
                               p->buffer[GX_DRAW_BUF_COL0].elemSize, 0);
        for (uint32_t k = 0; k < rec->ntc; ++k) {
            int bi = GX_DRAW_BUF_TEX0 + rec->slots[k];
            GX2RSetAttributeBuffer(&p->buffer[bi], 2 + k, p->buffer[bi].elemSize, 0);
        }
        GX2DrawEx(rec->mode, rec->count, 0, 1);
    }
}

static void on_primitive(void *user, GXPrimitive prim, uint8_t vat,
                         uint32_t vertex_size, uint16_t num_vertices,
                         const uint8_t *vertex_data) {
    draw_primitive(user, prim, vat, num_vertices, vertex_data, vertex_size);
}

// API

bool gx_draw_init(GXDrawPipeline *p, const GXDrawCallbacks *cb) {
    if (!p || !cb) return false;
    memset(p, 0, sizeof(*p));
    p->cb = *cb;
    gx_draw_reset_state(p);
    p->ok = true;
    return true;
}

void gx_draw_shutdown(GXDrawPipeline *p) {
    if (!p) return;
    if (p->sig.valid) gx2_bind_free(&p->bound);
    for (int i = 0; i < GX_DRAW_BUF_COUNT; ++i)
        if (p->buffer_cap[i]) {
            GX2RDestroyBufferEx(&p->buffer[i], 0);
            p->buffer_cap[i] = 0;
        }
    p->ok = false;
}

void gx_draw_reset_state(GXDrawPipeline *p) {
    if (!p) return;
    gx_fifo_state_reset(&p->fifo);
    gx_xf_reset(&p->xf);
    gx_tev_reset(&p->tev);
    gx_state_reset(&p->render);
    memset(&p->arr, 0, sizeof(p->arr));
    p->geom_mtx_index = 0;
    p->sig.valid = false;
}

uint32_t gx_draw_execute(GXDrawPipeline *p, const uint8_t *fifo, size_t len) {
    if (!p || !fifo) return 0;
    p->prims = 0;
    p->verts = 0;
    p->nrecords = 0;
    const GXFifoSink sink = {
        .on_cp = on_cp,
        .on_xf = on_xf,
        .on_bp = on_bp,
        .on_primitive = on_primitive,
        .resolve_dl = resolve_dl,
    };
    gx_fifo_run(&p->fifo, fifo, len, &sink, p);
    return p->prims;
}

// self test

static void put_be32(uint8_t *b, size_t *n, uint32_t v) {
    b[(*n)++] = v >> 24; b[(*n)++] = v >> 16; b[(*n)++] = v >> 8; b[(*n)++] = (uint8_t)v;
}
static void put_f(uint8_t *b, size_t *n, float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    put_be32(b, n, u);
}

int gx_draw_selftest(void) {
    static GXDrawPipeline p;
    GXDrawCallbacks cb = {0};
    if (!gx_draw_init(&p, &cb)) return 0;
    p.dry_run = true;

    const uint32_t vcd_lo = (1u << 9) | (1u << 13);  // pos Direct, col0 Direct
    const uint32_t vcd_hi = 0x00000001;              // tex0 Direct
    const uint32_t vat_g0 = 0x01216009;              // pos xyz f32; col0 8888; tex0 f32 st
    const uint16_t nverts = 4;

    uint8_t fifo[512];
    size_t n = 0;

    // CP
    fifo[n++] = GX_OPCODE_LOAD_CP_REG; fifo[n++] = GX_CP_VCD_LO; put_be32(fifo, &n, vcd_lo);
    fifo[n++] = GX_OPCODE_LOAD_CP_REG; fifo[n++] = GX_CP_VCD_HI; put_be32(fifo, &n, vcd_hi);
    fifo[n++] = GX_OPCODE_LOAD_CP_REG; fifo[n++] = GX_CP_VAT_A;  put_be32(fifo, &n, vat_g0);

    // XF
    fifo[n++] = GX_OPCODE_LOAD_XF_REG;
    put_be32(fifo, &n, ((7u - 1u) << 16) | XF_SETPROJECTION);
    float proj[6] = {1.0f, 0.0f, 1.0f, 0.0f, -1.0f, 0.0f};
    for (int i = 0; i < 6; ++i) put_f(fifo, &n, proj[i]);
    put_be32(fifo, &n, GX_XF_ORTHOGRAPHIC);

    // BP
    fifo[n++] = GX_OPCODE_LOAD_BP_REG; fifo[n++] = GX_BP_GENMODE;
    fifo[n++] = 0x00; fifo[n++] = 0x00; fifo[n++] = 0x00;

    static const float quad_pos[4][3] = {
        {-0.8f, -0.8f, 0.0f}, {0.8f, -0.8f, 0.0f},
        {-0.8f, 0.8f, 0.0f},  {0.8f, 0.8f, 0.0f}};
    static const float quad_uv[4][2] = {
        {0.0f, 1.0f}, {1.0f, 1.0f}, {0.0f, 0.0f}, {1.0f, 0.0f}};
    fifo[n++] = 0x98;                       // GX_PRIM_TRIANGLE_STRIP, VAT 0
    fifo[n++] = 0x00; fifo[n++] = (uint8_t)nverts;
    for (int v = 0; v < nverts; ++v) {
        put_f(fifo, &n, quad_pos[v][0]);
        put_f(fifo, &n, quad_pos[v][1]);
        put_f(fifo, &n, quad_pos[v][2]);
        fifo[n++] = 0xFF; fifo[n++] = 0x80; fifo[n++] = 0x40; fifo[n++] = 0xFF;  // RGBA
        put_f(fifo, &n, quad_uv[v][0]);
        put_f(fifo, &n, quad_uv[v][1]);
    }

    uint32_t prims = gx_draw_execute(&p, fifo, n);
    int ok = 1;
    if (prims != 1 || p.verts != nverts) ok = 0;
    if (p.tev.num_stages != 1) ok = 0;
    if (p.xf.projection_type != GX_XF_ORTHOGRAPHIC) ok = 0;

    // The folded position of vertex 3 must survive decode.
    if (g_dry_pos[3 * 3 + 0] != 0.8f || g_dry_pos[3 * 3 + 1] != 0.8f) ok = 0;

    gx_draw_shutdown(&p);
    return ok;
}
