// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "audio/wii_audio.h"

#include "mem/wii_memory.h"

#include <coreinit/cache.h>
#include <coreinit/time.h>
#include <sndcore2/core.h>
#include <sndcore2/voice.h>

#include <string.h>

#define WII_AUDIO_RATE             32000u
#define WII_AUDIO_DMA_BLOCK_BYTES  32u
#define WII_AUDIO_DMA_BLOCK_FRAMES 8u
#define WII_AUDIO_RING_FRAMES      8192u
#define WII_AUDIO_RING_SAMPLES     (WII_AUDIO_RING_FRAMES * 2u)

#define AI_CTRL_PSTAT      (1u << 0)
#define AI_CTRL_AIINTMSK   (1u << 2)
#define AI_CTRL_AIINT      (1u << 3)
#define AI_CTRL_AIINTVLD   (1u << 4)
#define AI_CTRL_SCRESET    (1u << 5)

#define DSP_CTRL_AID       (1u << 3)
#define DSP_CTRL_AID_MASK  (1u << 4)
#define DSP_CTRL_ARAM      (1u << 5)

typedef struct {
    uint32_t ai_control;
    uint32_t ai_volume;
    uint32_t ai_counter;
    uint32_t ai_interrupt_timing;
    uint16_t dsp_control;
    uint16_t dsp_mailbox_hi;
    uint16_t dsp_mailbox_lo;
    uint32_t dma_config_source;
    uint32_t dma_source;
    uint16_t dma_blocks;
    uint16_t dma_remaining;
    uint16_t dma_control;
    uint64_t dma_bytes;
    uint64_t dma_block_count;
    int16_t last_left;
    int16_t last_right;
    uint32_t ring_write;
    uint64_t last_tick;
    uint64_t tick_remainder;
    AXVoice *voice;
    bool ax_owned;
} WiiAudioState;

static WiiAudioState s_audio;
static int16_t s_ring[WII_AUDIO_RING_SAMPLES] __attribute__((aligned(64)));

static uint32_t mask_value(uint32_t value, uint32_t size) {
    if (size == 1) return value & 0xFFu;
    if (size == 2) return value & 0xFFFFu;
    return value;
}

bool wii_audio_is_mmio(uint32_t ea) {
    return ea >= WII_DSP_BASE && ea < WII_DSP_BASE + 0x1000u;
}

void wii_audio_reset(void) {
    AXVoice *voice = s_audio.voice;
    bool ax_owned = s_audio.ax_owned;
    memset(&s_audio, 0, sizeof(s_audio));
    s_audio.voice = voice;
    s_audio.ax_owned = ax_owned;
    s_audio.dsp_control = 1u << 2;  // DSP starts halted until guest initialization.
    s_audio.ring_write = WII_AUDIO_RING_FRAMES / 4u;
    s_audio.last_tick = (uint64_t)OSGetTime();
    memset(s_ring, 0, sizeof(s_ring));
    DCFlushRange(s_ring, sizeof(s_ring));
}

static void configure_host_voice(void) {
    if (!s_audio.voice) return;

    AXVoiceOffsets offsets = {
        .dataType = AX_VOICE_FORMAT_LPCM16,
        .loopingEnabled = AX_VOICE_LOOP_ENABLED,
        .loopOffset = 0,
        .endOffset = WII_AUDIO_RING_SAMPLES - 1u,
        .currentOffset = 0,
        .data = s_ring,
    };
    AXVoiceDeviceMixData mix = {0};
    for (uint32_t bus = 0; bus < 4; ++bus)
        mix.bus[bus].volume = 0x8000u;

    AXVoiceBegin(s_audio.voice);
    AXSetVoiceOffsets(s_audio.voice, &offsets);
    AXSetVoiceSrcType(s_audio.voice, AX_VOICE_SRC_TYPE_NONE);
    AXSetVoiceDeviceMix(s_audio.voice, AX_DEVICE_TYPE_TV, 0, &mix);
    AXSetVoiceDeviceMix(s_audio.voice, AX_DEVICE_TYPE_DRC, 0, &mix);
    AXSetVoiceState(s_audio.voice, AX_VOICE_STATE_PLAYING);
    AXVoiceEnd(s_audio.voice);
}

