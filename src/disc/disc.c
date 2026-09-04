// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#define _POSIX_C_SOURCE 200112L

#include "disc/disc.h"

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

// Ticket field offsets from the packed IOS ticket layout:
// title key at 0x1BF, title id at 0x1DC, common-key index at 0x1F1.
#define TICKET_TITLE_KEY   0x1BF
#define TICKET_TITLE_ID    0x1DC
#define TICKET_KEY_INDEX   0x1F1
#define PART_TMD_OFF_ADDR  0x2A8  // >>2-encoded offset of the partition TMD
#define PART_DATA_OFF_ADDR 0x2B8  // >>2-encoded offset of the partition's data
#define TMD_SYSTEM_VERSION 0x184

#define WBFS_MAGIC              0x57424653u
#define WBFS_HEADER_SIZE        12u
#define WBFS_DISC_HEADER_SIZE   0x100u
#define WBFS_DISC_TABLE_OFFSET  12u
#define WBFS_MAX_SECTOR_SHIFT   31u
#define WII_DISC_SIZE ((uint64_t)143432u * 2u * DISC_BLOCK_TOTAL)

// Wii common key.
static const uint8_t kWiiCommonKey[16] = {
    0xeb, 0xe4, 0x2a, 0x22, 0x5e, 0x85, 0x93, 0xe4,
    0x48, 0xd9, 0xc5, 0x45, 0x73, 0x81, 0xaa, 0xf7,
};

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8) | p[1];
}

static bool disc_read_backing(Disc *d, uint64_t offset, void *buf, uint32_t len) {
    if (offset > d->backing_size || len > d->backing_size - offset) return false;
    if (d->mem) {
        memcpy(buf, d->mem + offset, len);
        return true;
    }

    uint8_t *out = buf;
    while (len) {
        uint32_t i;
        for (i = 0; i < d->backing_count; ++i) {
            if (offset >= d->backing_base[i] &&
                offset - d->backing_base[i] < d->backing_len[i])
                break;
        }
        if (i == d->backing_count) return false;

        uint64_t file_offset = offset - d->backing_base[i];
        uint32_t count = (uint32_t)(d->backing_len[i] - file_offset);
        if (count > len) count = len;
        if ((uint64_t)(off_t)file_offset != file_offset ||
            fseeko(d->backing_files[i], (off_t)file_offset, SEEK_SET) != 0 ||
            fread(out, 1, count, d->backing_files[i]) != count)
            return false;

        out += count;
        offset += count;
        len -= count;
    }
    return true;
}

static bool disc_configure_wbfs(Disc *d) {
    uint8_t header[WBFS_HEADER_SIZE];
    if (!disc_read_backing(d, 0, header, sizeof(header)) ||
        be32(header) != WBFS_MAGIC)
        return false;

    const uint8_t hd_shift = header[8];
    const uint8_t wbfs_shift = header[9];
    if (hd_shift > WBFS_MAX_SECTOR_SHIFT || wbfs_shift > WBFS_MAX_SECTOR_SHIFT ||
        wbfs_shift < 15)
        return false;

    const uint64_t hd_size = 1ull << hd_shift;
    const uint64_t wbfs_size = 1ull << wbfs_shift;
    const uint64_t hd_count = be32(header + 4);
    if (hd_count == 0 || hd_count > UINT64_MAX / hd_size ||
        hd_count * hd_size != d->backing_size || wbfs_size < DISC_BLOCK_TOTAL)
        return false;

    const uint64_t blocks = (WII_DISC_SIZE + wbfs_size - 1) / wbfs_size;
    if (blocks > UINT32_MAX || hd_size < WBFS_DISC_HEADER_SIZE ||
        blocks > (UINT64_MAX - WBFS_DISC_HEADER_SIZE) / sizeof(uint16_t))
        return false;
    const uint64_t disc_info_size =
        (WBFS_DISC_HEADER_SIZE + blocks * sizeof(uint16_t) + hd_size - 1) & ~(hd_size - 1);

    uint8_t disc_table[DISC_MAX_BACKING_FILES] = {0};
    if (!disc_read_backing(d, WBFS_DISC_TABLE_OFFSET, disc_table, sizeof(disc_table))) return false;
    uint32_t slot = 0;
    while (slot < sizeof(disc_table) && disc_table[slot] == 0) ++slot;
    if (slot == sizeof(disc_table) || slot > (UINT64_MAX - hd_size) / disc_info_size)
        return false;

    const uint64_t disc_info = hd_size + slot * disc_info_size;
    if (disc_info > d->backing_size ||
        WBFS_DISC_HEADER_SIZE + blocks * sizeof(uint16_t) > d->backing_size - disc_info)
        return false;

    uint16_t *wlba = malloc((size_t)blocks * sizeof(*wlba));
    if (!wlba) return false;
    uint8_t *map = (uint8_t *)wlba;
    if (!disc_read_backing(d, disc_info + WBFS_DISC_HEADER_SIZE, map,
                           (uint32_t)(blocks * sizeof(*wlba)))) {
        free(wlba);
        return false;
    }
    for (uint32_t i = 0; i < blocks; ++i)
        wlba[i] = be16(map + i * sizeof(*wlba));

    d->is_wbfs = true;
    d->wbfs_sector_shift = wbfs_shift;
    d->wbfs_block_count = (uint32_t)blocks;
    d->wbfs_wlba = wlba;
    d->size = WII_DISC_SIZE;
    return true;
}

