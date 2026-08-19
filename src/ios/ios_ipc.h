// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef RISTRETTO_IOS_IOS_IPC_H
#define RISTRETTO_IOS_IOS_IPC_H

#include <stdbool.h>
#include <stdint.h>

// HLE of the Wii IOS IPC surface.
// At this point is basically a stub

// IPC command types matching Starlet's ABI.
enum {
    IOS_CMD_OPEN   = 1,
    IOS_CMD_CLOSE  = 2,
    IOS_CMD_READ   = 3,
    IOS_CMD_WRITE  = 4,
    IOS_CMD_SEEK   = 5,
    IOS_CMD_IOCTL  = 6,
    IOS_CMD_IOCTLV = 7,
    IOS_REPLY      = 8,
};

// IOS_Open mode
enum {
    IOS_OPEN_NONE  = 0,
    IOS_OPEN_READ  = 1,
    IOS_OPEN_WRITE = 2,
    IOS_OPEN_RW    = 3,
};

// IOS return codes
enum {
    IOS_IPC_SUCCESS = 0,
    IOS_IPC_EACCES  = -1,
    IOS_IPC_EEXIST  = -2,
    IOS_IPC_EINVAL  = -4,
    IOS_IPC_EMAX    = -5,
    IOS_IPC_ENOENT  = -6,
    IOS_IPC_ENOMEM  = -22,
};

// An ioctlv argument vector
typedef struct {
    uint32_t addr;  // guest EA of the buffer
    uint32_t size;  // byte length
} IosIoVector;

#define IOS_IOCTLV_MAX_VECTORS 16

// A /dev/* device.
typedef struct IosDevice {
    const char *name;
    int32_t (*open)(const char *path, int32_t mode);
    void    (*close)(int32_t fd);
    int32_t (*ioctl)(int32_t fd, uint32_t request,
                     uint32_t in_ea, uint32_t in_len,
                     uint32_t io_ea, uint32_t io_len);
    int32_t (*ioctlv)(int32_t fd, uint32_t request,
                      uint32_t in_count, uint32_t io_count,
                      const IosIoVector *vectors);
} IosDevice;

// Register the inbuilt device stubs
void ios_ipc_init(void);

// Service one guest-resident IPC command block at effective address `cmd_block_ea`.
int32_t ios_ipc_dispatch(uint32_t cmd_block_ea);

typedef enum {
    IOS_IPC_SELFTEST_PASS,
    IOS_IPC_SELFTEST_FAIL,
} IosIpcSelfTestResult;

IosIpcSelfTestResult ios_ipc_selftest(void);

#endif  // RISTRETTO_IOS_IOS_IPC_H
