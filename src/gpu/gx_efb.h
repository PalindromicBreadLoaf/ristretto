// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

// Guest EFB resources and their GX2 state bridge.

#ifndef RISTRETTO_GPU_GX_EFB_H
#define RISTRETTO_GPU_GX_EFB_H

#include <stdbool.h>

#include <gx2/context.h>
#include <gx2/surface.h>
#include <gx2r/buffer.h>

#include "gpu/gx2_bind.h"
#include "gpu/gx_state.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    GX_EFB_WIDTH = 640,
    GX_EFB_HEIGHT = 528,
};

typedef struct {
    GX2ContextState *context;
    GX2ColorBuffer color;
    GX2DepthBuffer depth;
    Gx2BoundShader clear_shader;
    GX2RBuffer clear_position;
    GX2RBuffer clear_color;
    GX2Surface stage;
    bool stage_ready;
    bool ready;
} GXEfb;

bool gx_efb_init(GXEfb *efb);
void gx_efb_shutdown(GXEfb *efb);

bool gx_efb_bind(GXEfb *efb);
void gx_efb_apply_viewport(const GXViewportState *viewport);
void gx_efb_apply_scissor(const GXScissorState *scissor);
void gx_efb_apply_color_mask(bool color_enable, bool alpha_enable);
bool gx_efb_clear(GXEfb *efb, const GXClearState *clear);

// Resolve the tiled colour buffer into a linear RGBA8 surface and return a
// CPU-readable pointer to its pixels.
const uint8_t *gx_efb_resolve_color(GXEfb *efb, uint32_t *pitch);

#ifdef __cplusplus
}
#endif

#endif  // RISTRETTO_GPU_GX_EFB_H
