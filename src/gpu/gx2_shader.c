// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu/gx2_shader.h"

#include <stddef.h>
#include <string.h>

// The mirror structs must match the wut GX2VertexShader::regs / GX2PixelShader::regs
// sub-structs byte-for-byte so console glue can memcpy a filled mirror in.
_Static_assert(sizeof(Gx2VsRegs) == 0xD0, "Gx2VsRegs must match GX2VertexShader::regs");
_Static_assert(sizeof(Gx2PsRegs) == 0xA4, "Gx2PsRegs must match GX2PixelShader::regs");
_Static_assert(offsetof(Gx2VsRegs, spi_vs_out_id) == 0x10, "vs regs layout");
_Static_assert(offsetof(Gx2VsRegs, pa_cl_vs_out_cntl) == 0x38, "vs regs layout");
_Static_assert(offsetof(Gx2VsRegs, sq_vtx_semantic) == 0x44, "vs regs layout");
_Static_assert(offsetof(Gx2VsRegs, vgt_hos_reuse_depth) == 0xCC, "vs regs layout");
_Static_assert(offsetof(Gx2PsRegs, spi_ps_input_cntls) == 0x14, "ps regs layout");
_Static_assert(offsetof(Gx2PsRegs, cb_shader_mask) == 0x94, "ps regs layout");
_Static_assert(offsetof(Gx2PsRegs, spi_input_z) == 0xA0, "ps regs layout");

// Register constants
enum {
    // SPI_PS_IN_CONTROL_0: PERSP_GRADIENT_ENA | BARYC_SAMPLE_CNTL.
    GX2_SPI_PS_IN_CONTROL_0_FLAGS = 0x14000000u,
    // SPI_PS_INPUT_CNTL_n: DEFAULT_VAL=1.
    GX2_SPI_PS_INPUT_CNTL_FLAGS   = 0x00000100u,
    GX2_CB_SHADER_CONTROL         = 0x00000001u,
    GX2_DB_SHADER_CONTROL         = 0x00000010u,
    GX2_VGT_VERTEX_REUSE_BLOCK    = 0x0000000Eu,
    GX2_VGT_HOS_REUSE_DEPTH       = 0x00000010u,
};

// SQ_PGM_RESOURCES: NUM_GPRS[7:0], STACK_SIZE[15:8].
static uint32_t sq_pgm_resources(uint32_t num_gprs, uint32_t stack_size)
{
    return (num_gprs & 0xFFu) | ((stack_size & 0xFFu) << 8);
}

static uint32_t used_mask(uint32_t n)
{
    return n >= 32 ? 0xFFFFFFFFu : ((1u << n) - 1u);
}

bool gx2_vs_regs(Gx2VsRegs *out, const Gx2VsShape *shape)
{
    if (!out || !shape) return false;
    if (shape->num_inputs > GX2_MAX_VS_INPUTS) return false;
    if (shape->num_exports > GX2_MAX_VS_EXPORTS) return false;

    memset(out, 0, sizeof(*out));

    out->sq_pgm_resources_vs = sq_pgm_resources(shape->num_gprs, shape->stack_size);

    // SPI_VS_OUT_CONFIG: VS_EXPORT_COUNT[6:1] = param count - 1.
    uint32_t export_count = shape->num_exports ? shape->num_exports - 1u : 0u;
    out->spi_vs_out_config = (export_count & 0x3Fu) << 1;

    // spi_vs_out_id
    uint32_t id_dwords = (shape->num_exports + 3u) / 4u;
    out->num_spi_vs_out_id = id_dwords;
    for (uint32_t i = 0; i < 10; ++i) {
        uint32_t w = 0;
        for (uint32_t lane = 0; lane < 4; ++lane) {
            uint32_t idx = i * 4u + lane;
            uint8_t sem = idx < shape->num_exports ? shape->export_semantics[idx] : 0xFFu;
            w |= (uint32_t)sem << (lane * 8u);
        }
        out->spi_vs_out_id[i] = w;
    }

    // sq_vtx_semantic
    out->sq_vtx_semantic_clear = ~used_mask(shape->num_inputs);
    out->num_sq_vtx_semantic = shape->num_inputs;
    for (uint32_t i = 0; i < 32; ++i)
        out->sq_vtx_semantic[i] =
            i < shape->num_inputs ? shape->input_semantics[i] : 0xFFu;

    out->vgt_vertex_reuse_block_cntl = GX2_VGT_VERTEX_REUSE_BLOCK;
    out->vgt_hos_reuse_depth = GX2_VGT_HOS_REUSE_DEPTH;
    return true;
}

