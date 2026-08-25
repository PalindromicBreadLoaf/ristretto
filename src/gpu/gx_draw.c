// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu/gx_draw.h"

#include <stdlib.h>
#include <string.h>

#include <gx2/draw.h>
#include <gx2/enum.h>
#include <gx2/event.h>
#include <gx2/registers.h>
#include <gx2/sampler.h>
#include <gx2/shaders.h>
#include <gx2/utils.h>
#include <gx2r/draw.h>

#define GX_DRAW_DRY_MAX 256

// state folding

static void record_clear(GXDrawPipeline *p, const GXClearState *clear);
static void record_copy(GXDrawPipeline *p, const GXCopyState *copy);

static void refresh_tev_pixel_state(GXDrawPipeline *p) {
    GXAlphaTestState alpha;
    gx_state_alpha_test(&p->render, &alpha);
    p->tev.pixel.alpha_test_enable = alpha.enable;
    p->tev.pixel.alpha_comp0 = (uint8_t)alpha.comp0;
    p->tev.pixel.alpha_comp1 = (uint8_t)alpha.comp1;
    p->tev.pixel.alpha_op = alpha.op;
    p->tev.pixel.alpha_ref0 = alpha.ref0;
    p->tev.pixel.alpha_ref1 = alpha.ref1;
    p->tev.pixel.efb_format = (uint8_t)gx_state_efb_format(&p->render);
    p->tev.pixel.rgba6 = p->tev.pixel.efb_format == GX_EFB_RGBA6_Z24;
    p->tev.pixel.dst_alpha_enable = ((p->render.constant_alpha >> 8) & 1u) &&
                                    ((p->render.blendmode >> 4) & 1u) &&
                                    p->tev.pixel.rgba6;
    p->tev.pixel.dst_alpha = (uint8_t)p->render.constant_alpha;
}

