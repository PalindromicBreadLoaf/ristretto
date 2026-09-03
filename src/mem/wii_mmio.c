// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "mem/wii_mmio.h"

#include "audio/wii_audio.h"
#include "ios/ios_ipc.h"
#include "mem/wii_si.h"
#include "mem/wii_vi.h"

#include <string.h>

// Captured write-gather stream.
#define WGP_CAP 0x10000u
static uint8_t  s_wgp[WGP_CAP];
static uint32_t s_wgp_len;
static uint64_t s_wgp_bytes_written;
static uint32_t s_wgp_bytes_dropped;
static WiiWgpSink s_wgp_sink;
static void *s_wgp_sink_user;

// Latched Hollywood IPC mailbox state.
static uint32_t s_ipc_msg;
static uint32_t s_ipc_arm_msg;
static uint32_t s_ipc_status;
static uint32_t s_ipc_interrupt_enable;
static WiiIpcStats s_ipc_stats;
static uint32_t s_pi_int_mask;

static uint32_t pi_int_cause(void) {
    uint32_t cause = 0;
    if (wii_si_irq_pending()) cause |= WII_MMIO_PI_SI;
    if (wii_vi_irq_pending()) cause |= WII_MMIO_PI_VI;
    if ((s_ipc_status & WII_MMIO_IPC_CTRL_Y2) &&
        (s_ipc_interrupt_enable & WII_MMIO_IPC_CTRL_IY2))
        cause |= WII_MMIO_PI_IPC;
    return cause;
}

void wii_mmio_reset(void) {
    s_wgp_len = 0;
    s_wgp_bytes_written = 0;
    s_wgp_bytes_dropped = 0;
    s_ipc_msg = 0;
    s_ipc_arm_msg = 0;
    s_ipc_status = 0;
    s_ipc_interrupt_enable = 0;
    memset(&s_ipc_stats, 0, sizeof s_ipc_stats);
    s_pi_int_mask = 0;
    wii_audio_reset();
    wii_si_reset();
    wii_vi_reset();
}

static void wgp_append(uint64_t value, uint32_t size) {
    uint8_t bytes[sizeof(value)];
    if (size > sizeof(bytes)) {
        s_wgp_bytes_dropped += size;
        return;
    }

    for (uint32_t i = 0; i < size; ++i) {
        uint32_t shift = (size - 1 - i) * 8;
        bytes[i] = (uint8_t)(value >> shift);
        s_wgp_bytes_written++;
        if (s_wgp_len >= WGP_CAP)
            s_wgp_bytes_dropped++;
        else
            s_wgp[s_wgp_len++] = bytes[i];
    }

    if (s_wgp_sink)
        s_wgp_sink(s_wgp_sink_user, bytes, size);
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
    if (wii_audio_is_mmio(ea)) {
        wii_audio_write(ea, (uint32_t)value, size);
        return;
    }
    if (wii_si_is_mmio(ea)) {
        wii_si_write(ea, (uint32_t)value, size);
        return;
    }
    switch (ea) {
    case WII_MMIO_PI_INTMR:
        s_pi_int_mask = (uint32_t)value;
        return;
    case WII_MMIO_IPC_PPCMSG:
        s_ipc_msg = (uint32_t)value;
        return;
    case WII_MMIO_IPC_PPCCTRL:
        s_ipc_interrupt_enable = (uint32_t)value &
                                 (WII_MMIO_IPC_CTRL_IY1 | WII_MMIO_IPC_CTRL_IY2);
        if ((value & WII_MMIO_IPC_CTRL_Y2) && s_ipc_arm_msg != 0) {
            s_ipc_arm_msg = 0;
            s_ipc_status &= ~WII_MMIO_IPC_CTRL_Y2;
            s_ipc_stats.acknowledgements++;
        }
        if (value & WII_MMIO_IPC_CTRL_Y1)
            s_ipc_status &= ~WII_MMIO_IPC_CTRL_Y1;
        if (value & WII_MMIO_IPC_CTRL_X1) {
            ios_ipc_dispatch(s_ipc_msg);
            s_ipc_arm_msg = s_ipc_msg;
            s_ipc_status |= WII_MMIO_IPC_CTRL_Y1 | WII_MMIO_IPC_CTRL_Y2;
            s_ipc_stats.requests++;
            s_ipc_stats.replies++;
        }
        return;
    default:
        return;   // unmodelled register
    }
}

uint32_t wii_mmio_read(uint32_t ea, uint32_t size) {
    if (wii_ea_is_vi(ea))
        return wii_vi_read(ea - WII_VI_BASE, size);
    if (wii_audio_is_mmio(ea))
        return wii_audio_read(ea, size);
    if (wii_si_is_mmio(ea))
        return wii_si_read(ea, size);
    if (ea == WII_MMIO_PI_INTSR)
        return pi_int_cause();
    if (ea == WII_MMIO_PI_INTMR)
        return s_pi_int_mask;
    if (ea == WII_MMIO_IPC_PPCMSG)
        return s_ipc_msg;
    if (ea == WII_MMIO_IPC_PPCCTRL)
        return s_ipc_status | s_ipc_interrupt_enable;
    if (ea == WII_MMIO_IPC_ARMMSG)
        return s_ipc_arm_msg;
    return 0;
}

