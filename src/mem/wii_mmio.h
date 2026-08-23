// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef RISTRETTO_MEM_WII_MMIO_H
#define RISTRETTO_MEM_WII_MMIO_H

#include <stdbool.h>
#include <stdint.h>

// Wii memory-mapped hardware register routing.

// 0xCC.. is the GP/PE/PI + write-gather region
// 0xCD.. is the Hollywood IPC block.
#define WII_MMIO_HW_BASE   0xCC000000u
#define WII_MMIO_IPC_BASE  0xCD000000u

// Write gather pipe: every write lands at this one address and streams into the FIFO.
#define WII_MMIO_WGP_EA    0xCC008000u

// Hollywood IPC registers the guest pokes to signal Starlet.
#define WII_MMIO_IPC_PPCMSG   0xCD000000u
#define WII_MMIO_IPC_PPCCTRL  0xCD000004u
#define WII_MMIO_IPC_CTRL_X1  0x00000001u   // IOS request bit

typedef void (*WiiWgpSink)(void *user, const uint8_t *bytes, uint32_t len);

typedef struct {
    uint64_t bytes_written;
    uint32_t bytes_captured;
    uint32_t bytes_dropped;
} WiiWgpStats;

static inline bool wii_ea_is_mmio(uint32_t ea) {
    uint32_t top = ea & 0xFF000000u;
    return top == WII_MMIO_HW_BASE || top == WII_MMIO_IPC_BASE;
}

static inline bool wii_ea_is_wgp(uint32_t ea) {
    return (ea & 0xFFFFFF00u) == (WII_MMIO_WGP_EA & 0xFFFFFF00u);
}

void wii_mmio_reset(void);

void     wii_mmio_write(uint32_t ea, uint64_t value, uint32_t size);
uint32_t wii_mmio_read(uint32_t ea, uint32_t size);

// Receive every write-gather byte sequence synchronously.
void wii_mmio_set_wgp_sink(WiiWgpSink sink, void *user);

// The diagnostic write-gather prefix captured since the last reset.
const uint8_t *wii_mmio_wgp_data(uint32_t *len);
WiiWgpStats wii_mmio_wgp_stats(void);

bool wii_mmio_selftest(void);

#endif  // RISTRETTO_MEM_WII_MMIO_H
