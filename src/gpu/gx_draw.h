// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

// Guest draw pipeline

#ifndef RISTRETTO_GPU_GX_DRAW_H
#define RISTRETTO_GPU_GX_DRAW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <gx2/sampler.h>
#include <gx2/texture.h>
#include <gx2r/buffer.h>

#include "gpu/gx2_bind.h"
#include "gpu/gx_efb.h"
#include "gpu/gx_fifo.h"
#include "gpu/gx_state.h"
#include "gpu/gx_tev.h"
#include "gpu/gx_texture.h"
#include "gpu/gx_vertex.h"
#include "gpu/gx_xf.h"

#ifdef __cplusplus
extern "C" {
#endif

// Attribute buffer slots
enum {
    GX_DRAW_BUF_POS  = 0,
    GX_DRAW_BUF_COL0 = 1,
    GX_DRAW_BUF_COL1 = 2,
    GX_DRAW_BUF_TEX0 = 3,   // 3..10
    GX_DRAW_BUF_COUNT = 11,
};

// Host callbacks the pipeline needs.
typedef struct {
    // Resolve the GX2 texture/sampler bound to a GX texmap the TEV samples.
    GX2Texture *(*get_texture)(void *user, uint8_t texmap);
    GX2Sampler *(*get_sampler)(void *user, uint8_t texmap);
    GXGuestRead read_guest;
    void       *user;
} GXDrawCallbacks;

// Maximum replay operations retained for a prepared stream.
#define GX_DRAW_MAX_RECORDS 128

// Generated shader trios retained by a draw pipeline.
#define GX_DRAW_SHADER_CACHE_CAP 64

// The shader-structure signature a bound shader was built for.
typedef struct {
    bool     valid;
    bool     transform;
    bool     has_color1;
    uint8_t  num_stages;
    uint32_t num_texcoords;
    TevStage stage[GX_TEV_MAX_STAGES];
    uint8_t  swap[4][4];
    TevPixelState pixel;
    TevIndirectStage indirect_stage[4];
    uint8_t  tex_slot[8];   // distinct sampled texcoord slots
    bool     texgen[8];
} GXDrawShaderSig;

typedef struct {
    Gx2BoundShader bound;
    GXDrawShaderSig sig;
    uint32_t        last_used;
} GXDrawShaderCacheEntry;

typedef struct {
    uint32_t hits;
    uint32_t misses;
    uint32_t evictions;
    uint32_t entries;
} GXDrawShaderCacheStats;

typedef enum {
    GX_DRAW_RECORD_DRAW,
    GX_DRAW_RECORD_CLEAR,
} GXDrawRecordType;

// One recorded EFB operation.
typedef struct {
    GXDrawRecordType type;
    GX2PrimitiveMode mode;
    uint32_t         count;
    uint32_t         ntc;
    uint8_t          shader_cache_index;
    uint8_t          slots[8];   // distinct sampled texcoord slots, ascending
    float            vs_cfile[16 + 12 * 8];
    uint32_t         vs_cfile_count;   // floats to upload
    float            ps_cfile[GX_TEV_PS_CFILE_COUNT][4];
    GXTextureUnit    texture_unit[GX_TEXTURE_MAX_UNITS];
    GX2Sampler       sampler[GX_TEXTURE_MAX_UNITS];
    GXDepthState     depth;
    GXBlendState     blend;
    GXCullState      cull;
    GXViewportState  viewport;
    GXScissorState   scissor;
    GXClearState     clear;
    GX2RBuffer       buffer[GX_DRAW_BUF_COUNT];
    uint32_t         buffer_cap[GX_DRAW_BUF_COUNT];
} GXDrawRecord;

typedef struct {
    GXDrawCallbacks cb;

    // Accumulated GX state
    GXFifoState   fifo;      // vertex descriptor + VATs
    XfConfig      xf;        // transform-unit registers
    TevConfig     tev;       // TEV combiner pipeline
    GXRenderState render;    // fixed-function render-state BP registers
    GXTextureCache texture;  // BP texture units, TLUT memory, and GX2 resources
    GXArrayTable  arr;       // CP array bases/strides for indexed attributes
    uint32_t      geom_mtx_index;   // MATINDEX_A position/normal matrix index
    uint8_t       tex_mtx_index[8]; // MATINDEX_A/B per-texcoord texture matrix index

    GXDrawShaderCacheEntry shader_cache[GX_DRAW_SHADER_CACHE_CAP];
    uint32_t               shader_cache_clock;
    uint32_t               shader_cache_hits;
    uint32_t               shader_cache_misses;
    uint32_t               shader_cache_evictions;

    GXEfb efb;
    bool  live_replay;

    // Draws recorded by the last prepare.
    GXDrawRecord record[GX_DRAW_MAX_RECORDS];
    uint32_t     nrecords;

    bool dry_run;

    uint32_t prims;
    uint32_t verts;
    bool     ok;
} GXDrawPipeline;

bool gx_draw_init(GXDrawPipeline *p, const GXDrawCallbacks *cb);

void gx_draw_shutdown(GXDrawPipeline *p);

void gx_draw_shutdown_after_gpu_idle(GXDrawPipeline *p);

void gx_draw_reset_state(GXDrawPipeline *p);

void gx_draw_begin_frame(GXDrawPipeline *p);

// Allocate the EFB targets required for replay.
bool gx_draw_enable_live_replay(GXDrawPipeline *p);

size_t gx_draw_submit(GXDrawPipeline *p, const uint8_t *fifo, size_t len);

// Prepare a GX FIFO command stream.
uint32_t gx_draw_execute(GXDrawPipeline *p, const uint8_t *fifo, size_t len);

bool gx_draw_replay(GXDrawPipeline *p);

GXDrawShaderCacheStats gx_draw_shader_cache_stats(const GXDrawPipeline *p);

int gx_draw_selftest(void);

#ifdef __cplusplus
}
#endif

#endif  // RISTRETTO_GPU_GX_DRAW_H
