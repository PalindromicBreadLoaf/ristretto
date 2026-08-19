// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef RISTRETTO_DISC_AES_H
#define RISTRETTO_DISC_AES_H

#include <stdbool.h>
#include <stdint.h>

// Decrypt Wii disc partitions via AES-128

#define AES_BLOCK_SIZE 16
#define AES_KEY_SIZE   16

typedef struct {
    uint8_t round_keys[176];  // 11 keys of 16 bytes
} AesKey;

void aes128_set_key(AesKey *ctx, const uint8_t key[AES_KEY_SIZE]);

// Single 16-byte block, ECB.
void aes128_encrypt_block(const AesKey *ctx, const uint8_t in[AES_BLOCK_SIZE],
                          uint8_t out[AES_BLOCK_SIZE]);
void aes128_decrypt_block(const AesKey *ctx, const uint8_t in[AES_BLOCK_SIZE],
                          uint8_t out[AES_BLOCK_SIZE]);

// CBC over `len` bytes.
void aes128_cbc_encrypt(const AesKey *ctx, const uint8_t iv[AES_BLOCK_SIZE],
                        const uint8_t *in, uint8_t *out, uint32_t len);
void aes128_cbc_decrypt(const AesKey *ctx, const uint8_t iv[AES_BLOCK_SIZE],
                        const uint8_t *in, uint8_t *out, uint32_t len);

bool aes_selftest(void);

#endif  // RISTRETTO_DISC_AES_H
