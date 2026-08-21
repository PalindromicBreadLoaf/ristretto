// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

// GX2 vertex/pixel shader register-state builder.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define GX2_MAX_VS_INPUTS   32
#define GX2_MAX_VS_EXPORTS  40
#define GX2_MAX_PS_INPUTS   32

// Mirror of GX2VertexShader::regs (wut).
typedef struct {
    uint32_t sq_pgm_resources_vs;
    uint32_t vgt_primitiveid_en;
    uint32_t spi_vs_out_config;
    uint32_t num_spi_vs_out_id;
    uint32_t spi_vs_out_id[10];
    uint32_t pa_cl_vs_out_cntl;
    uint32_t sq_vtx_semantic_clear;
    uint32_t num_sq_vtx_semantic;
    uint32_t sq_vtx_semantic[32];
    uint32_t vgt_strmout_buffer_en;
    uint32_t vgt_vertex_reuse_block_cntl;
    uint32_t vgt_hos_reuse_depth;
} Gx2VsRegs;

// Mirror of GX2PixelShader::regs (wut).
typedef struct {
    uint32_t sq_pgm_resources_ps;
    uint32_t sq_pgm_exports_ps;
    uint32_t spi_ps_in_control_0;
    uint32_t spi_ps_in_control_1;
    uint32_t num_spi_ps_input_cntl;
    uint32_t spi_ps_input_cntls[32];
    uint32_t cb_shader_mask;
    uint32_t cb_shader_control;
    uint32_t db_shader_control;
    uint32_t spi_input_z;
} Gx2PsRegs;

typedef struct {
    uint32_t num_gprs;
    uint32_t stack_size;
    // sq_vtx_semantic table.
    uint8_t  input_semantics[GX2_MAX_VS_INPUTS];
    uint32_t num_inputs;
    // Param-export semantic ids.
    uint8_t  export_semantics[GX2_MAX_VS_EXPORTS];
    uint32_t num_exports;
} Gx2VsShape;

typedef struct {
    uint32_t num_gprs;
    uint32_t stack_size;
    // Interpolant semantic ids
    uint8_t  input_semantics[GX2_MAX_PS_INPUTS];
    uint32_t num_inputs;
    uint32_t num_color_exports;  // MRT count
} Gx2PsShape;

// Fill the register mirror from a shape.
bool gx2_vs_regs(Gx2VsRegs *out, const Gx2VsShape *shape);
bool gx2_ps_regs(Gx2PsRegs *out, const Gx2PsShape *shape);

bool gx2_shader_selftest(void);
