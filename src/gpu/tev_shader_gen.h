// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

// Multi-stage TEV to R700 pixel shader generator.

#ifndef RISTRETTO_GPU_TEV_SHADER_GEN_H
#define RISTRETTO_GPU_TEV_SHADER_GEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gpu/gx2_shader.h"
#include "gpu/gx_tev.h"

#ifdef __cplusplus
extern "C" {
#endif

// cfile register that holds the first konst colour K0.
#define TEV_PS_KONST_CFILE_BASE 4u
#define TEV_PS_PIXEL_CFILE_BASE 8u
#define TEV_PS_FORMAT_CFILE_BASE 9u

// Emit the R700 pixel program for cfg into buf.
size_t tev_shader_gen_ps(uint8_t *buf, size_t cap, const TevConfig *cfg);

// Fill the shader shape that matches the program.
void tev_shader_gen_ps_shape(const TevConfig *cfg, Gx2PsShape *out);

bool tev_shader_gen_selftest(void);

#ifdef __cplusplus
}
#endif

#endif  // RISTRETTO_GPU_TEV_SHADER_GEN_H