bool wii_audio_init(void) {
    wii_audio_reset();
    if (!AXIsInit()) {
        AXInitParams params = {
            .renderer = AX_INIT_RENDERER_32KHZ,
            .pipeline = AX_INIT_PIPELINE_SINGLE,
        };
        AXInitWithParams(&params);
        s_audio.ax_owned = true;
    }
    s_audio.voice = AXAcquireVoice(31, NULL, NULL);
    if (!s_audio.voice) return false;
    configure_host_voice();
    return true;
}

void wii_audio_shutdown(void) {
    if (s_audio.voice) {
        AXSetVoiceState(s_audio.voice, AX_VOICE_STATE_STOPPED);
        AXFreeVoice(s_audio.voice);
        s_audio.voice = NULL;
    }
    if (s_audio.ax_owned) {
        AXQuit();
        s_audio.ax_owned = false;
    }
}

static bool dma_enabled(void) {
    return (s_audio.dma_control & 0x8000u) != 0;
}

static void raise_dma_interrupt(void) {
    s_audio.dsp_control |= DSP_CTRL_AID;
}

static void write_frame(int16_t left, int16_t right) {
    uint32_t index = s_audio.ring_write * 2u;
    s_ring[index] = left;
    s_ring[index + 1u] = right;
    s_audio.ring_write = (s_audio.ring_write + 1u) % WII_AUDIO_RING_FRAMES;
    s_audio.last_left = left;
    s_audio.last_right = right;
}

static void transfer_dma_block(void) {
    const uint8_t *src = wii_mem_range(s_audio.dma_source, WII_AUDIO_DMA_BLOCK_BYTES);
    for (uint32_t frame = 0; frame < WII_AUDIO_DMA_BLOCK_FRAMES; ++frame) {
        int16_t left = 0;
        int16_t right = 0;
        if (src) {
            uint32_t i = frame * 4u;
            left = (int16_t)((uint16_t)src[i] << 8 | src[i + 1u]);
            right = (int16_t)((uint16_t)src[i + 2u] << 8 | src[i + 3u]);
        }
        write_frame(left, right);
    }
    s_audio.dma_source += WII_AUDIO_DMA_BLOCK_BYTES;
    s_audio.dma_bytes += WII_AUDIO_DMA_BLOCK_BYTES;
    s_audio.dma_block_count++;
}

static void advance_dma(uint32_t frames) {
    while (frames >= WII_AUDIO_DMA_BLOCK_FRAMES) {
        if (dma_enabled()) {
            transfer_dma_block();
            if (s_audio.dma_remaining > 0) --s_audio.dma_remaining;
            if (s_audio.dma_remaining == 0) {
                s_audio.dma_source = s_audio.dma_config_source;
                s_audio.dma_remaining = s_audio.dma_blocks;
                raise_dma_interrupt();
            }
        } else {
            for (uint32_t i = 0; i < WII_AUDIO_DMA_BLOCK_FRAMES; ++i)
                write_frame(0, 0);
        }
        frames -= WII_AUDIO_DMA_BLOCK_FRAMES;
    }

    while (frames-- != 0)
        write_frame(0, 0);
}

static void advance_ai_counter(uint32_t frames) {
    if (!(s_audio.ai_control & AI_CTRL_PSTAT)) return;
    uint32_t before = s_audio.ai_counter;
    s_audio.ai_counter += frames;
    if ((s_audio.ai_interrupt_timing - before) <= frames)
        s_audio.ai_control |= AI_CTRL_AIINT;
}

void wii_audio_advance(uint32_t frames) {
    if (frames == 0) return;
    advance_dma(frames);
    advance_ai_counter(frames);
    if (s_audio.voice) DCFlushRange(s_ring, sizeof(s_ring));
}

void wii_audio_tick(void) {
    uint64_t now = (uint64_t)OSGetTime();
    uint64_t elapsed = now - s_audio.last_tick;
    s_audio.last_tick = now;
    uint64_t scaled = elapsed * WII_AUDIO_RATE + s_audio.tick_remainder;
    uint64_t ticks_per_second = OSSecondsToTicks(1);
    uint32_t frames = (uint32_t)(scaled / ticks_per_second);
    s_audio.tick_remainder = scaled % ticks_per_second;
    wii_audio_advance(frames);
}