static void on_cp(void *user, uint8_t sub, uint32_t value) {
    GXDrawPipeline *p = user;
    const uint8_t group = sub & GX_CP_COMMAND_MASK;
    const uint8_t idx = sub & 0x0F;
    switch (group) {
    case GX_CP_MATINDEX_A:
        // PosNormal[0:6], Tex0[6:6], Tex1[12:6], Tex2[18:6], Tex3[24:6].
        p->geom_mtx_index    = value & 0x3F;
        p->tex_mtx_index[0]  = (value >> 6)  & 0x3F;
        p->tex_mtx_index[1]  = (value >> 12) & 0x3F;
        p->tex_mtx_index[2]  = (value >> 18) & 0x3F;
        p->tex_mtx_index[3]  = (value >> 24) & 0x3F;
        break;
    case GX_CP_MATINDEX_B:
        // Tex4[0:6], Tex5[6:6], Tex6[12:6], Tex7[18:6].
        p->tex_mtx_index[4]  = value & 0x3F;
        p->tex_mtx_index[5]  = (value >> 6)  & 0x3F;
        p->tex_mtx_index[6]  = (value >> 12) & 0x3F;
        p->tex_mtx_index[7]  = (value >> 18) & 0x3F;
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

static void on_indexed(void *user, uint8_t array, uint32_t index, uint16_t address,
                       uint8_t size) {
    GXDrawPipeline *p = user;
    if (!p->cb.read_guest || array >= GX_ARRAY_COUNT || size == 0) return;
    const uint32_t ea = p->arr.base[array] + index * p->arr.stride[array];
    const uint8_t *data = p->cb.read_guest(p->cb.user, ea, (uint32_t)size * 4u);
    if (data) gx_xf_apply_xf(&p->xf, address, size, data);
}

static void on_bp(void *user, uint8_t command, uint32_t value) {
    GXDrawPipeline *p = user;
    gx_state_apply_bp(&p->render, command, value);
    gx_tev_apply_bp(&p->tev, command, value);
    refresh_tev_pixel_state(p);
    gx_texture_cache_apply_bp(&p->texture, command, value);
    GXClearState clear;
    if (gx_state_take_clear(&p->render, &clear) && !p->dry_run)
        record_clear(p, &clear);
    GXCopyState copy;
    if (gx_state_take_copy(&p->render, &copy) && !p->dry_run)
        record_copy(p, &copy);
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
    for (uint32_t s = 0; s < tev->num_stages; ++s) {
        const uint32_t ind = tev->stage[s].indirect;
        if (((ind >> 9) & 3u) == 0 && ((ind >> 7) & 3u) == 0) continue;
        used[tev->indirect_stage[ind & 3u].texcoord & 7u] = true;
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
    memcpy(out->stage, p->tev.stage, (size_t)out->num_stages * sizeof(out->stage[0]));
    memcpy(out->swap, p->tev.swap, sizeof(out->swap));
    out->pixel = p->tev.pixel;
    memcpy(out->indirect_stage, p->tev.indirect_stage, sizeof(out->indirect_stage));
    for (uint32_t s = 0; s < out->num_stages; ++s)
        if (out->stage[s].colorchan == GX_RAS_COLOR1)
            out->has_color1 = true;
    out->num_texcoords = distinct_texcoords(&p->tev, out->tex_slot);
}

static bool sig_equal(const GXDrawShaderSig *a, const GXDrawShaderSig *b) {
    if (!a->valid || !b->valid || a->transform != b->transform ||
        a->has_color1 != b->has_color1 ||
        a->num_stages != b->num_stages || a->num_texcoords != b->num_texcoords) {
        return false;
    }
    return memcmp(a->stage, b->stage, (size_t)a->num_stages * sizeof(a->stage[0])) == 0 &&
           memcmp(a->swap, b->swap, sizeof(a->swap)) == 0 &&
           memcmp(&a->pixel, &b->pixel, sizeof(a->pixel)) == 0 &&
           memcmp(a->indirect_stage, b->indirect_stage, sizeof(a->indirect_stage)) == 0 &&
           memcmp(a->tex_slot, b->tex_slot, a->num_texcoords) == 0 &&
           memcmp(a->texgen, b->texgen, a->num_texcoords * sizeof(a->texgen[0])) == 0;
}

static bool cache_entry_in_use(const GXDrawPipeline *p, uint32_t index) {
    for (uint32_t i = 0; i < p->nrecords; ++i)
        if (p->record[i].type == GX_DRAW_RECORD_DRAW &&
            p->record[i].shader_cache_index == index) return true;
    return false;
}

static int cache_slot_for(GXDrawPipeline *p, const GXDrawShaderSig *sig) {
    for (uint32_t i = 0; i < GX_DRAW_SHADER_CACHE_CAP; ++i) {
        GXDrawShaderCacheEntry *entry = &p->shader_cache[i];
        if (!entry->sig.valid || !sig_equal(&entry->sig, sig)) continue;
        entry->last_used = ++p->shader_cache_clock;
        p->shader_cache_hits++;
        return (int)i;
    }

    p->shader_cache_misses++;
    int slot = -1;
    uint32_t oldest = UINT32_MAX;
    for (uint32_t i = 0; i < GX_DRAW_SHADER_CACHE_CAP; ++i) {
        GXDrawShaderCacheEntry *entry = &p->shader_cache[i];
        if (!entry->sig.valid) {
            slot = (int)i;
            break;
        }
        if (!cache_entry_in_use(p, i) && entry->last_used < oldest) {
            oldest = entry->last_used;
            slot = (int)i;
        }
    }
    if (slot < 0) return -1;

    GXDrawShaderCacheEntry *entry = &p->shader_cache[slot];
    if (entry->sig.valid) {
        GX2DrawDone();
        gx2_bind_free(&entry->bound);
        p->shader_cache_evictions++;
    }
    entry->sig = *sig;
    entry->last_used = ++p->shader_cache_clock;
    return slot;
}

static bool build_cached_shader(GXDrawPipeline *p, const GXDrawShaderSig *sig,
                                uint32_t ntc, int slot) {
    GX2AttribStream attribs[3 + 8];
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
    if (sig->has_color1) {
        attribs[na++] = (GX2AttribStream){
            .location = 2, .buffer = GX_DRAW_BUF_COL1, .offset = 0,
            .format = GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32,
            .type = GX2_ATTRIB_INDEX_PER_VERTEX, .aluDivisor = 0,
            .mask = GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_Z, GX2_SQ_SEL_W),
            .endianSwap = GX2_ENDIAN_SWAP_DEFAULT};
    }
    for (uint32_t k = 0; k < ntc; ++k) {
        attribs[na++] = (GX2AttribStream){
            .location = 2 + (sig->has_color1 ? 1u : 0u) + k,
            .buffer = 2 + (sig->has_color1 ? 1u : 0u) + k, .offset = 0,
            .format = GX2_ATTRIB_FORMAT_FLOAT_32_32,
            .type = GX2_ATTRIB_INDEX_PER_VERTEX, .aluDivisor = 0,
            .mask = GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_0, GX2_SQ_SEL_1),
            .endianSwap = GX2_ENDIAN_SWAP_DEFAULT};
    }

    GXDrawShaderCacheEntry *entry = &p->shader_cache[slot];
    if (!gx2_bind_build_tev_ex(&entry->bound, &p->tev, sig->transform, sig->texgen,
                               attribs, na)) {
        memset(entry, 0, sizeof(*entry));
        return false;
    }
    return true;
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

static bool ensure_buffer(GXDrawRecord *rec, int idx, uint32_t elem_size,
                          uint32_t elem_count) {
    GX2RBuffer *b = &rec->buffer[idx];
    if (rec->buffer_cap[idx] >= elem_count && b->elemSize == elem_size) return true;
    if (rec->buffer_cap[idx]) {
        GX2RDestroyBufferEx(b, 0);
        rec->buffer_cap[idx] = 0;
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
    rec->buffer_cap[idx] = elem_count;
    return true;
}

static void release_record_buffers(GXDrawRecord *rec) {
    for (int i = 0; i < GX_DRAW_BUF_COUNT; ++i)
        if (rec->buffer_cap[i]) {
            GX2RDestroyBufferEx(&rec->buffer[i], 0);
            rec->buffer_cap[i] = 0;
        }
}

static void release_records(GXDrawPipeline *p, bool wait_for_gpu) {
    if (p->nrecords && wait_for_gpu) GX2DrawDone();
    for (uint32_t i = 0; i < p->nrecords; ++i) {
        release_record_buffers(&p->record[i]);
        memset(&p->record[i], 0, sizeof(p->record[i]));
    }
    p->nrecords = 0;
}

static void record_clear(GXDrawPipeline *p, const GXClearState *clear) {
    if (p->nrecords >= GX_DRAW_MAX_RECORDS) return;
    GXDrawRecord *rec = &p->record[p->nrecords++];
    memset(rec, 0, sizeof(*rec));
    rec->type = GX_DRAW_RECORD_CLEAR;
    rec->clear = *clear;
}

static void record_copy(GXDrawPipeline *p, const GXCopyState *copy) {
    if (p->nrecords >= GX_DRAW_MAX_RECORDS) return;
    GXDrawRecord *rec = &p->record[p->nrecords++];
    memset(rec, 0, sizeof(*rec));
    rec->type = GX_DRAW_RECORD_COPY;
    rec->copy = *copy;
}

// EFB-copy encode scratch
static uint8_t g_copy_scratch[GX_EFB_WIDTH * GX_EFB_HEIGHT * 4];

// Decode targets
static float g_dry_pos[GX_DRAW_DRY_MAX * 3];
static float g_dry_col[GX_DRAW_DRY_MAX * 4];
static float g_dry_col1[GX_DRAW_DRY_MAX * 4];
static float g_dry_tex[8][GX_DRAW_DRY_MAX * 2];

static size_t load_and_prepare_vertices(const GXDrawPipeline *p, uint8_t vat,
                                        const GXVertexLayout *layout, uint32_t num_vertices,
                                        const uint8_t *vertex_data, size_t vertex_bytes,
                                        bool transform, float *pos, float *col0, float *col1,
                                        float *final_tex[8]) {
    float *normal = calloc((size_t)num_vertices * 9u, sizeof(*normal));
    float *raw_tex_storage = calloc((size_t)num_vertices * 8u * 2u, sizeof(*raw_tex_storage));
    uint8_t *pmi = calloc(num_vertices, sizeof(*pmi));
    uint8_t *tex_mtx_storage = calloc((size_t)num_vertices * 8u, sizeof(*tex_mtx_storage));
    if (!normal || !raw_tex_storage || !pmi || !tex_mtx_storage) {
        free(normal);
        free(raw_tex_storage);
        free(pmi);
        free(tex_mtx_storage);
        return 0;
    }

    float *raw_tex[8];
    uint8_t *tex_mtx[8];
    for (uint32_t tc = 0; tc < 8; ++tc) {
        raw_tex[tc] = raw_tex_storage + (size_t)tc * num_vertices * 2u;
        tex_mtx[tc] = tex_mtx_storage + (size_t)tc * num_vertices;
    }
    for (uint32_t v = 0; v < num_vertices; ++v)
        for (uint32_t c = 0; c < 4; ++c)
            col0[v * 4u + c] = col1[v * 4u + c] = 1.0f;

    const size_t used = gx_vertex_load(&p->fifo, vat, &p->arr, p->cb.read_guest, p->cb.user,
                                       vertex_data, vertex_bytes, num_vertices, pos, col0, col1,
                                       normal, raw_tex, pmi, tex_mtx);
    if (used == 0) {
        free(normal);
        free(raw_tex_storage);
        free(pmi);
        free(tex_mtx_storage);
        return 0;
    }

    const uint32_t num_texgens = p->xf.num_texgens > 8 ? 8 : p->xf.num_texgens;
    for (uint32_t v = 0; v < num_vertices; ++v) {
        float raw_pos[3] = {pos[v * 3u + 0], pos[v * 3u + 1], pos[v * 3u + 2]};
        float raw_normal[9];
        memcpy(raw_normal, normal + (size_t)v * 9u, sizeof(raw_normal));
        if (!layout->has_color1)
            memcpy(col1 + (size_t)v * 4u, col0 + (size_t)v * 4u, 4 * sizeof(float));
        if (!layout->has_pos_mtx_idx) pmi[v] = (uint8_t)p->geom_mtx_index;
        for (uint32_t tc = 0; tc < 8; ++tc)
            if (!layout->has_tex_mtx_idx[tc]) tex_mtx[tc][v] = p->tex_mtx_index[tc];

        float mv_pos[3] = {raw_pos[0], raw_pos[1], raw_pos[2]};
        if (transform) gx_xf_transform_position(&p->xf, pmi[v], raw_pos, mv_pos);
        memcpy(pos + (size_t)v * 3u, mv_pos, sizeof(mv_pos));

        float mv_normal[9] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        if (layout->has_normal)
            gx_xf_transform_normal(&p->xf, pmi[v], raw_normal, mv_normal);

        float input_color[2][4];
        memcpy(input_color[0], col0 + (size_t)v * 4u, sizeof(input_color[0]));
        memcpy(input_color[1], col1 + (size_t)v * 4u, sizeof(input_color[1]));
        float lit_color[2][4];
        memcpy(lit_color, input_color, sizeof(lit_color));
        if (p->xf.num_color_chans > 0)
            gx_xf_light_vertex(&p->xf, 0, mv_pos, mv_normal, input_color, lit_color[0]);
        if (p->xf.num_color_chans > 1)
            gx_xf_light_vertex(&p->xf, 1, mv_pos, mv_normal, input_color, lit_color[1]);
        memcpy(col0 + (size_t)v * 4u, lit_color[0], sizeof(lit_color[0]));
        memcpy(col1 + (size_t)v * 4u, lit_color[1], sizeof(lit_color[1]));

        float raw_uv[8][2];
        for (uint32_t tc = 0; tc < 8; ++tc)
            memcpy(raw_uv[tc], raw_tex[tc] + (size_t)v * 2u, sizeof(raw_uv[tc]));
        float generated[8][3] = {{0}};
        for (uint32_t tc = 0; tc < num_texgens; ++tc)
            gx_xf_generate_texcoord(&p->xf, tc, raw_pos, raw_normal, mv_pos, mv_normal,
                                    lit_color, raw_uv, tex_mtx[tc][v], generated,
                                    generated[tc]);
        for (uint32_t tc = 0; tc < 8; ++tc) {
            if (!final_tex[tc]) continue;
            float *out = final_tex[tc] + (size_t)v * 2u;
            if (tc < num_texgens) {
                out[0] = generated[tc][0];
                out[1] = generated[tc][1];
            } else {
                out[0] = raw_uv[tc][0];
                out[1] = raw_uv[tc][1];
            }
        }
    }

    free(normal);
    free(raw_tex_storage);
    free(pmi);
    free(tex_mtx_storage);
    return used;
}

static void draw_primitive(GXDrawPipeline *p, GXPrimitive prim, uint8_t vat,
                           uint16_t num_vertices, const uint8_t *vertex_data,
                           uint32_t vertex_size) {
    if (num_vertices == 0) return;
    if (!p->dry_run && p->nrecords >= GX_DRAW_MAX_RECORDS) return;

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
            tex[s] = g_dry_tex[s];
        }
        size_t used = load_and_prepare_vertices(p, vat, &layout, num_vertices, vertex_data,
                                                (size_t)num_vertices * vertex_size, transform,
                                                g_dry_pos, g_dry_col, g_dry_col1, tex);
        if (used == 0) return;
        p->prims++;
        p->verts += num_vertices;
        return;
    }

    GXDrawRecord *rec = &p->record[p->nrecords];
    memset(rec, 0, sizeof(*rec));
    rec->type = GX_DRAW_RECORD_DRAW;
    if (!ensure_buffer(rec, GX_DRAW_BUF_POS, 3 * sizeof(float), num_vertices)) return;
    if (!ensure_buffer(rec, GX_DRAW_BUF_COL0, 4 * sizeof(float), num_vertices)) {
        release_record_buffers(rec);
        return;
    }
    if (sig.has_color1 &&
        !ensure_buffer(rec, GX_DRAW_BUF_COL1, 4 * sizeof(float), num_vertices)) {
        release_record_buffers(rec);
        return;
    }
    for (uint32_t k = 0; k < ntc; ++k)
        if (!ensure_buffer(rec, GX_DRAW_BUF_TEX0 + slots[k], 2 * sizeof(float),
                           num_vertices)) {
            release_record_buffers(rec);
            return;
        }

    float *pos = GX2RLockBufferEx(&rec->buffer[GX_DRAW_BUF_POS], 0);
    float *col = GX2RLockBufferEx(&rec->buffer[GX_DRAW_BUF_COL0], 0);
    float *col1 = sig.has_color1 ? GX2RLockBufferEx(&rec->buffer[GX_DRAW_BUF_COL1], 0) :
                                  calloc((size_t)num_vertices * 4u, sizeof(*col1));
    float *tex[8] = {0};
    float *tex_locked[8] = {0};
    for (uint32_t k = 0; k < ntc; ++k) {
        uint8_t s = slots[k];
        tex_locked[s] = GX2RLockBufferEx(&rec->buffer[GX_DRAW_BUF_TEX0 + s], 0);
        tex[s] = tex_locked[s];
    }

    if (!pos || !col || !col1) {
        if (pos) GX2RUnlockBufferEx(&rec->buffer[GX_DRAW_BUF_POS], 0);
        if (col) GX2RUnlockBufferEx(&rec->buffer[GX_DRAW_BUF_COL0], 0);
        if (col1 && sig.has_color1) GX2RUnlockBufferEx(&rec->buffer[GX_DRAW_BUF_COL1], 0);
        if (col1 && !sig.has_color1) free(col1);
        for (uint32_t k = 0; k < ntc; ++k)
            if (tex_locked[slots[k]])
                GX2RUnlockBufferEx(&rec->buffer[GX_DRAW_BUF_TEX0 + slots[k]], 0);
        release_record_buffers(rec);
        return;
    }
    for (uint32_t k = 0; k < ntc; ++k)
        if (!tex_locked[slots[k]]) {
            GX2RUnlockBufferEx(&rec->buffer[GX_DRAW_BUF_POS], 0);
            GX2RUnlockBufferEx(&rec->buffer[GX_DRAW_BUF_COL0], 0);
            if (sig.has_color1) GX2RUnlockBufferEx(&rec->buffer[GX_DRAW_BUF_COL1], 0);
            else free(col1);
            for (uint32_t j = 0; j < ntc; ++j)
                if (tex_locked[slots[j]])
                    GX2RUnlockBufferEx(&rec->buffer[GX_DRAW_BUF_TEX0 + slots[j]], 0);
            release_record_buffers(rec);
            return;
        }

    size_t used = load_and_prepare_vertices(p, vat, &layout, num_vertices, vertex_data,
                                            (size_t)num_vertices * vertex_size, transform,
                                            pos, col, col1, tex);

    GX2RUnlockBufferEx(&rec->buffer[GX_DRAW_BUF_POS], 0);
    GX2RUnlockBufferEx(&rec->buffer[GX_DRAW_BUF_COL0], 0);
    if (sig.has_color1) GX2RUnlockBufferEx(&rec->buffer[GX_DRAW_BUF_COL1], 0);
    else free(col1);
    for (uint32_t k = 0; k < ntc; ++k)
        GX2RUnlockBufferEx(&rec->buffer[GX_DRAW_BUF_TEX0 + slots[k]], 0);

    if (used == 0) {
        release_record_buffers(rec);
        return;
    }

    int shader_slot = cache_slot_for(p, &sig);
    if (shader_slot < 0) {
        release_record_buffers(rec);
        return;
    }
    if (!p->shader_cache[shader_slot].bound.valid &&
        !build_cached_shader(p, &sig, ntc, shader_slot)) {
        release_record_buffers(rec);
        return;
    }

    // Record the draw for replay inside the render pass.
    rec->mode  = prim_mode(prim);
    rec->count = num_vertices;
    rec->ntc   = ntc;
    rec->shader_cache_index = (uint8_t)shader_slot;
    memcpy(rec->slots, slots, sizeof(rec->slots));
    gx_xf_build_projection_cfile(&p->xf, rec->vs_cfile);
    rec->vs_cfile_count = 16;
    gx_tev_build_ps_cfile(&p->tev, rec->ps_cfile);
    memcpy(rec->texture_unit, p->texture.unit, sizeof(rec->texture_unit));
    for (uint32_t i = 0; i < GX_TEXTURE_MAX_UNITS; ++i)
        gx_texture_sampler_from_unit(&rec->sampler[i], &rec->texture_unit[i]);
    gx_state_depth(&p->render, &rec->depth);
    gx_state_blend(&p->render, &rec->blend);
    gx_state_cull(&p->render, &rec->cull);
    gx_state_viewport(&p->render, p->xf.viewport, &rec->viewport);
    gx_state_scissor(&p->render, &rec->scissor);

    p->nrecords++;
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
    gx_efb_apply_color_mask(rec->blend.color_update, rec->blend.alpha_update);
    GX2SetBlendControl(GX2_RENDER_TARGET_0, rec->blend.src_color, rec->blend.dst_color,
                       rec->blend.color_combine, TRUE, rec->blend.src_alpha,
                       rec->blend.dst_alpha, rec->blend.alpha_combine);
    GX2SetCullOnlyControl(rec->cull.front_face, rec->cull.cull_front,
                          rec->cull.cull_back);
}

// Resolve the colour buffer and encode a triggered EFB copy into guest memory.
static void replay_copy(GXDrawPipeline *p, const GXCopyState *copy) {
    // TODO: YUV XFB copies feed VI/XFB presentations
    if (copy->to_xfb || copy->half_scale || copy->width == 0 || copy->height == 0) return;

    const GXCopyFormat fmt = (GXCopyFormat)copy->format;
    const GXTextureFormat layout = gx_texture_copy_layout(fmt);
    const int size = gx_texture_encoded_size((int)copy->width, (int)copy->height, layout);
    if (size <= 0 || (size_t)size > sizeof(g_copy_scratch)) return;

    uint32_t pitch = 0;
    const uint8_t *linear = gx_efb_resolve_color(&p->efb, &pitch);
    if (!linear) return;
    const uint8_t *region = linear + (size_t)copy->src_y * pitch + (size_t)copy->src_x * 4u;
    const int written = gx_texture_encode_copy(g_copy_scratch, region, (int)copy->width,
                                               (int)copy->height, pitch, fmt, copy->intensity);
    if (written <= 0) return;
    if (p->cb.write_guest)
        p->cb.write_guest(p->cb.user, copy->dst_ea, g_copy_scratch, (uint32_t)written);
    gx_texture_cache_invalidate_range(&p->texture, copy->dst_ea, (uint32_t)written);
}

bool gx_draw_replay(GXDrawPipeline *p) {
    if (!p || !p->live_replay || p->nrecords == 0 || !gx_efb_bind(&p->efb)) return false;

    for (uint32_t r = 0; r < p->nrecords; ++r) {
        GXDrawRecord *rec = &p->record[r];
        if (rec->type == GX_DRAW_RECORD_CLEAR) {
            (void)gx_efb_clear(&p->efb, &rec->clear);
            continue;
        }
        if (rec->type == GX_DRAW_RECORD_COPY) {
            replay_copy(p, &rec->copy);
            gx_efb_bind(&p->efb);
            continue;
        }
        const GXDrawShaderCacheEntry *entry = &p->shader_cache[rec->shader_cache_index];
        if (!entry->bound.valid) continue;
        gx_efb_apply_viewport(&rec->viewport);
        gx_efb_apply_scissor(&rec->scissor);
        GX2SetFetchShader(&entry->bound.fs);
        GX2SetVertexShader(&entry->bound.vs);
        GX2SetPixelShader(&entry->bound.ps);
        GX2SetPixelUniformReg(0, GX_TEV_PS_CFILE_COUNT * 4, (void *)&rec->ps_cfile[0][0]);
        for (uint32_t i = 0; i < entry->bound.ps_sampler_count; ++i) {
            uint32_t loc = entry->bound.ps_samplers[i].location;
            if (loc >= GX_TEXTURE_MAX_UNITS) continue;
            GX2Texture *t = p->cb.get_texture ? p->cb.get_texture(p->cb.user, (uint8_t)loc)
                                              : gx_texture_cache_get_texture_unit(
                                                    &p->texture, &rec->texture_unit[loc]);
            const GX2Sampler *s = p->cb.get_sampler ? p->cb.get_sampler(p->cb.user, (uint8_t)loc)
                                                    : &rec->sampler[loc];
            if (t) GX2SetPixelTexture(t, loc);
            if (s) GX2SetPixelSampler(s, loc);
        }
        GX2SetVertexUniformReg(0, rec->vs_cfile_count, (void *)rec->vs_cfile);
        apply_state(rec);
        GX2RSetAttributeBuffer(&rec->buffer[GX_DRAW_BUF_POS], 0,
                               rec->buffer[GX_DRAW_BUF_POS].elemSize, 0);
        GX2RSetAttributeBuffer(&rec->buffer[GX_DRAW_BUF_COL0], 1,
                               rec->buffer[GX_DRAW_BUF_COL0].elemSize, 0);
        const uint32_t tex_binding_base = entry->sig.has_color1 ? 3u : 2u;
        if (entry->sig.has_color1)
            GX2RSetAttributeBuffer(&rec->buffer[GX_DRAW_BUF_COL1], 2,
                                   rec->buffer[GX_DRAW_BUF_COL1].elemSize, 0);
        for (uint32_t k = 0; k < rec->ntc; ++k) {
            int bi = GX_DRAW_BUF_TEX0 + rec->slots[k];
            GX2RSetAttributeBuffer(&rec->buffer[bi], tex_binding_base + k,
                                   rec->buffer[bi].elemSize, 0);
        }
        GX2DrawEx(rec->mode, rec->count, 0, 1);
    }
    return true;
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
    if (!gx_texture_cache_init(&p->texture, cb->read_guest, cb->user)) return false;
    p->dry_run = true;
    gx_draw_reset_state(p);
    p->ok = true;
    return true;
}

static void gx_draw_shutdown_impl(GXDrawPipeline *p, bool wait_for_gpu) {
    if (!p) return;
    release_records(p, wait_for_gpu);
    for (uint32_t i = 0; i < GX_DRAW_SHADER_CACHE_CAP; ++i)
        if (p->shader_cache[i].sig.valid)
            gx2_bind_free(&p->shader_cache[i].bound);
    gx_efb_shutdown(&p->efb);
    if (wait_for_gpu)
        gx_texture_cache_destroy(&p->texture);
    else
        gx_texture_cache_destroy_after_gpu_idle(&p->texture);
    p->ok = false;
}

void gx_draw_shutdown(GXDrawPipeline *p) {
    gx_draw_shutdown_impl(p, true);
}

void gx_draw_shutdown_after_gpu_idle(GXDrawPipeline *p) {
    gx_draw_shutdown_impl(p, false);
}

void gx_draw_reset_state(GXDrawPipeline *p) {
    if (!p) return;
    gx_fifo_state_reset(&p->fifo);
    gx_xf_reset(&p->xf);
    gx_tev_reset(&p->tev);
    gx_state_reset(&p->render);
    refresh_tev_pixel_state(p);
    gx_texture_cache_reset_state(&p->texture);
    memset(&p->arr, 0, sizeof(p->arr));
    p->geom_mtx_index = 0;
    memset(p->tex_mtx_index, 0, sizeof(p->tex_mtx_index));
}

void gx_draw_begin_frame(GXDrawPipeline *p) {
    if (!p) return;
    release_records(p, true);
    p->prims = 0;
    p->verts = 0;
}

bool gx_draw_enable_live_replay(GXDrawPipeline *p) {
    if (!p) return false;
    if (p->live_replay) return true;
    if (!gx_efb_init(&p->efb)) return false;
    p->dry_run = false;
    p->live_replay = true;
    return true;
}

size_t gx_draw_submit(GXDrawPipeline *p, const uint8_t *fifo, size_t len) {
    if (!p || !fifo) return 0;
    const GXFifoSink sink = {
        .on_cp = on_cp,
        .on_xf = on_xf,
        .on_indexed = on_indexed,
        .on_bp = on_bp,
        .on_primitive = on_primitive,
        .resolve_dl = resolve_dl,
    };
    return gx_fifo_run(&p->fifo, fifo, len, &sink, p);
}

uint32_t gx_draw_execute(GXDrawPipeline *p, const uint8_t *fifo, size_t len) {
    if (!p || !fifo) return 0;
    gx_draw_begin_frame(p);
    gx_draw_submit(p, fifo, len);
    return p->prims;
}

GXDrawShaderCacheStats gx_draw_shader_cache_stats(const GXDrawPipeline *p) {
    GXDrawShaderCacheStats stats = {0};
    if (!p) return stats;
    stats.hits = p->shader_cache_hits;
    stats.misses = p->shader_cache_misses;
    stats.evictions = p->shader_cache_evictions;
    for (uint32_t i = 0; i < GX_DRAW_SHADER_CACHE_CAP; ++i)
        if (p->shader_cache[i].sig.valid) stats.entries++;
    return stats;
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

static const uint8_t *draw_test_read(void *user, uint32_t ea, uint32_t len) {
    const uint8_t *data = user;
    if (ea < 0x80018000u || ea - 0x80018000u + len > 96u) return NULL;
    return data + ea - 0x80018000u;
}

int gx_draw_selftest(void) {
    static GXDrawPipeline p;
    uint8_t guest[96] = {0};
    GXDrawCallbacks cb = {.read_guest = draw_test_read, .user = guest};
    if (!gx_draw_init(&p, &cb)) return 0;
    p.dry_run = true;

    size_t gn = 48;
    const float indexed_mtx[12] = {
        1, 0, 0, 4,
        0, 1, 0, 5,
        0, 0, 1, 6,
    };
    for (uint32_t i = 0; i < 12; ++i) put_f(guest, &gn, indexed_mtx[i]);
    p.arr.base[12] = 0x80018000u;
    p.arr.stride[12] = 48;
    uint8_t indexed[5] = {GX_OPCODE_LOAD_INDX_A, 0, 1, 0xB0,
                          (uint8_t)(XF_POSMATRICES + 4)};
    if (gx_draw_submit(&p, indexed, sizeof(indexed)) != sizeof(indexed) ||
        p.xf.pos_matrices[4] != 1.0f || p.xf.pos_matrices[7] != 4.0f ||
        p.xf.pos_matrices[11] != 5.0f || p.xf.pos_matrices[15] != 6.0f) {
        gx_draw_shutdown(&p);
        return 0;
    }

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

    GXDrawShaderSig sig0;
    GXDrawShaderSig sig1;
    build_sig(&p, true, &sig0);
    p.tev.color[0][0] = 0.5f;
    build_sig(&p, true, &sig1);
    if (!sig_equal(&sig0, &sig1)) ok = 0;
    p.tev.stage[0].texmap = 1;
    build_sig(&p, true, &sig1);
    if (sig_equal(&sig0, &sig1)) ok = 0;

    p.shader_cache[0].sig = sig0;
    p.shader_cache[0].bound.valid = true;
    p.shader_cache[0].last_used = 1;
    if (cache_slot_for(&p, &sig0) != 0) ok = 0;
    if (cache_slot_for(&p, &sig1) != 1) ok = 0;
    GXDrawShaderCacheStats cache_stats = gx_draw_shader_cache_stats(&p);
    if (cache_stats.entries != 2 || cache_stats.hits != 1 || cache_stats.misses != 1)
        ok = 0;

    gx_draw_shutdown(&p);
    return ok;
}
