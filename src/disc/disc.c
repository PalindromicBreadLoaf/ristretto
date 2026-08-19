// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "disc/disc.h"

#include <stdlib.h>
#include <string.h>

// Ticket field offsets from the packed IOS ticket layout:
// title key at 0x1BF, title id at 0x1DC, common-key index at 0x1F1.
#define TICKET_TITLE_KEY   0x1BF
#define TICKET_TITLE_ID    0x1DC
#define TICKET_KEY_INDEX   0x1F1
#define PART_DATA_OFF_ADDR 0x2B8  // >>2-encoded offset of the partition's data

// Wii common key.
static const uint8_t kWiiCommonKey[16] = {
    0xeb, 0xe4, 0x2a, 0x22, 0x5e, 0x85, 0x93, 0xe4,
    0x48, 0xd9, 0xc5, 0x45, 0x73, 0x81, 0xaa, 0xf7,
};

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

bool disc_read_raw(Disc *d, uint64_t offset, void *buf, uint32_t len) {
    if (offset + len > d->size) return false;
    if (d->mem) {
        memcpy(buf, d->mem + offset, len);
        return true;
    }
    if (d->file) {
        // TODO: off_t may be 32-bit here, however dual-layer images past 2 GiB need a
        // 64-bit-seek backing before real retail discs load fully.
        if (fseek(d->file, (long)offset, SEEK_SET) != 0) return false;
        return fread(buf, 1, len, d->file) == len;
    }
    return false;
}

static bool parse_header(Disc *d) {
    uint8_t hdr[0x20];
    if (!disc_read_raw(d, 0, hdr, sizeof(hdr))) return false;

    memcpy(d->game_id, hdr, 6);
    d->game_id[6] = 0;

    if (be32(hdr + 0x18) == WII_DISC_MAGIC) {
        d->is_wii = true;
        d->valid = true;
    } else if (be32(hdr + 0x1C) == GC_DISC_MAGIC) {
        d->is_wii = false;
        d->valid = true;
    } else {
        d->valid = false;
    }
    return d->valid;
}

bool disc_open_memory(Disc *d, const uint8_t *image, uint64_t size) {
    memset(d, 0, sizeof(*d));
    d->mem = image;
    d->size = size;
    if (size < 0x20) return false;
    return parse_header(d);
}

bool disc_open_file(Disc *d, const char *path) {
    memset(d, 0, sizeof(*d));
    d->file = fopen(path, "rb");
    if (!d->file) return false;
    if (fseek(d->file, 0, SEEK_END) != 0) { disc_close(d); return false; }
    long sz = ftell(d->file);
    if (sz <= 0) { disc_close(d); return false; }
    d->size = (uint64_t)sz;
    if (!parse_header(d)) { disc_close(d); return false; }
    return true;
}

void disc_close(Disc *d) {
    if (d->file) fclose(d->file);
    d->file = NULL;
    d->mem = NULL;
    d->valid = false;
    d->part_open = false;
}

bool disc_find_game_partition(Disc *d, uint64_t *out_offset) {
    if (!d->valid || !d->is_wii) return false;

    for (uint32_t group = 0; group < 4; ++group) {
        uint8_t entry[8];
        if (!disc_read_raw(d, 0x40000 + group * 8, entry, 8)) return false;
        uint32_t count = be32(entry);
        uint64_t table = (uint64_t)be32(entry + 4) << 2;
        if (count == 0 || count > 8) continue;

        for (uint32_t i = 0; i < count; ++i) {
            uint8_t pe[8];
            if (!disc_read_raw(d, table + i * 8, pe, 8)) return false;
            uint64_t offset = (uint64_t)be32(pe) << 2;
            uint32_t type = be32(pe + 4);
            if (type == 0) {  // PARTITION_DATA
                *out_offset = offset;
                return true;
            }
        }
    }
    return false;
}

bool disc_open_partition(Disc *d, uint64_t part_offset) {
    if (!d->valid || !d->is_wii) return false;

    uint8_t enc_key[16], title_id[8], key_index;
    if (!disc_read_raw(d, part_offset + TICKET_TITLE_KEY, enc_key, 16)) return false;
    if (!disc_read_raw(d, part_offset + TICKET_TITLE_ID, title_id, 8)) return false;
    if (!disc_read_raw(d, part_offset + TICKET_KEY_INDEX, &key_index, 1)) return false;

    // TODO: common-key index 1 (Korean) and 2 (vWii)
    if (key_index != 0) return false;

    uint8_t iv[16] = {0};
    memcpy(iv, title_id, 8);  // IV = title id

    AesKey common;
    aes128_set_key(&common, kWiiCommonKey);
    uint8_t title_key[16];
    aes128_cbc_decrypt(&common, iv, enc_key, title_key, 16);
    aes128_set_key(&d->part_key, title_key);

    uint8_t doff[4];
    if (!disc_read_raw(d, part_offset + PART_DATA_OFF_ADDR, doff, 4)) return false;
    uint64_t data_offset = (uint64_t)be32(doff) << 2;

    d->part_offset = part_offset;
    d->part_data = part_offset + data_offset;
    d->part_open = true;
    return true;
}

