// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef RISTRETTO_IOS_MOCHA_H
#define RISTRETTO_IOS_MOCHA_H

#include <stdint.h>

#include <coreinit/filesystem_fsa.h>

// MochaPayload IPC client (thanks Wuphax)

typedef enum {
    MOCHA_OK          = 0,
    MOCHA_NO_CFW      = -1,
    MOCHA_UNSUPPORTED = -2,
    MOCHA_MAX_CLIENT  = -3,
    MOCHA_ERROR       = -4,
} MochaStatus;

MochaStatus mocha_init(void);

MochaStatus mocha_unlock_fsa_client(FSAClientHandle client);

void mocha_deinit(void);

#endif  // RISTRETTO_IOS_MOCHA_H
