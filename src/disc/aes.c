// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "disc/aes.h"

#include <string.h>

static const uint8_t kSbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

static uint8_t s_inv_sbox[256];
static bool    s_inv_ready = false;

static void build_inv_sbox(void) {
    if (s_inv_ready) return;
    for (int i = 0; i < 256; ++i) s_inv_sbox[kSbox[i]] = (uint8_t)i;
    s_inv_ready = true;
}

static const uint8_t kRcon[11] = {
    0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36,
};

// GF(2^8) multiply modulo 0x11b.
static uint8_t gf_mul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (int i = 0; i < 8; ++i) {
        if (b & 1) p ^= a;
        uint8_t hi = a & 0x80;
        a <<= 1;
        if (hi) a ^= 0x1b;
        b >>= 1;
    }
    return p;
}

void aes128_set_key(AesKey *ctx, const uint8_t key[AES_KEY_SIZE]) {
    build_inv_sbox();
    uint8_t *rk = ctx->round_keys;
    memcpy(rk, key, 16);
    for (int i = 4; i < 44; ++i) {
        uint8_t t[4];
        memcpy(t, rk + (i - 1) * 4, 4);
        if (i % 4 == 0) {
            uint8_t tmp = t[0];
            t[0] = (uint8_t)(kSbox[t[1]] ^ kRcon[i / 4]);
            t[1] = kSbox[t[2]];
            t[2] = kSbox[t[3]];
            t[3] = kSbox[tmp];
        }
        for (int j = 0; j < 4; ++j)
            rk[i * 4 + j] = (uint8_t)(rk[(i - 4) * 4 + j] ^ t[j]);
    }
}

static void add_round_key(uint8_t s[16], const uint8_t *rk) {
    for (int i = 0; i < 16; ++i) s[i] ^= rk[i];
}

void aes128_encrypt_block(const AesKey *ctx, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    memcpy(s, in, 16);
    add_round_key(s, ctx->round_keys);

    for (int round = 1; round <= 10; ++round) {
        for (int i = 0; i < 16; ++i) s[i] = kSbox[s[i]];

        // ShiftRows (byte at row r, col c is s[c*4 + r]).
        uint8_t t[16];
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                t[c * 4 + r] = s[((c + r) % 4) * 4 + r];
        memcpy(s, t, 16);

        if (round != 10) {
            for (int c = 0; c < 4; ++c) {
                uint8_t *col = s + c * 4;
                uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                col[0] = (uint8_t)(gf_mul(a0, 2) ^ gf_mul(a1, 3) ^ a2 ^ a3);
                col[1] = (uint8_t)(a0 ^ gf_mul(a1, 2) ^ gf_mul(a2, 3) ^ a3);
                col[2] = (uint8_t)(a0 ^ a1 ^ gf_mul(a2, 2) ^ gf_mul(a3, 3));
                col[3] = (uint8_t)(gf_mul(a0, 3) ^ a1 ^ a2 ^ gf_mul(a3, 2));
            }
        }

        add_round_key(s, ctx->round_keys + round * 16);
    }
    memcpy(out, s, 16);
}

void aes128_decrypt_block(const AesKey *ctx, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    memcpy(s, in, 16);
    add_round_key(s, ctx->round_keys + 10 * 16);

    for (int round = 9; round >= 0; --round) {
        // InvShiftRows (row r rotates right by r).
        uint8_t t[16];
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                t[c * 4 + r] = s[((c - r + 4) % 4) * 4 + r];
        memcpy(s, t, 16);

        for (int i = 0; i < 16; ++i) s[i] = s_inv_sbox[s[i]];

        add_round_key(s, ctx->round_keys + round * 16);

        if (round != 0) {
            for (int c = 0; c < 4; ++c) {
                uint8_t *col = s + c * 4;
                uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                col[0] = (uint8_t)(gf_mul(a0, 14) ^ gf_mul(a1, 11) ^ gf_mul(a2, 13) ^ gf_mul(a3, 9));
                col[1] = (uint8_t)(gf_mul(a0, 9) ^ gf_mul(a1, 14) ^ gf_mul(a2, 11) ^ gf_mul(a3, 13));
                col[2] = (uint8_t)(gf_mul(a0, 13) ^ gf_mul(a1, 9) ^ gf_mul(a2, 14) ^ gf_mul(a3, 11));
                col[3] = (uint8_t)(gf_mul(a0, 11) ^ gf_mul(a1, 13) ^ gf_mul(a2, 9) ^ gf_mul(a3, 14));
            }
        }
    }
    memcpy(out, s, 16);
}

void aes128_cbc_encrypt(const AesKey *ctx, const uint8_t iv[16],
                        const uint8_t *in, uint8_t *out, uint32_t len) {
    uint8_t prev[16];
    memcpy(prev, iv, 16);
    for (uint32_t off = 0; off < len; off += 16) {
        uint8_t block[16];
        for (int i = 0; i < 16; ++i) block[i] = (uint8_t)(in[off + i] ^ prev[i]);
        aes128_encrypt_block(ctx, block, out + off);
        memcpy(prev, out + off, 16);
    }
}

void aes128_cbc_decrypt(const AesKey *ctx, const uint8_t iv[16],
                        const uint8_t *in, uint8_t *out, uint32_t len) {
    uint8_t prev[16];
    memcpy(prev, iv, 16);
    for (uint32_t off = 0; off < len; off += 16) {
        uint8_t cipher[16];
        memcpy(cipher, in + off, 16);  // save in case in==out
        aes128_decrypt_block(ctx, cipher, out + off);
        for (int i = 0; i < 16; ++i) out[off + i] ^= prev[i];
        memcpy(prev, cipher, 16);
    }
}

bool aes_selftest(void) {
    // FIPS-197 appendix B / C.1 known-answer vector.
    static const uint8_t key[16] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f};
    static const uint8_t pt[16] = {
        0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
    static const uint8_t ct[16] = {
        0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a};

    AesKey ctx;
    aes128_set_key(&ctx, key);

    uint8_t buf[16];
    aes128_encrypt_block(&ctx, pt, buf);
    if (memcmp(buf, ct, 16) != 0) return false;
    aes128_decrypt_block(&ctx, ct, buf);
    if (memcmp(buf, pt, 16) != 0) return false;

    // CBC round-trip across two blocks with a non-zero IV.
    static const uint8_t iv[16] = {
        0x0f,0x0e,0x0d,0x0c,0x0b,0x0a,0x09,0x08,0x07,0x06,0x05,0x04,0x03,0x02,0x01,0x00};
    uint8_t plain[32], enc[32], dec[32];
    for (int i = 0; i < 32; ++i) plain[i] = (uint8_t)(i * 7 + 1);
    aes128_cbc_encrypt(&ctx, iv, plain, enc, 32);
    aes128_cbc_decrypt(&ctx, iv, enc, dec, 32);
    if (memcmp(plain, dec, 32) != 0) return false;

    return true;
}
