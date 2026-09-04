// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef RISTRETTO_DISC_DISC_H
#define RISTRETTO_DISC_DISC_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "disc/aes.h"

// Loopback reader over a Wii/GC disc image on SD/USB (or in memory).

#define WII_DISC_MAGIC        0x5D1C9EA3u
#define GC_DISC_MAGIC         0xC2339F3Du

#define DISC_BLOCK_TOTAL      0x8000u   // one on-disc cluster
#define DISC_BLOCK_HEADER     0x0400u   // hash/IV header
#define DISC_BLOCK_DATA       0x7C00u   // decrypted payload per cluster
#define DISC_MAX_BACKING_FILES 10u

typedef struct {
    FILE          *file;   // first file-backed image
    const uint8_t *mem;    // memory-backed image
    uint64_t       size;   // logical disc size in bytes

    FILE     *backing_files[DISC_MAX_BACKING_FILES];
    uint64_t  backing_base[DISC_MAX_BACKING_FILES];
    uint64_t  backing_len[DISC_MAX_BACKING_FILES];
    uint32_t  backing_count;
    uint64_t  backing_size;

    bool      is_wbfs;
    uint8_t   wbfs_sector_shift;
    uint32_t  wbfs_block_count;
    uint16_t *wbfs_wlba;

    bool  valid;           // a recognised disc header was found
    bool  is_wii;          // Wii or GameCube
    char  game_id[7];      // 6-char disc ID + NUL

    bool     part_open;    // a partition is open for decrypted reads
    uint64_t part_offset;  // absolute byte offset of the open partition
    uint64_t part_data;    // absolute byte offset of its first encrypted cluster
    uint32_t part_ios_version;  // IOS version requested by the partition TMD
    AesKey   part_key;     // decrypted title key for the open partition
} Disc;

// Attach a backing image and parse the disc header.
bool disc_open_memory(Disc *d, const uint8_t *image, uint64_t size);
bool disc_open_file(Disc *d, const char *path);
void disc_close(Disc *d);

// Raw (unencrypted) read straight from the backing image.
bool disc_read_raw(Disc *d, uint64_t offset, void *buf, uint32_t len);

bool disc_is_wbfs(const Disc *d);

// Locate the first data partition (type 0). Wii only.
bool disc_find_game_partition(Disc *d, uint64_t *out_offset);

// Open a partition and recover its title key from the ticket and note where its
// encrypted data begins.
bool disc_open_partition(Disc *d, uint64_t part_offset);

// Read decrypted partition data at a logical offset.
bool disc_read_partition(Disc *d, uint64_t dec_offset, void *buf, uint32_t len);

bool disc_selftest(void);

#endif  // RISTRETTO_DISC_DISC_H