static void write16(uint32_t ea, uint16_t value) {
    switch (ea) {
    case WII_DSP_CONTROL:
        if (value & 1u) s_audio.dma_control = 0;
        if (value & DSP_CTRL_AID) s_audio.dsp_control &= ~DSP_CTRL_AID;
        s_audio.dsp_control = (s_audio.dsp_control & ~(DSP_CTRL_AID_MASK | (1u << 2))) |
                              (value & (DSP_CTRL_AID_MASK | (1u << 2)));
        return;
    case WII_DSP_ARAM_DMA_CNT_LO:
        s_audio.dsp_control |= DSP_CTRL_ARAM;
        s_audio.dsp_mailbox_hi = 0x8000u;
        s_audio.dsp_mailbox_lo = 0;
        return;
    case WII_DSP_AUDIO_DMA_START_HI:
        s_audio.dma_config_source = (s_audio.dma_config_source & 0x0000FFFFu) |
                                    ((uint32_t)(value & 0x1FFFu) << 16);
        return;
    case WII_DSP_AUDIO_DMA_START_LO:
        s_audio.dma_config_source = (s_audio.dma_config_source & 0x1FFF0000u) |
                                    (value & 0xFFE0u);
        return;
    case WII_DSP_AUDIO_DMA_BLOCKS:
        return;
    case WII_DSP_AUDIO_DMA_CONTROL: {
        bool was_enabled = dma_enabled();
        s_audio.dma_control = value;
        s_audio.dma_blocks = value & 0x7FFFu;
        if (!was_enabled && dma_enabled()) {
            s_audio.dma_source = s_audio.dma_config_source;
            s_audio.dma_remaining = s_audio.dma_blocks;
            raise_dma_interrupt();
        }
        return;
    }
    default:
        return;
    }
}

static void write32(uint32_t ea, uint32_t value) {
    switch (ea) {
    case WII_AI_CONTROL:
        if (value & AI_CTRL_AIINT) s_audio.ai_control &= ~AI_CTRL_AIINT;
        s_audio.ai_control = (s_audio.ai_control & AI_CTRL_AIINT) |
                             (value & (AI_CTRL_PSTAT | (1u << 1) | AI_CTRL_AIINTMSK |
                                       AI_CTRL_AIINTVLD | (1u << 6)));
        if (value & AI_CTRL_SCRESET) s_audio.ai_counter = 0;
        return;
    case WII_AI_VOLUME:
        s_audio.ai_volume = value;
        return;
    case WII_AI_SAMPLE_COUNTER:
        s_audio.ai_counter = value;
        return;
    case WII_AI_INTERRUPT_TIMING:
        s_audio.ai_interrupt_timing = value;
        return;
    default:
        return;
    }
}

void wii_audio_write(uint32_t ea, uint32_t value, uint32_t size) {
    wii_audio_tick();
    value = mask_value(value, size);
    if (size == 4 && ea >= WII_DSP_BASE && ea < WII_DSP_BASE + 0x1000u) {
        write16(ea, (uint16_t)(value >> 16));
        write16(ea + 2u, (uint16_t)value);
    } else if (size == 2) {
        write16(ea, (uint16_t)value);
    } else if (size == 4) {
        write32(ea, value);
    }
}

static uint16_t read16(uint32_t ea) {
    uint32_t value = 0;
    switch (ea) {
    case WII_DSP_MAILBOX_HI: value = s_audio.dsp_mailbox_hi; break;
    case WII_DSP_MAILBOX_LO: value = s_audio.dsp_mailbox_lo; break;
    case WII_DSP_CPU_MAILBOX_HI: value = s_audio.dsp_mailbox_hi; break;
    case WII_DSP_CPU_MAILBOX_LO: value = s_audio.dsp_mailbox_lo; break;
    case WII_DSP_CONTROL: value = s_audio.dsp_control; break;
    case WII_DSP_AUDIO_DMA_START_HI: value = s_audio.dma_config_source >> 16; break;
    case WII_DSP_AUDIO_DMA_START_LO: value = s_audio.dma_config_source & 0xFFFFu; break;
    case WII_DSP_AUDIO_DMA_BLOCKS: value = s_audio.dma_blocks; break;
    case WII_DSP_AUDIO_DMA_CONTROL: value = s_audio.dma_control; break;
    case WII_DSP_AUDIO_DMA_REMAINING:
        value = s_audio.dma_remaining ? s_audio.dma_remaining - 1u : 0;
        break;
    default: break;
    }
    return (uint16_t)value;
}

