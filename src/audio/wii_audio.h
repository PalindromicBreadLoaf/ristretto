// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef RISTRETTO_AUDIO_WII_AUDIO_H
#define RISTRETTO_AUDIO_WII_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

// Wii AI and DSP audio-DMA register model.

#define WII_AI_BASE                 0xCC006C00u
#define WII_AI_CONTROL              (WII_AI_BASE + 0x00u)
#define WII_AI_VOLUME               (WII_AI_BASE + 0x04u)
#define WII_AI_SAMPLE_COUNTER       (WII_AI_BASE + 0x08u)
#define WII_AI_INTERRUPT_TIMING     (WII_AI_BASE + 0x0Cu)

#define WII_DSP_BASE                0xCC005000u
#define WII_DSP_CONTROL             (WII_DSP_BASE + 0x0Au)
#define WII_DSP_AUDIO_DMA_START_HI  (WII_DSP_BASE + 0x30u)
#define WII_DSP_AUDIO_DMA_START_LO  (WII_DSP_BASE + 0x32u)
#define WII_DSP_AUDIO_DMA_BLOCKS    (WII_DSP_BASE + 0x34u)
#define WII_DSP_AUDIO_DMA_CONTROL   (WII_DSP_BASE + 0x36u)
#define WII_DSP_AUDIO_DMA_REMAINING (WII_DSP_BASE + 0x3Au)

typedef struct {
    uint64_t dma_blocks;
    uint64_t dma_bytes;
    uint32_t sample_counter;
    int16_t  last_left;
    int16_t  last_right;
} WiiAudioStats;

bool wii_audio_is_mmio(uint32_t ea);
void wii_audio_reset(void);

// Set up the Cafe AX output voice after guest memory and the application's process
// services are ready.
bool wii_audio_init(void);
void wii_audio_shutdown(void);

void     wii_audio_write(uint32_t ea, uint32_t value, uint32_t size);
uint32_t wii_audio_read(uint32_t ea, uint32_t size);

// Advance the devices from wall-clock time.
void wii_audio_tick(void);
// Advance by an exact number of 32 kHz frames.
void wii_audio_advance(uint32_t frames);

WiiAudioStats wii_audio_stats(void);
bool wii_audio_selftest(void);

#endif  // RISTRETTO_AUDIO_WII_AUDIO_H
