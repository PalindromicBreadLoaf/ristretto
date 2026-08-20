// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef RISTRETTO_MEM_WII_MEMORY_H
#define RISTRETTO_MEM_WII_MEMORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Model of the Wii's address space inside a Cafe OS app.

// MEM1: 24 MiB, physical 0x00000000. MEM2: 64 MiB, physical 0x10000000.
#define WII_MEM1_EA_CACHED    0x80000000u
#define WII_MEM1_EA_UNCACHED  0xC0000000u
#define WII_MEM1_PHYS         0x00000000u
#define WII_MEM1_SIZE         0x01800000u

#define WII_MEM2_EA_CACHED    0x90000000u
#define WII_MEM2_EA_UNCACHED  0xD0000000u
#define WII_MEM2_PHYS         0x10000000u
#define WII_MEM2_SIZE         0x04000000u

// Masking a guest EA with this drops the cached/uncached/physical alias bits and
// leaves the bank's physical offset (MEM1 at 0 MEM2 at 0x10000000).
#define WII_FASTMEM_MASK      0x1FFFFFFFu
#define WII_FASTMEM_WINDOW_SIZE (WII_MEM2_PHYS + WII_MEM2_SIZE)

bool wii_mem_init(void);
void wii_mem_shutdown(void);

// Host base of the single contiguous window that backs both banks.
void *wii_mem_fastmem_window(void);

// Translate a guest effective address to a host pointer.
void *wii_mem_ptr(uint32_t ea);
void *wii_mem_range(uint32_t ea, uint32_t size);

uint8_t  wii_read_u8(uint32_t ea);
uint16_t wii_read_u16(uint32_t ea);
uint32_t wii_read_u32(uint32_t ea);
uint64_t wii_read_u64(uint32_t ea);

void wii_write_u8(uint32_t ea, uint8_t value);
void wii_write_u16(uint32_t ea, uint16_t value);
void wii_write_u32(uint32_t ea, uint32_t value);
void wii_write_u64(uint32_t ea, uint64_t value);

void wii_mem_write(uint32_t ea, const void *src, uint32_t size);
void wii_mem_read(void *dst, uint32_t ea, uint32_t size);

// Populate the low-MEM1 globals a Wii game/apploader reads at boot.
void wii_mem_setup_lowmem(void);

void wii_mem_log_layout(void);

#endif  // RISTRETTO_MEM_WII_MEMORY_H
