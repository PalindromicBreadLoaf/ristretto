// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "ios/es_formats.h"

#include <string.h>

enum {
    ES_SIG_RSA4096 = 0x00010000,
    ES_SIG_RSA2048 = 0x00010001,
    ES_SIG_ECC = 0x00010002,

    TMD_BODY_TITLE_ID = 0x0C,
    TMD_BODY_VWII = 0x03,
    TMD_BODY_CONTENT_COUNT = 0x5E,
    TMD_BODY_SIZE = 0x64,
    TMD_CONTENT_SIZE = 0x24,

    TICKET_BODY_TITLE_ID = 0x5C,
    TICKET_BODY_COMMON_KEY_INDEX = 0x71,
    TICKET_BODY_SIZE = 0x124,
};

static uint16_t read_be16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static uint32_t read_be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | p[3];
}

static uint64_t read_be64(const uint8_t *p) {
    return (uint64_t)read_be32(p) << 32 | read_be32(p + 4);
}

static size_t signature_size(const uint8_t *data, size_t size) {
    if (size < 4) return 0;
    switch (read_be32(data)) {
    case ES_SIG_RSA4096: return 0x280;
    case ES_SIG_RSA2048: return 0x180;
    case ES_SIG_ECC:     return 0x0C0;
    default:             return 0;
    }
}

bool es_tmd_parse(const void *data, size_t size, EsTmdInfo *out_info) {
    if (!data || !out_info) return false;
    const uint8_t *bytes = data;
    size_t sig_size = signature_size(bytes, size);
    if (sig_size == 0 || size < sig_size + TMD_BODY_SIZE) return false;

    const uint8_t *body = bytes + sig_size;
    uint16_t content_count = read_be16(body + TMD_BODY_CONTENT_COUNT);
    if (content_count > (size - sig_size - TMD_BODY_SIZE) / TMD_CONTENT_SIZE) return false;

    out_info->title_id = read_be64(body + TMD_BODY_TITLE_ID);
    out_info->content_count = content_count;
    out_info->is_vwii = body[TMD_BODY_VWII] != 0;
    return true;
}

bool es_ticket_parse(const void *data, size_t size, EsTicketInfo *out_info) {
    if (!data || !out_info) return false;
    const uint8_t *bytes = data;
    size_t sig_size = signature_size(bytes, size);
    if (sig_size == 0 || size < sig_size + TICKET_BODY_SIZE) return false;

    const uint8_t *body = bytes + sig_size;
    out_info->title_id = read_be64(body + TICKET_BODY_TITLE_ID);
    out_info->common_key_index = body[TICKET_BODY_COMMON_KEY_INDEX];
    return true;
}

bool es_formats_selftest(void) {
    uint8_t tmd[0x1E4] = {0};
    tmd[1] = 1; tmd[3] = 1;
    tmd[0x183] = 1;
    tmd[0x18C] = 0x00; tmd[0x18D] = 0x01; tmd[0x18E] = 0x00; tmd[0x18F] = 0x01;
    tmd[0x190] = 0x48; tmd[0x191] = 0x41; tmd[0x192] = 0x42; tmd[0x193] = 0x43;
    tmd[0x1DE] = 0x00; tmd[0x1DF] = 0x00;

    EsTmdInfo tmd_info;
    if (!es_tmd_parse(tmd, sizeof(tmd), &tmd_info) || !tmd_info.is_vwii ||
        tmd_info.content_count != 0 || tmd_info.title_id != 0x0001000148414243ULL)
        return false;

    uint8_t ticket[0x2A4] = {0};
    ticket[1] = 1; ticket[3] = 1;
    ticket[0x1DC] = 0x00; ticket[0x1DD] = 0x01; ticket[0x1DE] = 0x00; ticket[0x1DF] = 0x01;
    ticket[0x1E0] = 0x48; ticket[0x1E1] = 0x41; ticket[0x1E2] = 0x42; ticket[0x1E3] = 0x43;
    ticket[0x1F1] = 1;

    EsTicketInfo ticket_info;
    return es_ticket_parse(ticket, sizeof(ticket), &ticket_info) &&
           ticket_info.title_id == 0x0001000148414243ULL &&
           ticket_info.common_key_index == 1;
}
