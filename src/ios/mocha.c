// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "ios/mocha.h"

#include <coreinit/ios.h>

#define IPC_CUSTOM_GET_MOCHA_API_VERSION 0xF8

#define MOCHA_API_MAGIC 0xCAFEBABE

#define MOCHA_IOCTL_FSA_UNLOCK 0x28

#define IPC_BUF_ALIGN 0x40

static int32_t s_hax_fd = -1;
static uint32_t s_api_version = 0;
static bool s_init_done = false;

static MochaStatus mocha_check_api_version(uint32_t *out_version) {
    IOSHandle mcp = IOS_Open("/dev/mcp", IOS_OPEN_READ);
    if (mcp < 0)
        return MOCHA_NO_CFW;

    __attribute__((aligned(IPC_BUF_ALIGN))) uint32_t io[IPC_BUF_ALIGN / sizeof(uint32_t)];
    io[0] = IPC_CUSTOM_GET_MOCHA_API_VERSION;

    MochaStatus res;
    if (IOS_Ioctl(mcp, 100, io, sizeof(uint32_t), io, 2 * sizeof(uint32_t)) == IOS_ERROR_OK) {
        if (io[0] == MOCHA_API_MAGIC) {
            *out_version = io[1];
            res = MOCHA_OK;
        } else if (io[0] == 1) {
            res = MOCHA_UNSUPPORTED;
        } else {
            res = MOCHA_NO_CFW;
        }
    } else {
        res = MOCHA_NO_CFW;
    }

    IOS_Close(mcp);
    return res;
}

MochaStatus mocha_init(void) {
    if (s_init_done)
        return MOCHA_OK;

    IOSHandle hax = IOS_Open("/dev/iosuhax", (IOSOpenMode)0);
    if (hax < 0)
        return MOCHA_NO_CFW;

    uint32_t version = 0;
    MochaStatus v = mocha_check_api_version(&version);
    if (v != MOCHA_OK) {
        IOS_Close(hax);
        return v;
    }

    s_hax_fd = hax;
    s_api_version = version;
    s_init_done = true;
    return MOCHA_OK;
}

MochaStatus mocha_unlock_fsa_client(FSAClientHandle client) {
    if (!s_init_done)
        return MOCHA_NO_CFW;
    if (s_api_version < 1)
        return MOCHA_UNSUPPORTED;

    __attribute__((aligned(IPC_BUF_ALIGN))) uint8_t dummy[IPC_BUF_ALIGN];
    IOSError res = IOS_Ioctl(client, MOCHA_IOCTL_FSA_UNLOCK,
                             dummy, sizeof(dummy), dummy, sizeof(dummy));
    if (res == IOS_ERROR_OK)
        return MOCHA_OK;
    if (res == -5)
        return MOCHA_MAX_CLIENT;
    return MOCHA_ERROR;
}

void mocha_deinit(void) {
    if (s_hax_fd >= 0) {
        IOS_Close(s_hax_fd);
        s_hax_fd = -1;
    }
    s_api_version = 0;
    s_init_done = false;
}