bool wii_mmio_irq_pending(void) {
    return (pi_int_cause() & s_pi_int_mask) != 0;
}

void wii_mmio_set_wgp_sink(WiiWgpSink sink, void *user) {
    s_wgp_sink = sink;
    s_wgp_sink_user = user;
}

const uint8_t *wii_mmio_wgp_data(uint32_t *len) {
    if (len)
        *len = s_wgp_len;
    return s_wgp;
}

WiiWgpStats wii_mmio_wgp_stats(void) {
    return (WiiWgpStats){
        .bytes_written = s_wgp_bytes_written,
        .bytes_captured = s_wgp_len,
        .bytes_dropped = s_wgp_bytes_dropped,
    };
}

WiiIpcStats wii_mmio_ipc_stats(void) {
    return s_ipc_stats;
}

typedef struct {
    uint8_t bytes[8];
    uint32_t len;
} WgpTestSink;

static void wgp_test_sink(void *user, const uint8_t *bytes, uint32_t len) {
    WgpTestSink *sink = user;
    if (sink->len + len > sizeof(sink->bytes))
        return;
    memcpy(sink->bytes + sink->len, bytes, len);
    sink->len += len;
}

bool wii_mmio_selftest(void) {
    WgpTestSink sink = {0};
    wii_mmio_set_wgp_sink(wgp_test_sink, &sink);
    wii_mmio_reset();
    wii_mmio_write(WII_MMIO_WGP_EA, 0x11223344u, 4);
    wii_mmio_write(WII_MMIO_WGP_EA, 0xAABBu, 2);

    uint32_t captured = 0;
    const uint8_t *data = wii_mmio_wgp_data(&captured);
    WiiWgpStats stats = wii_mmio_wgp_stats();
    bool ok = sink.len == 6 && captured == 6 && stats.bytes_written == 6 &&
              stats.bytes_captured == 6 && stats.bytes_dropped == 0 &&
              memcmp(data, "\x11\x22\x33\x44\xAA\xBB", 6) == 0 &&
              memcmp(sink.bytes, data, 6) == 0;

    wii_mmio_set_wgp_sink(NULL, NULL);
    wii_mmio_reset();

    uint32_t di0 = (1u << 28) | (240u << 16);
    wii_mmio_write(WII_VI_BASE + WII_VI_DISPLAY_INT_0, di0, 4);
    wii_mmio_write(WII_MMIO_PI_INTMR, WII_MMIO_PI_VI, 4);
    wii_vi_tick_vblank();
    if (!(wii_mmio_read(WII_MMIO_PI_INTSR, 4) & WII_MMIO_PI_VI) ||
        !wii_mmio_irq_pending()) {
        wii_mmio_reset();
        return false;
    }
    wii_mmio_reset();

    wii_mmio_write(WII_MMIO_IPC_PPCCTRL,
                   WII_MMIO_IPC_CTRL_IY1 | WII_MMIO_IPC_CTRL_IY2, 4);
    wii_mmio_write(WII_MMIO_PI_INTMR, WII_MMIO_PI_IPC, 4);
    wii_mmio_write(WII_MMIO_IPC_PPCMSG, 0x90000000u, 4);
    wii_mmio_write(WII_MMIO_IPC_PPCCTRL,
                   WII_MMIO_IPC_CTRL_IY1 | WII_MMIO_IPC_CTRL_IY2 |
                   WII_MMIO_IPC_CTRL_X1, 4);
    WiiIpcStats ipc = wii_mmio_ipc_stats();
    if (wii_mmio_read(WII_MMIO_IPC_ARMMSG, 4) != 0x90000000u ||
        !(wii_mmio_read(WII_MMIO_PI_INTSR, 4) & WII_MMIO_PI_IPC) ||
        !wii_mmio_irq_pending() || ipc.requests != 1 || ipc.replies != 1) {
        wii_mmio_reset();
        return false;
    }
    wii_mmio_write(WII_MMIO_IPC_PPCCTRL,
                   WII_MMIO_IPC_CTRL_IY1 | WII_MMIO_IPC_CTRL_IY2 |
                   WII_MMIO_IPC_CTRL_Y2, 4);
    ipc = wii_mmio_ipc_stats();
    if (wii_mmio_irq_pending() || ipc.acknowledgements != 1 ||
        !(wii_mmio_read(WII_MMIO_IPC_PPCCTRL, 4) & WII_MMIO_IPC_CTRL_Y1)) {
        wii_mmio_reset();
        return false;
    }
    wii_mmio_write(WII_MMIO_IPC_PPCCTRL,
                   WII_MMIO_IPC_CTRL_IY1 | WII_MMIO_IPC_CTRL_IY2 |
                   WII_MMIO_IPC_CTRL_Y1, 4);
    if (wii_mmio_read(WII_MMIO_IPC_PPCCTRL, 4) & WII_MMIO_IPC_CTRL_Y1) {
        wii_mmio_reset();
        return false;
    }
    wii_mmio_reset();
    return ok;
}
