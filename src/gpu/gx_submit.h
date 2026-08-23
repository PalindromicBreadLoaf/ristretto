// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef RISTRETTO_GPU_GX_SUBMIT_H
#define RISTRETTO_GPU_GX_SUBMIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gpu/gx_draw.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GX_SUBMIT_MAX_PENDING (9u * 1024u * 1024u)

typedef struct {
    uint64_t bytes_received;
    uint64_t bytes_decoded;
    uint32_t submissions;
    uint32_t rejected_bytes;
    uint32_t pending_bytes;
    uint32_t primitives;
    uint32_t vertices;
    bool     failed;
} GXSubmitStats;

typedef struct {
    GXDrawPipeline draw;
    uint8_t       *pending;
    size_t         pending_len;
    size_t         pending_cap;
    uint64_t       bytes_received;
    uint64_t       bytes_decoded;
    uint32_t       submissions;
    uint32_t       rejected_bytes;
    bool           failed;
} GXSubmitter;

bool gx_submit_init(GXSubmitter *submitter, const GXDrawCallbacks *callbacks);
void gx_submit_shutdown(GXSubmitter *submitter);
void gx_submit_shutdown_after_gpu_idle(GXSubmitter *submitter);

// Start a new host frame without discarding folded GX state or an incomplete
// write-gather command.
void gx_submit_begin_frame(GXSubmitter *submitter);

bool gx_submit_push(GXSubmitter *submitter, const uint8_t *bytes, size_t len);

// Adapter for wii_mmio_set_wgp_sink()
void gx_submit_wgp_sink(void *user, const uint8_t *bytes, uint32_t len);

GXSubmitStats gx_submit_stats(const GXSubmitter *submitter);

bool gx_submit_selftest(void);

#ifdef __cplusplus
}
#endif

#endif  // RISTRETTO_GPU_GX_SUBMIT_H
