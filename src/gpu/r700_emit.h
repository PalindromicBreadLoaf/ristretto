// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later
//
// R700/Latte GPU microcode encoder.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 64-bit instruction (CF, ALU)
typedef struct {
    uint32_t w0;
    uint32_t w1;
} R700Inst64;

// 128-bit instruction (TEX / VTX fetch)
typedef struct {
    uint32_t w[4];
} R700Inst128;

// CF instruction opcodes
enum {
    R700_CF_TEX        = 0x01,
    R700_CF_CALL_FS    = 0x13,
    R700_CF_ALU        = 0x08,  // encoded in the CF_ALU 4-bit inst field
    R700_CF_EXPORT     = 0x27,
    R700_CF_EXPORT_DONE = 0x28,
};

// Export destination types (CF_ALLOC_EXPORT TYPE field).
enum {
    R700_EXPORT_PIXEL = 0,
    R700_EXPORT_POS   = 1,
    R700_EXPORT_PARAM = 2,
};

// ALU OP2 opcodes
enum {
    R700_OP2_ADD      = 0x000,
    R700_OP2_MUL      = 0x001,
    R700_OP2_MUL_IEEE = 0x002,
    R700_OP2_MAX      = 0x003,
    R700_OP2_MIN      = 0x004,
    R700_OP2_MOV      = 0x019,
};

// ALU OP3 opcodes.
enum {
    R700_OP3_MULADD   = 0x10,
    // Conditional selects
    R700_OP3_CNDE     = 0x18,  // src0 == 0
    R700_OP3_CNDGT    = 0x19,  // src0 > 0
    R700_OP3_CNDGE    = 0x1A,  // src0 >= 0
};

// TEX instruction opcodes (TEX_INST field).
enum {
    R700_TEX_SAMPLE = 0x10,
};

// Special ALU source selects
enum {
    R700_ALU_SRC_LITERAL = 253,
    R700_ALU_SRC_PV      = 254,
    R700_ALU_SRC_PS      = 255,
    R700_ALU_SRC_0       = 248,
    R700_ALU_SRC_1       = 249,  // 1.0f
    R700_ALU_SRC_0_5     = 252,  // 0.5f
};

// A cfile (uniform register) constant is source-selected as 256 + register.
#define R700_ALU_SRC_CFILE(reg) (256u + (uint32_t)(reg))

// Channel / swizzle selects.
enum { R700_CHAN_X = 0, R700_CHAN_Y = 1, R700_CHAN_Z = 2, R700_CHAN_W = 3 };

// TEX coordinate source selects (SRC_SEL_*): the channel enums plus constants.
enum { R700_SEL_X = 0, R700_SEL_Y = 1, R700_SEL_Z = 2, R700_SEL_W = 3,
       R700_SEL_0 = 4, R700_SEL_1 = 5 };

// Instruction encoders

// Generic control-flow word (TEX/VTX clause start, CALL_FS, loops, etc.).
R700Inst64 r700_cf(uint32_t addr, uint32_t inst, uint32_t count,
                   bool valid_pixel_mode, bool end_of_program, bool barrier);

// ALU clause start
R700Inst64 r700_cf_alu(uint32_t addr, uint32_t slots, bool barrier);

// CF_ALLOC_EXPORT
R700Inst64 r700_cf_export(uint32_t type, uint32_t array_base, uint32_t src_gpr,
                          const uint8_t sel[4], bool end_of_program,
                          bool barrier, uint32_t inst);

// ALU OP2 (two-source) instruction.
R700Inst64 r700_alu_op2(uint32_t op, uint32_t dst_gpr, uint32_t dst_chan,
                        uint32_t s0_sel, uint32_t s0_chan, bool s0_neg,
                        uint32_t s1_sel, uint32_t s1_chan, bool s1_neg,
                        bool clamp, bool write_mask, bool last);

// ALU OP3 (three-source) instruction. OP3 forms always write the destination
// (there is no write_mask bit; the field is taken by the third source).
R700Inst64 r700_alu_op3(uint32_t op, uint32_t dst_gpr, uint32_t dst_chan,
                        uint32_t s0_sel, uint32_t s0_chan, bool s0_neg,
                        uint32_t s1_sel, uint32_t s1_chan, bool s1_neg,
                        uint32_t s2_sel, uint32_t s2_chan, bool s2_neg,
                        bool clamp, bool last);

// A literal-constant slot (two float bit patterns) placed after the ALU group
// that references R700_ALU_SRC_LITERAL. It carries no instruction fields.
R700Inst64 r700_alu_literal(uint32_t x_bits, uint32_t y_bits);

// TEX SAMPLE from src_gpr into dst_gpr, with a coordinate source swizzle and a
// destination swizzle. All coordinates are treated as normalized.
R700Inst128 r700_tex_sample(uint32_t resource_id, uint32_t sampler_id,
                            uint32_t src_gpr, uint32_t dst_gpr,
                            const uint8_t coord_sel[4], const uint8_t dst_sel[4]);

// Program assembler

#define R700_PROG_MAX_CF     64
#define R700_PROG_MAX_CLAUSE 64

typedef struct {
    uint8_t *buf;
    size_t   cap;

    R700Inst64 cf[R700_PROG_MAX_CF];
    uint32_t   cf_count;
    uint32_t   clause_count;

    uint32_t body_offset;  // running write cursor for clause bodies
    bool     overflow;
} R700Program;

// Reserve the CF section at the front of buf.
bool r700_prog_begin(R700Program *p, uint8_t *buf, size_t cap, uint32_t max_cf);

// Append a fully-formed CF word (CALL_FS, export, ...) that does not start a
// clause.
uint32_t r700_prog_cf(R700Program *p, R700Inst64 word);

// Append an ALU clause.
bool r700_prog_alu_clause(R700Program *p, const R700Inst64 *body, uint32_t slots,
                          bool barrier);

// Append a TEX clause.
bool r700_prog_tex_clause(R700Program *p, const R700Inst128 *body, uint32_t count,
                          bool valid_pixel_mode, bool barrier);

// Append only a clause body and return its CF ADDR.
uint32_t r700_prog_alu_body(R700Program *p, const R700Inst64 *body, uint32_t slots);
uint32_t r700_prog_tex_body(R700Program *p, const R700Inst128 *body, uint32_t count);

// Emit the CF section into buf and return the total program size in bytes.
size_t r700_prog_finalize(R700Program *p);

bool r700_emit_selftest(void);
