// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "mem/wii_si.h"

#include <string.h>

#define SI_CHANNELS      4u
#define SI_OUTBUF_STRIDE  0x0Cu
#define SI_INBUF_STRIDE   0x0Cu
#define SI_TCINT         0x80000000u
#define SI_START          0x00000001u
#define SI_TYPE_STANDARD  0x09000000u

typedef struct {
    uint32_t command[SI_CHANNELS];
    uint32_t response[SI_CHANNELS][2];
    uint32_t comcsr;
    uint32_t status;
    WiiSiStats stats;
} WiiSiState;

static WiiSiState s_si;

bool wii_si_is_mmio(uint32_t ea) {
    return ea >= WII_SI_BASE && ea < WII_SI_BASE + 0x100u;
}

void wii_si_reset(void) {
    memset(&s_si, 0, sizeof(s_si));
}

static void complete_transfer(uint32_t control) {
    uint32_t channel = (control >> 1) & 3u;
    if (channel >= SI_CHANNELS)
        return;

    uint8_t command = (uint8_t)(s_si.command[channel] >> 24);
    s_si.stats.transfers++;
    s_si.stats.last_control = control;
    if (command == 0x00u) {
        s_si.response[channel][0] = SI_TYPE_STANDARD;
        s_si.response[channel][1] = 0;
    } else {
        s_si.response[channel][0] = 0x00008080u;
        s_si.response[channel][1] = 0x80800000u;
    }
    s_si.status = 0;
    s_si.comcsr = (control & ~(SI_TCINT | SI_START)) | SI_TCINT;
}

void wii_si_write(uint32_t ea, uint32_t value, uint32_t size) {
    if (size != 4)
        return;

    if (ea >= WII_SI_OUTBUF0 && ea < WII_SI_OUTBUF0 + SI_CHANNELS * SI_OUTBUF_STRIDE) {
        uint32_t offset = ea - WII_SI_OUTBUF0;
        if (offset % SI_OUTBUF_STRIDE == 0) {
            s_si.command[offset / SI_OUTBUF_STRIDE] = value;
            s_si.stats.command_writes++;
            s_si.stats.last_command = value;
        }
        return;
    }
    if (ea == WII_SI_COMCSR) {
        if (value & SI_START)
            complete_transfer(value);
        else if (value & SI_TCINT)
            s_si.comcsr &= ~SI_TCINT;
    }
}

uint32_t wii_si_read(uint32_t ea, uint32_t size) {
    if (size != 4)
        return 0;
    if (ea == WII_SI_COMCSR)
        return s_si.comcsr;
    if (ea == WII_SI_STATUS) {
        s_si.stats.status_reads++;
        return s_si.status;
    }
    if (ea >= WII_SI_INBUF0_HI && ea < WII_SI_INBUF0_HI + SI_CHANNELS * SI_INBUF_STRIDE) {
        uint32_t offset = ea - WII_SI_INBUF0_HI;
        uint32_t channel = offset / SI_INBUF_STRIDE;
        offset %= SI_INBUF_STRIDE;
        if (offset == 0) {
            s_si.stats.response_reads++;
            return s_si.response[channel][0];
        }
        if (offset == 4) {
            s_si.stats.response_reads++;
            return s_si.response[channel][1];
        }
    }
    return 0;
}

bool wii_si_irq_pending(void) {
    return (s_si.comcsr & SI_TCINT) != 0;
}

WiiSiStats wii_si_stats(void) {
    return s_si.stats;
}

bool wii_si_selftest(void) {
    wii_si_reset();
    wii_si_write(WII_SI_OUTBUF0, 0, 4);
    wii_si_write(WII_SI_COMCSR, SI_START, 4);
    bool ok = wii_si_irq_pending() &&
              wii_si_read(WII_SI_INBUF0_HI, 4) == SI_TYPE_STANDARD &&
              wii_si_read(WII_SI_INBUF0_LO, 4) == 0;
    wii_si_write(WII_SI_COMCSR, SI_TCINT, 4);
    ok &= !wii_si_irq_pending();

    wii_si_write(WII_SI_OUTBUF0 + 2u * SI_OUTBUF_STRIDE, 0x41000000u, 4);
    wii_si_write(WII_SI_COMCSR, SI_START | (2u << 1), 4);
    ok &= wii_si_irq_pending() &&
          wii_si_read(WII_SI_INBUF0_HI + 2u * SI_INBUF_STRIDE, 4) == 0x00008080u &&
          wii_si_read(WII_SI_INBUF0_LO + 2u * SI_INBUF_STRIDE, 4) == 0x80800000u;
    wii_si_reset();
    return ok;
}
