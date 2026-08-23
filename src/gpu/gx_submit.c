// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu/gx_submit.h"

#include <stdlib.h>
#include <string.h>

static bool reserve_pending(GXSubmitter *submitter, size_t additional) {
    if (additional > GX_SUBMIT_MAX_PENDING - submitter->pending_len)
        return false;

    size_t needed = submitter->pending_len + additional;
    if (needed <= submitter->pending_cap)
        return true;

    size_t capacity = submitter->pending_cap ? submitter->pending_cap : 256u;
    while (capacity < needed) {
        if (capacity >= GX_SUBMIT_MAX_PENDING / 2u) {
            capacity = GX_SUBMIT_MAX_PENDING;
            break;
        }
        capacity *= 2u;
    }

    uint8_t *pending = realloc(submitter->pending, capacity);
    if (!pending)
        return false;
    submitter->pending = pending;
    submitter->pending_cap = capacity;
    return true;
}

bool gx_submit_init(GXSubmitter *submitter, const GXDrawCallbacks *callbacks) {
    if (!submitter || !callbacks)
        return false;

    memset(submitter, 0, sizeof(*submitter));
    if (!gx_draw_init(&submitter->draw, callbacks))
        return false;
    submitter->draw.dry_run = true;
    return true;
}

void gx_submit_shutdown(GXSubmitter *submitter) {
    if (!submitter)
        return;
    gx_draw_shutdown(&submitter->draw);
    free(submitter->pending);
    memset(submitter, 0, sizeof(*submitter));
}

void gx_submit_begin_frame(GXSubmitter *submitter) {
    if (!submitter)
        return;
    gx_draw_begin_frame(&submitter->draw);
}

bool gx_submit_push(GXSubmitter *submitter, const uint8_t *bytes, size_t len) {
    if (!submitter || !bytes || len == 0 || submitter->failed)
        return false;
    if (!reserve_pending(submitter, len)) {
        submitter->rejected_bytes += (uint32_t)len;
        submitter->failed = true;
        return false;
    }

    memcpy(submitter->pending + submitter->pending_len, bytes, len);
    submitter->pending_len += len;
    submitter->bytes_received += len;
    submitter->submissions++;

    size_t consumed = gx_draw_submit(&submitter->draw, submitter->pending,
                                     submitter->pending_len);
    if (consumed > submitter->pending_len) {
        submitter->failed = true;
        return false;
    }
    if (consumed) {
        const size_t remaining = submitter->pending_len - consumed;
        if (remaining)
            memmove(submitter->pending, submitter->pending + consumed, remaining);
        submitter->pending_len = remaining;
        submitter->bytes_decoded += consumed;
    }
    return true;
}

void gx_submit_wgp_sink(void *user, const uint8_t *bytes, uint32_t len) {
    (void)gx_submit_push(user, bytes, len);
}

GXSubmitStats gx_submit_stats(const GXSubmitter *submitter) {
    GXSubmitStats stats = {0};
    if (!submitter)
        return stats;
    stats.bytes_received = submitter->bytes_received;
    stats.bytes_decoded = submitter->bytes_decoded;
    stats.submissions = submitter->submissions;
    stats.rejected_bytes = submitter->rejected_bytes;
    stats.pending_bytes = (uint32_t)submitter->pending_len;
    stats.primitives = submitter->draw.prims;
    stats.vertices = submitter->draw.verts;
    stats.failed = submitter->failed;
    return stats;
}

static void put_be32(uint8_t *out, size_t *offset, uint32_t value) {
    out[(*offset)++] = (uint8_t)(value >> 24);
    out[(*offset)++] = (uint8_t)(value >> 16);
    out[(*offset)++] = (uint8_t)(value >> 8);
    out[(*offset)++] = (uint8_t)value;
}

static void push_cp(uint8_t *out, size_t *offset, uint8_t reg, uint32_t value) {
    out[(*offset)++] = GX_OPCODE_LOAD_CP_REG;
    out[(*offset)++] = reg;
    put_be32(out, offset, value);
}

bool gx_submit_selftest(void) {
    static GXSubmitter submitter;
    GXDrawCallbacks callbacks = {0};
    if (!gx_submit_init(&submitter, &callbacks))
        return false;

    uint8_t fifo[128];
    size_t length = 0;
    push_cp(fifo, &length, GX_CP_VCD_LO, (1u << 9) | (1u << 13));
    push_cp(fifo, &length, GX_CP_VCD_HI, 0x00000001u);
    push_cp(fifo, &length, GX_CP_VAT_A, 0x01216009u);
    fifo[length++] = 0x90;
    fifo[length++] = 0;
    fifo[length++] = 3;
    for (uint32_t vertex = 0; vertex < 3; ++vertex) {
        for (uint32_t word = 0; word < 3; ++word)
            put_be32(fifo, &length, 0);
        fifo[length++] = 0xFF;
        fifo[length++] = 0xFF;
        fifo[length++] = 0xFF;
        fifo[length++] = 0xFF;
        put_be32(fifo, &length, 0);
        put_be32(fifo, &length, 0);
    }

    if (!gx_submit_push(&submitter, fifo, 4))
        goto fail;
    GXSubmitStats stats = gx_submit_stats(&submitter);
    if (stats.bytes_decoded != 0 || stats.pending_bytes != 4 || stats.primitives != 0)
        goto fail;

    if (!gx_submit_push(&submitter, fifo + 4, length - 5))
        goto fail;
    stats = gx_submit_stats(&submitter);
    if (stats.primitives != 0 || stats.pending_bytes == 0)
        goto fail;

    if (!gx_submit_push(&submitter, fifo + length - 1, 1))
        goto fail;
    stats = gx_submit_stats(&submitter);
    if (stats.failed || stats.bytes_received != length || stats.bytes_decoded != length ||
        stats.pending_bytes != 0 || stats.primitives != 1 || stats.vertices != 3)
        goto fail;

    gx_submit_begin_frame(&submitter);
    stats = gx_submit_stats(&submitter);
    if (stats.primitives != 0 || stats.vertices != 0 || stats.pending_bytes != 0)
        goto fail;

    gx_submit_shutdown(&submitter);
    return true;

fail:
    gx_submit_shutdown(&submitter);
    return false;
}