bool disc_read_raw(Disc *d, uint64_t offset, void *buf, uint32_t len) {
    if (offset > d->size || len > d->size - offset) return false;
    if (!d->is_wbfs) return disc_read_backing(d, offset, buf, len);

    uint8_t *out = buf;
    const uint64_t sector_size = 1ull << d->wbfs_sector_shift;
    while (len) {
        uint64_t block = offset >> d->wbfs_sector_shift;
        if (block >= d->wbfs_block_count) return false;
        uint64_t in_block = offset & (sector_size - 1);
        uint32_t count = (uint32_t)(sector_size - in_block);
        if (count > len) count = len;

        const uint16_t wlba = d->wbfs_wlba[block];
        if (wlba == 0 || (uint64_t)wlba > UINT64_MAX / sector_size ||
            !disc_read_backing(d, (uint64_t)wlba * sector_size + in_block, out, count))
            return false;

        out += count;
        offset += count;
        len -= count;
    }
    return true;
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
    d->backing_size = size;
    d->size = size;
    if (size < 0x20) return false;
    uint8_t magic[4];
    if (disc_read_backing(d, 0, magic, sizeof(magic)) && be32(magic) == WBFS_MAGIC &&
        !disc_configure_wbfs(d)) {
        disc_close(d);
        return false;
    }
    return parse_header(d);
}

static bool disc_add_backing_file(Disc *d, FILE *file, uint64_t size) {
    if (d->backing_count == DISC_MAX_BACKING_FILES) return false;
    const uint32_t i = d->backing_count++;
    d->backing_files[i] = file;
    d->backing_base[i] = d->backing_size;
    d->backing_len[i] = size;
    d->backing_size += size;
    return true;
}

static bool disc_open_split_files(Disc *d, const char *path) {
    const size_t path_len = strlen(path);
    if (path_len < 5 || strcmp(path + path_len - 5, ".wbfs") != 0) return true;

    char split_path[512];
    if (path_len >= sizeof(split_path)) return false;
    memcpy(split_path, path, path_len + 1);
    for (uint32_t i = 1; i < DISC_MAX_BACKING_FILES; ++i) {
        split_path[path_len - 1] = (char)('0' + i);
        FILE *file = fopen(split_path, "rb");
        if (!file) return true;
        if (fseeko(file, 0, SEEK_END) != 0) {
            fclose(file);
            return false;
        }
        off_t file_size = ftello(file);
        if (file_size <= 0 || !disc_add_backing_file(d, file, (uint64_t)file_size)) {
            fclose(file);
            return false;
        }
    }
    return true;
}

bool disc_open_file(Disc *d, const char *path) {
    memset(d, 0, sizeof(*d));
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    if (fseeko(file, 0, SEEK_END) != 0) { fclose(file); return false; }
    off_t file_size = ftello(file);
    if (file_size <= 0 || !disc_add_backing_file(d, file, (uint64_t)file_size)) {
        fclose(file);
        return false;
    }
    d->file = file;
    d->size = d->backing_size;
    uint8_t magic[4];
    if (disc_read_backing(d, 0, magic, sizeof(magic)) && be32(magic) == WBFS_MAGIC) {
        if (!disc_open_split_files(d, path) || !disc_configure_wbfs(d)) {
            disc_close(d);
            return false;
        }
    }
    if (!parse_header(d)) { disc_close(d); return false; }
    return true;
}

void disc_close(Disc *d) {
    for (uint32_t i = 0; i < d->backing_count; ++i)
        if (d->backing_files[i]) fclose(d->backing_files[i]);
    free(d->wbfs_wlba);
    d->file = NULL;
    d->mem = NULL;
    d->backing_count = 0;
    d->backing_size = 0;
    d->size = 0;
    d->is_wbfs = false;
    d->wbfs_wlba = NULL;
    d->wbfs_block_count = 0;
    d->valid = false;
    d->part_open = false;
}

