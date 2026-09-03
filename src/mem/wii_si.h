// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef RISTRETTO_MEM_WII_SI_H
#define RISTRETTO_MEM_WII_SI_H

#include <stdbool.h>
#include <stdint.h>

#define WII_SI_BASE       0xCD006400u
#define WII_SI_OUTBUF0    (WII_SI_BASE + 0x00u)
#define WII_SI_COMCSR     (WII_SI_BASE + 0x34u)
#define WII_SI_STATUS     (WII_SI_BASE + 0x38u)
#define WII_SI_INBUF0_HI  (WII_SI_BASE + 0x04u)
#define WII_SI_INBUF0_LO  (WII_SI_BASE + 0x08u)

typedef struct {
    uint32_t command_writes;
    uint32_t response_reads;
    uint32_t status_reads;
    uint32_t transfers;
    uint32_t last_command;
    uint32_t last_control;
} WiiSiStats;

bool wii_si_is_mmio(uint32_t ea);
void wii_si_reset(void);

void     wii_si_write(uint32_t ea, uint32_t value, uint32_t size);
uint32_t wii_si_read(uint32_t ea, uint32_t size);
bool     wii_si_irq_pending(void);
WiiSiStats wii_si_stats(void);

bool wii_si_selftest(void);

#endif  // RISTRETTO_MEM_WII_SI_H
