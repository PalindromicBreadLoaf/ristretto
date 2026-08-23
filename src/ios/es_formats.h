// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef RISTRETTO_IOS_ES_FORMATS_H
#define RISTRETTO_IOS_ES_FORMATS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t title_id;
    uint16_t content_count;
    bool is_vwii;
} EsTmdInfo;

typedef struct {
    uint64_t title_id;
    uint8_t common_key_index;
} EsTicketInfo;

bool es_tmd_parse(const void *data, size_t size, EsTmdInfo *out_info);
bool es_ticket_parse(const void *data, size_t size, EsTicketInfo *out_info);
bool es_formats_selftest(void);

#endif  // RISTRETTO_IOS_ES_FORMATS_H