bool disc_is_wbfs(const Disc *d) {
    return d && d->is_wbfs;
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

    uint8_t tmdoff[4], doff[4], ios_title_id[8];
    if (!disc_read_raw(d, part_offset + PART_TMD_OFF_ADDR, tmdoff, sizeof(tmdoff)) ||
        !disc_read_raw(d, part_offset + PART_DATA_OFF_ADDR, doff, sizeof(doff)))
        return false;
    const uint64_t tmd_offset = (uint64_t)be32(tmdoff) << 2;
    if (part_offset > UINT64_MAX - tmd_offset ||
        part_offset + tmd_offset > UINT64_MAX - TMD_SYSTEM_VERSION ||
        !disc_read_raw(d, part_offset + tmd_offset + TMD_SYSTEM_VERSION,
                       ios_title_id, sizeof(ios_title_id)))
        return false;
    uint64_t data_offset = (uint64_t)be32(doff) << 2;
    if (data_offset > UINT64_MAX - part_offset)
        return false;

    d->part_offset = part_offset;
    d->part_data = part_offset + data_offset;
    d->part_ios_version = be32(ios_title_id + 4);
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
    uint8_t *wbfs = NULL;
    Disc d = {0};

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
        put_be32(img + ST_PART_OFF + PART_TMD_OFF_ADDR, 0x400 >> 2);
        put_be32(img + ST_PART_OFF + PART_DATA_OFF_ADDR, ST_DATA_OFF >> 2);
        put_be32(img + ST_PART_OFF + 0x400 + TMD_SYSTEM_VERSION, 1);
        put_be32(img + ST_PART_OFF + 0x404 + TMD_SYSTEM_VERSION, 53);

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

        if (!disc_open_memory(&d, img, ST_IMG_SIZE)) break;
        if (!d.is_wii || strcmp(d.game_id, "RTST01") != 0) break;

        uint64_t part;
        if (!disc_find_game_partition(&d, &part) || part != ST_PART_OFF) break;
        if (!disc_open_partition(&d, part) || d.part_ios_version != 53) break;

        // Read across a cluster-internal boundary.
        uint8_t got[512];
        if (!disc_read_partition(&d, 0, got, sizeof(got))) break;
        if (memcmp(got, payload, sizeof(got)) != 0) break;
        if (!disc_read_partition(&d, 0x1000, got, sizeof(got))) break;
        if (memcmp(got, payload + 0x1000, sizeof(got)) != 0) break;
        disc_close(&d);

        const uint8_t wbfs_shift = 17;
        const uint64_t wbfs_size = 1ull << wbfs_shift;
        const uint32_t blocks = (uint32_t)((WII_DISC_SIZE + wbfs_size - 1) / wbfs_size);
        const uint32_t used_blocks = (uint32_t)((ST_IMG_SIZE + wbfs_size - 1) / wbfs_size);
        const uint16_t first_wlba = 2;
        const uint64_t wbfs_len = (uint64_t)(first_wlba + used_blocks) * wbfs_size;
        wbfs = calloc(1, (size_t)wbfs_len);
        if (!wbfs) break;

        memcpy(wbfs, "WBFS", 4);
        put_be32(wbfs + 4, (uint32_t)(wbfs_len >> 9));
        wbfs[8] = 9;
        wbfs[9] = wbfs_shift;
        wbfs[WBFS_DISC_TABLE_OFFSET] = 1;
        for (uint32_t i = 0; i < used_blocks; ++i) {
            const uint16_t wlba = first_wlba + i;
            wbfs[0x200 + WBFS_DISC_HEADER_SIZE + i * 2] = (uint8_t)(wlba >> 8);
            wbfs[0x200 + WBFS_DISC_HEADER_SIZE + i * 2 + 1] = (uint8_t)wlba;
            uint64_t src_off = (uint64_t)i * wbfs_size;
            uint32_t copy = (uint32_t)(ST_IMG_SIZE - src_off);
            if (copy > wbfs_size) copy = (uint32_t)wbfs_size;
            memcpy(wbfs + (uint64_t)wlba * wbfs_size, img + src_off, copy);
        }

        if (!disc_open_memory(&d, wbfs, wbfs_len) || !disc_is_wbfs(&d) ||
            strcmp(d.game_id, "RTST01") != 0) break;
        if (!disc_find_game_partition(&d, &part) || part != ST_PART_OFF ||
            !disc_open_partition(&d, part) || d.part_ios_version != 53) break;
        if (!disc_read_partition(&d, 0x1000, got, sizeof(got)) ||
            memcmp(got, payload + 0x1000, sizeof(got)) != 0) break;
        if (disc_read_raw(&d, (uint64_t)used_blocks * wbfs_size, got, sizeof(got))) break;
        disc_close(&d);

        ok = true;
    } while (0);

    disc_close(&d);
    free(wbfs);
    free(img);
    return ok;
}
