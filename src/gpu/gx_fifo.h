// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

// GX command stream decoder.

#ifndef RISTRETTO_GPU_GX_FIFO_H
#define RISTRETTO_GPU_GX_FIFO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GX_OPCODE_NOP          = 0x00,
    GX_OPCODE_LOAD_CP_REG  = 0x08,
    GX_OPCODE_LOAD_XF_REG  = 0x10,
    GX_OPCODE_LOAD_INDX_A  = 0x20,  // position matrices
    GX_OPCODE_LOAD_INDX_B  = 0x28,  // normal matrices
    GX_OPCODE_LOAD_INDX_C  = 0x30,  // post/tex matrices
    GX_OPCODE_LOAD_INDX_D  = 0x38,  // lights
    GX_OPCODE_CALL_DL      = 0x40,
    GX_OPCODE_INVL_VC      = 0x48,  // invalidate vertex cache
    GX_OPCODE_LOAD_BP_REG  = 0x61,
    GX_OPCODE_PRIM_START   = 0x80,
    GX_OPCODE_PRIM_END     = 0xBF,
} GXOpcode;

// Extracted from the primitive opcode via (op & 0x78) >> 3.
typedef enum {
    GX_PRIM_QUADS          = 0x0,  // 0x80
    GX_PRIM_QUADS_2        = 0x1,  // 0x88
    GX_PRIM_TRIANGLES      = 0x2,  // 0x90
    GX_PRIM_TRIANGLE_STRIP = 0x3,  // 0x98
    GX_PRIM_TRIANGLE_FAN   = 0x4,  // 0xA0
    GX_PRIM_LINES          = 0x5,  // 0xA8
    GX_PRIM_LINE_STRIP     = 0x6,  // 0xB0
    GX_PRIM_POINTS         = 0x7,  // 0xB8
} GXPrimitive;

// CP register sub-command groups.
enum {
    GX_CP_MATINDEX_A = 0x30,
    GX_CP_MATINDEX_B = 0x40,
    GX_CP_VCD_LO     = 0x50,
    GX_CP_VCD_HI     = 0x60,
    GX_CP_VAT_A      = 0x70,
    GX_CP_VAT_B      = 0x80,
    GX_CP_VAT_C      = 0x90,
    GX_CP_ARRAY_BASE = 0xA0,
    GX_CP_ARRAY_STRIDE = 0xB0,
    GX_CP_COMMAND_MASK = 0xF0,
    GX_CP_INDEX_MASK   = 0x07,
};

// The slice of CP state a decoder needs to size draw calls.
typedef struct {
    uint32_t desc_lo;
    uint32_t desc_hi;
    struct {
        uint32_t g0;
        uint32_t g1;
        uint32_t g2;
    } vat[8];
} GXFifoState;

// Handlers for each decoded command.
typedef struct {
    void (*on_nop)(void *user, uint32_t count);
    void (*on_cp)(void *user, uint8_t sub_command, uint32_t value);
    void (*on_xf)(void *user, uint16_t address, uint8_t count, const uint8_t *data);
    void (*on_bp)(void *user, uint8_t command, uint32_t value);
    void (*on_indexed)(void *user, uint8_t array, uint32_t index, uint16_t address, uint8_t size);
    void (*on_primitive)(void *user, GXPrimitive primitive, uint8_t vat,
                         uint32_t vertex_size, uint16_t num_vertices, const uint8_t *vertex_data);
    void (*on_display_list)(void *user, uint32_t addr, uint32_t size);
    void (*on_unknown)(void *user, uint8_t opcode);
    const uint8_t *(*resolve_dl)(void *user, uint32_t addr, uint32_t size);
} GXFifoSink;

// Reset CP state.
void gx_fifo_state_reset(GXFifoState *state);

// Apply a CP register load to the state.
void gx_fifo_apply_cp(GXFifoState *state, uint8_t sub_command, uint32_t value);

// Size in bytes of one vertex for the given VAT under the current descriptor.
uint32_t gx_fifo_vertex_size(const GXFifoState *state, uint8_t vat);

// Walk `avail` bytes of command stream, updating `state` and invoking `sink`.
size_t gx_fifo_run(GXFifoState *state, const uint8_t *data, size_t avail,
                   const GXFifoSink *sink, void *user);

// Build a synthetic FIFO with a known vertex format and verifies the
// walk consumes exactly the right bytes and classifies every command.
int gx_fifo_selftest(void);

#ifdef __cplusplus
}
#endif

#endif  // RISTRETTO_GPU_GX_FIFO_H
