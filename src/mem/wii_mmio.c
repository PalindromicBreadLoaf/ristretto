// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "mem/wii_mmio.h"

#include "ios/ios_ipc.h"
#include "mem/wii_vi.h"

#include <string.h>

// Captured write-gather stream.
#define WGP_CAP 0x10000u
static uint8_t  s_wgp[WGP_CAP];
static uint32_t s_wgp_len;

// Latched Hollywood IPC message
static uint32_t s_ipc_msg;

void wii_mmio_reset(void) {
    s_wgp_len = 0;
    s_ipc_msg = 0;
    wii_vi_reset();
}

static void wgp_append(uint64_t value, uint32_t size) {
    for (uint32_t i = 0; i < size; ++i) {
        if (s_wgp_len >= WGP_CAP)
            return;
        uint32_t shift = (size - 1 - i) * 8;
        s_wgp[s_wgp_len++] = (uint8_t)(value >> shift);
    }
}

void wii_mmio_write(uint32_t ea, uint64_t value, uint32_t size) {
    if (wii_ea_is_wgp(ea)) {
        wgp_append(value, size);
        return;
    }
    if (wii_ea_is_vi(ea)) {
        wii_vi_write(ea - WII_VI_BASE, (uint32_t)value, size);
        return;
    }
    switch (ea) {
    case WII_MMIO_IPC_PPCMSG:
        s_ipc_msg = (uint32_t)value;
        return;
    case WII_MMIO_IPC_PPCCTRL:
        if (value & WII_MMIO_IPC_CTRL_X1)
            ios_ipc_dispatch(s_ipc_msg);
        return;
    default:
        return;   // unmodelled register
    }
}

uint32_t wii_mmio_read(uint32_t ea, uint32_t size) {
    if (wii_ea_is_vi(ea))
        return wii_vi_read(ea - WII_VI_BASE, size);
    if (ea == WII_MMIO_IPC_PPCMSG)
        return s_ipc_msg;
    return 0;
}

const uint8_t *wii_mmio_wgp_data(uint32_t *len) {
    if (len)
        *len = s_wgp_len;
    return s_wgp;
}