uint32_t wii_audio_read(uint32_t ea, uint32_t size) {
    wii_audio_tick();
    if (size == 4 && ea >= WII_DSP_BASE && ea < WII_DSP_BASE + 0x1000u)
        return (uint32_t)read16(ea) << 16 | read16(ea + 2u);
    if (size == 2)
        return read16(ea);

    uint32_t value = 0;
    switch (ea) {
    case WII_AI_CONTROL: value = s_audio.ai_control; break;
    case WII_AI_VOLUME: value = s_audio.ai_volume; break;
    case WII_AI_SAMPLE_COUNTER: value = s_audio.ai_counter; break;
    case WII_AI_INTERRUPT_TIMING: value = s_audio.ai_interrupt_timing; break;
    default: break;
    }
    return mask_value(value, size);
}

WiiAudioStats wii_audio_stats(void) {
    return (WiiAudioStats){
        .dma_blocks = s_audio.dma_block_count,
        .dma_bytes = s_audio.dma_bytes,
        .sample_counter = s_audio.ai_counter,
        .last_left = s_audio.last_left,
        .last_right = s_audio.last_right,
    };
}

bool wii_audio_selftest(void) {
    const uint8_t source[WII_AUDIO_DMA_BLOCK_BYTES] = {
        0x12, 0x34, 0xFF, 0xFE, 0x12, 0x34, 0xFF, 0xFE,
        0x12, 0x34, 0xFF, 0xFE, 0x12, 0x34, 0xFF, 0xFE,
        0x12, 0x34, 0xFF, 0xFE, 0x12, 0x34, 0xFF, 0xFE,
        0x12, 0x34, 0xFF, 0xFE, 0x12, 0x34, 0xFF, 0xFE,
    };
    wii_audio_reset();
    wii_mem_write(0x00001000u, source, sizeof(source));
    wii_audio_write(WII_DSP_AUDIO_DMA_START_HI, 0, 2);
    wii_audio_write(WII_DSP_AUDIO_DMA_START_LO, 0x1000, 2);
    wii_audio_write(WII_DSP_AUDIO_DMA_CONTROL, 0x8001, 2);
    wii_audio_write(WII_AI_INTERRUPT_TIMING, 8, 4);
    wii_audio_write(WII_AI_CONTROL, AI_CTRL_PSTAT | AI_CTRL_AIINTMSK, 4);
    s_audio.dma_source = s_audio.dma_config_source;
    s_audio.dma_remaining = s_audio.dma_blocks;
    s_audio.dma_bytes = 0;
    s_audio.dma_block_count = 0;
    s_audio.ai_counter = 0;
    s_audio.ai_control &= ~AI_CTRL_AIINT;
    s_audio.dsp_control &= ~DSP_CTRL_AID;
    s_audio.last_tick = (uint64_t)OSGetTime();
    s_audio.tick_remainder = 0;
    wii_audio_advance(WII_AUDIO_DMA_BLOCK_FRAMES);

    WiiAudioStats stats = wii_audio_stats();
    bool ok = stats.dma_blocks == 1 && stats.dma_bytes == WII_AUDIO_DMA_BLOCK_BYTES &&
              stats.last_left == 0x1234 && stats.last_right == -2 &&
              stats.sample_counter == 8 &&
              (wii_audio_read(WII_AI_CONTROL, 4) & AI_CTRL_AIINT) != 0 &&
              (wii_audio_read(WII_DSP_CONTROL, 2) & DSP_CTRL_AID) != 0 &&
              wii_audio_read(WII_DSP_AUDIO_DMA_REMAINING, 2) == 0;
    wii_audio_write(WII_DSP_ARAM_DMA_CNT_LO, 0x20, 2);
    ok &= (wii_audio_read(WII_DSP_CONTROL, 2) & DSP_CTRL_ARAM) != 0 &&
          wii_audio_read(WII_DSP_CPU_MAILBOX_HI, 2) == 0x8000u;
    wii_audio_reset();
    return ok;
}