bool gx2_ps_regs(Gx2PsRegs *out, const Gx2PsShape *shape)
{
    if (!out || !shape) return false;
    if (shape->num_inputs > GX2_MAX_PS_INPUTS) return false;
    if (shape->num_color_exports > 8) return false;

    memset(out, 0, sizeof(*out));

    out->sq_pgm_resources_ps = sq_pgm_resources(shape->num_gprs, shape->stack_size);

    // SQ_PGM_EXPORTS_PS
    out->sq_pgm_exports_ps = shape->num_color_exports << 1;

    // SPI_PS_IN_CONTROL_0
    out->spi_ps_in_control_0 =
        (shape->num_inputs & 0x3Fu) | GX2_SPI_PS_IN_CONTROL_0_FLAGS;

    out->num_spi_ps_input_cntl = shape->num_inputs;
    for (uint32_t i = 0; i < shape->num_inputs; ++i)
        out->spi_ps_input_cntls[i] =
            shape->input_semantics[i] | GX2_SPI_PS_INPUT_CNTL_FLAGS;

    // cb_shader_mask
    for (uint32_t i = 0; i < shape->num_color_exports; ++i)
        out->cb_shader_mask |= 0xFu << (i * 4u);

    out->cb_shader_control = GX2_CB_SHADER_CONTROL;
    out->db_shader_control = GX2_DB_SHADER_CONTROL;
    return true;
}

// Self test
bool gx2_shader_selftest(void)
{
    // Vertex shader
    Gx2VsShape vs = {0};
    vs.num_gprs = 4;
    vs.stack_size = 1;
    vs.num_inputs = 3;
    vs.input_semantics[0] = 0;
    vs.input_semantics[1] = 1;
    vs.input_semantics[2] = 2;
    vs.num_exports = 2;
    vs.export_semantics[0] = 0;
    vs.export_semantics[1] = 1;

    Gx2VsRegs vr;
    if (!gx2_vs_regs(&vr, &vs)) return false;
    if (vr.sq_pgm_resources_vs != 0x00000104u) return false;
    if (vr.spi_vs_out_config != 0x00000002u) return false;
    if (vr.num_spi_vs_out_id != 1u) return false;
    if (vr.spi_vs_out_id[0] != 0xFFFF0100u) return false;
    if (vr.spi_vs_out_id[1] != 0xFFFFFFFFu) return false;
    if (vr.sq_vtx_semantic_clear != 0xFFFFFFF8u) return false;
    if (vr.num_sq_vtx_semantic != 3u) return false;
    if (vr.sq_vtx_semantic[0] != 0 || vr.sq_vtx_semantic[1] != 1 ||
        vr.sq_vtx_semantic[2] != 2 || vr.sq_vtx_semantic[3] != 0xFFu)
        return false;
    if (vr.vgt_vertex_reuse_block_cntl != 0x0000000Eu) return false;
    if (vr.vgt_hos_reuse_depth != 0x00000010u) return false;

    // Pixel shader
    Gx2PsShape ps = {0};
    ps.num_gprs = 2;
    ps.stack_size = 0;
    ps.num_inputs = 2;
    ps.input_semantics[0] = 0;
    ps.input_semantics[1] = 1;
    ps.num_color_exports = 1;

    Gx2PsRegs pr;
    if (!gx2_ps_regs(&pr, &ps)) return false;
    if (pr.sq_pgm_resources_ps != 0x00000002u) return false;
    if (pr.sq_pgm_exports_ps != 0x00000002u) return false;
    if (pr.spi_ps_in_control_0 != 0x14000002u) return false;
    if (pr.spi_ps_in_control_1 != 0x00000000u) return false;
    if (pr.num_spi_ps_input_cntl != 2u) return false;
    if (pr.spi_ps_input_cntls[0] != 0x00000100u) return false;
    if (pr.spi_ps_input_cntls[1] != 0x00000101u) return false;
    if (pr.cb_shader_mask != 0x0000000Fu) return false;
    if (pr.cb_shader_control != 0x00000001u) return false;
    if (pr.db_shader_control != 0x00000010u) return false;
    if (pr.spi_input_z != 0x00000000u) return false;

    return true;
}