// Cluster scratch
static uint8_t s_cluster[DISC_BLOCK_TOTAL];
static uint8_t s_plain[DISC_BLOCK_DATA];

bool disc_read_partition(Disc *d, uint64_t dec_offset, void *buf, uint32_t len) {
    if (!d->part_open) return false;

    uint8_t *out = (uint8_t *)buf;

    while (len > 0) {
        uint64_t block = dec_offset / DISC_BLOCK_DATA;
        uint32_t in_block = (uint32_t)(dec_offset % DISC_BLOCK_DATA);
        uint64_t disc_off = d->part_data + block * DISC_BLOCK_TOTAL;

        if (!disc_read_raw(d, disc_off, s_cluster, DISC_BLOCK_TOTAL)) return false;

        // The data IV is stored in the cluster's own hash header at 0x3D0.
        uint8_t iv[16];
        memcpy(iv, s_cluster + 0x3D0, 16);
        aes128_cbc_decrypt(&d->part_key, iv, s_cluster + DISC_BLOCK_HEADER, s_plain,
                           DISC_BLOCK_DATA);

        uint32_t avail = DISC_BLOCK_DATA - in_block;
        uint32_t copy = len < avail ? len : avail;
        memcpy(out, s_plain + in_block, copy);

        out += copy;
        dec_offset += copy;
        len -= copy;
    }
    return true;
}

// Self test
#define ST_PART_OFF   0x40040u
#define ST_DATA_OFF   0x8000u
#define ST_DATA_ABS   (ST_PART_OFF + ST_DATA_OFF)
#define ST_IMG_SIZE   (ST_DATA_ABS + DISC_BLOCK_TOTAL)

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

bool disc_selftest(void) {
    if (!aes_selftest()) return false;

    uint8_t *img = calloc(1, ST_IMG_SIZE);
    if (!img) return false;

    bool ok = false;
    do {
        // Disc header
        memcpy(img, "RTST01", 6);
        put_be32(img + 0x18, WII_DISC_MAGIC);

        // Partition group table
        put_be32(img + 0x40000, 1);                 // group 0 count
        put_be32(img + 0x40004, 0x40020 >> 2);      // group 0 table offset
        put_be32(img + 0x40020, ST_PART_OFF >> 2);  // partition 0 offset
        put_be32(img + 0x40024, 0);                 // partition 0 type = DATA

        // Ticket
        static const uint8_t plain_title_key[16] = {
            0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,
            0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff,0x00};
        static const uint8_t title_id[8] = {0x00,0x01,0x00,0x00,'R','T','S','T'};
        uint8_t iv_tk[16] = {0};
        memcpy(iv_tk, title_id, 8);

        AesKey common;
        aes128_set_key(&common, kWiiCommonKey);
        aes128_cbc_encrypt(&common, iv_tk, plain_title_key,
                           img + ST_PART_OFF + TICKET_TITLE_KEY, 16);
        memcpy(img + ST_PART_OFF + TICKET_TITLE_ID, title_id, 8);
        img[ST_PART_OFF + TICKET_KEY_INDEX] = 0;
        put_be32(img + ST_PART_OFF + PART_DATA_OFF_ADDR, ST_DATA_OFF >> 2);

        // One encrypted cluster
        static uint8_t payload[DISC_BLOCK_DATA];
        for (uint32_t i = 0; i < DISC_BLOCK_DATA; ++i) payload[i] = (uint8_t)(i * 3 + 7);
        static const uint8_t data_iv[16] = {
            0xa5,0x5a,0x01,0x02,0x03,0x04,0x05,0x06,
            0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e};
        memcpy(img + ST_DATA_ABS + 0x3D0, data_iv, 16);

        AesKey tk;
        aes128_set_key(&tk, plain_title_key);
        aes128_cbc_encrypt(&tk, data_iv, payload,
                           img + ST_DATA_ABS + DISC_BLOCK_HEADER, DISC_BLOCK_DATA);

        Disc d;
        if (!disc_open_memory(&d, img, ST_IMG_SIZE)) break;
        if (!d.is_wii || strcmp(d.game_id, "RTST01") != 0) break;

        uint64_t part;
        if (!disc_find_game_partition(&d, &part) || part != ST_PART_OFF) break;
        if (!disc_open_partition(&d, part)) break;

        // Read across a cluster-internal boundary.
        uint8_t got[512];
        if (!disc_read_partition(&d, 0, got, sizeof(got))) break;
        if (memcmp(got, payload, sizeof(got)) != 0) break;
        if (!disc_read_partition(&d, 0x1000, got, sizeof(got))) break;
        if (memcmp(got, payload + 0x1000, sizeof(got)) != 0) break;

        ok = true;
    } while (0);

    free(img);
    return ok;
}
