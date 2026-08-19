// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "ios/ios_ipc.h"

#include <stdio.h>
#include <string.h>

#include <whb/log.h>

#include "disc/disc.h"
#include "mem/wii_memory.h"

// Starlet ABI
#define IPC_OFF_CMD    0x00
#define IPC_OFF_RESULT 0x04
#define IPC_OFF_FD     0x08
#define IPC_OFF_ARG0   0x0C
#define IPC_OFF_ARG1   0x10
#define IPC_OFF_ARG2   0x14
#define IPC_OFF_ARG3   0x18
#define IPC_OFF_ARG4   0x1C

#define IOS_MAX_PATH 64

// These return canned values with the real IOS ioctl numbers and reply layouts
// TODO: actual backing

// ES ioctls actually issued by early boot.
#define IOCTL_ES_GETTITLEID  0x20
#define IOCTL_ES_GETTITLEDIR 0x1D

// Placeholder boot title
#define IOS_STUB_TITLE_ID 0x0000000152545354ULL  // 'RTST' in the low word

static int32_t es_ioctl(int32_t fd, uint32_t request,
                        uint32_t in_ea, uint32_t in_len,
                        uint32_t io_ea, uint32_t io_len) {
    (void)fd;
    switch (request) {
    case IOCTL_ES_GETTITLEID:
        if (io_len < 8 || !wii_mem_range(io_ea, 8)) return IOS_IPC_EINVAL;
        wii_write_u64(io_ea, IOS_STUB_TITLE_ID);
        return IOS_IPC_SUCCESS;
    case IOCTL_ES_GETTITLEDIR: {
        if (in_len < 8 || !wii_mem_range(in_ea, 8)) return IOS_IPC_EINVAL;
        if (io_len < 30 || !wii_mem_range(io_ea, 30)) return IOS_IPC_EINVAL;
        uint64_t title = wii_read_u64(in_ea);
        char dir[30];
        snprintf(dir, sizeof(dir), "/title/%08x/%08x/data",
                 (unsigned)(title >> 32), (unsigned)(title & 0xFFFFFFFFu));
        wii_mem_write(io_ea, dir, 30);
        return IOS_IPC_SUCCESS;
    }
    default:
        return IOS_IPC_EINVAL;
    }
}

// DI
#define DI_DVDLOWINQUIRY         0x12
#define DI_DVDLOWREADDISKID      0x70
#define DI_DVDLOWREAD            0x71
#define DI_DVDLOWOPENPARTITION   0x8B  // ioctlv
#define DI_DVDLOWUNENCRYPTEDREAD 0x8D

// DI results occupy their own code space
#define DI_RESULT_SUCCESS  1
#define DI_RESULT_DRIVE    2
#define DI_RESULT_SECURITY 32
#define DI_RESULT_BADARG   128

static Disc *s_disc = NULL;

void ios_ipc_mount_disc(Disc *disc) {
    s_disc = disc;
}

static int32_t di_ioctl(int32_t fd, uint32_t request,
                        uint32_t in_ea, uint32_t in_len,
                        uint32_t io_ea, uint32_t io_len) {
    (void)fd;
    switch (request) {
    case DI_DVDLOWINQUIRY:
        if (io_len < 8 || !wii_mem_range(io_ea, 8)) return DI_RESULT_SECURITY;
        // driveinfo: [0]=revision level, [1]=device code, [2..]=release date.
        wii_write_u32(io_ea + 0, 0x00000002u);
        wii_write_u32(io_ea + 4, 0x20080203u);
        return DI_RESULT_SUCCESS;

    case DI_DVDLOWREADDISKID: {
        if (!s_disc || !s_disc->valid) return DI_RESULT_DRIVE;
        void *dst = wii_mem_range(io_ea, 0x20);
        if (io_len < 0x20 || !dst) return DI_RESULT_SECURITY;
        if (!disc_read_raw(s_disc, 0, dst, 0x20)) return DI_RESULT_DRIVE;
        return DI_RESULT_SUCCESS;
    }

    case DI_DVDLOWREAD: {
        if (!s_disc || !s_disc->part_open) return DI_RESULT_SECURITY;
        if (in_len < 12 || !wii_mem_range(in_ea, 12)) return DI_RESULT_SECURITY;
        uint32_t length = wii_read_u32(in_ea + 4);
        uint64_t position = (uint64_t)wii_read_u32(in_ea + 8) << 2;
        void *dst = wii_mem_range(io_ea, length);
        if (io_len < length || !dst) return DI_RESULT_SECURITY;
        if (!disc_read_partition(s_disc, position, dst, length)) return DI_RESULT_DRIVE;
        return DI_RESULT_SUCCESS;
    }

    case DI_DVDLOWUNENCRYPTEDREAD: {
        if (!s_disc || !s_disc->valid) return DI_RESULT_DRIVE;
        if (in_len < 12 || !wii_mem_range(in_ea, 12)) return DI_RESULT_SECURITY;
        uint32_t length = wii_read_u32(in_ea + 4);
        uint64_t position = (uint64_t)wii_read_u32(in_ea + 8) << 2;
        void *dst = wii_mem_range(io_ea, length);
        if (io_len < length || !dst) return DI_RESULT_SECURITY;
        if (!disc_read_raw(s_disc, position, dst, length)) return DI_RESULT_DRIVE;
        return DI_RESULT_SUCCESS;
    }

    default:
        return DI_RESULT_BADARG;
    }
}

static int32_t di_ioctlv(int32_t fd, uint32_t request,
                         uint32_t in_count, uint32_t io_count,
                         const IosIoVector *vectors) {
    (void)fd;
    if (request != DI_DVDLOWOPENPARTITION) return DI_RESULT_BADARG;
    if (in_count != 3 || io_count != 2) return DI_RESULT_BADARG;
    if (!s_disc || !s_disc->valid) return DI_RESULT_DRIVE;

    uint32_t params = vectors[0].addr;
    if (vectors[0].size < 8 || !wii_mem_range(params, 8)) return DI_RESULT_SECURITY;
    uint64_t part_offset = (uint64_t)wii_read_u32(params + 4) << 2;
    if (!disc_open_partition(s_disc, part_offset)) return DI_RESULT_SECURITY;
    // TODO: copy the partition TMD into the first io vector once TMD parsing exists.
    return DI_RESULT_SUCCESS;
}

// ISFS
#define ISFS_IOCTL_GETSTATS  2
#define ISFS_IOCTLV_READDIR  4

static int32_t fs_ioctl(int32_t fd, uint32_t request,
                        uint32_t in_ea, uint32_t in_len,
                        uint32_t io_ea, uint32_t io_len) {
    (void)fd; (void)in_ea; (void)in_len;
    if (request == ISFS_IOCTL_GETSTATS) {
        // ISFSNandStats. 7 big-endian u32s. Canned to a healthy, basically empty NAND.
        if (io_len < 28 || !wii_mem_range(io_ea, 28)) return IOS_IPC_EINVAL;
        wii_write_u32(io_ea + 0x00, 0x00004000u);  // cluster_size
        wii_write_u32(io_ea + 0x04, 0x00007F00u);  // free_clusters
        wii_write_u32(io_ea + 0x08, 0x00000100u);  // used_clusters
        wii_write_u32(io_ea + 0x0C, 0x00000000u);  // bad_clusters
        wii_write_u32(io_ea + 0x10, 0x00000000u);  // reserved_clusters
        wii_write_u32(io_ea + 0x14, 0x00001700u);  // free_inodes
        wii_write_u32(io_ea + 0x18, 0x00000100u);  // used_inodes
        return IOS_IPC_SUCCESS;
    }
    return IOS_IPC_EINVAL;
}

// Stubbed directory listing returned by READDIR. Each name is a 13-byte NUL-padded field.
#define ISFS_NAME_LEN 13
static const char *const kFsStubDir[] = {"file0.bin", "file1.bin"};

static int32_t fs_ioctlv(int32_t fd, uint32_t request,
                         uint32_t in_count, uint32_t io_count,
                         const IosIoVector *vectors) {
    (void)fd;
    if (request != ISFS_IOCTLV_READDIR) return IOS_IPC_EINVAL;

    if (in_count != 1 || io_count != 2) return IOS_IPC_EINVAL;
    const IosIoVector *names = &vectors[1];
    const IosIoVector *count = &vectors[2];

    uint32_t n = (uint32_t)(sizeof(kFsStubDir) / sizeof(kFsStubDir[0]));
    if (count->size < 4 || !wii_mem_range(count->addr, 4)) return IOS_IPC_EINVAL;
    if (names->size < n * ISFS_NAME_LEN || !wii_mem_range(names->addr, n * ISFS_NAME_LEN))
        return IOS_IPC_EINVAL;

    for (uint32_t i = 0; i < n; ++i) {
        char field[ISFS_NAME_LEN];
        memset(field, 0, sizeof(field));
        strncpy(field, kFsStubDir[i], ISFS_NAME_LEN - 1);
        wii_mem_write(names->addr + i * ISFS_NAME_LEN, field, ISFS_NAME_LEN);
    }
    wii_write_u32(count->addr, n);
    return IOS_IPC_SUCCESS;
}

// SDI
static int32_t sdi_ioctl(int32_t fd, uint32_t request,
                         uint32_t in_ea, uint32_t in_len,
                         uint32_t io_ea, uint32_t io_len) {
    (void)fd; (void)request; (void)in_ea; (void)in_len; (void)io_ea; (void)io_len;
    return IOS_IPC_SUCCESS;
}

static const IosDevice kDevices[] = {
    {"/dev/es",          NULL, NULL, es_ioctl,  NULL},
    {"/dev/di",          NULL, NULL, di_ioctl,  di_ioctlv},
    {"/dev/fs",          NULL, NULL, fs_ioctl,  fs_ioctlv},
    {"/dev/sdio/slot0",  NULL, NULL, sdi_ioctl, NULL},
};
#define NUM_DEVICES (sizeof(kDevices) / sizeof(kDevices[0]))

#define IOS_MAX_FDS 32
static struct {
    bool used;
    const IosDevice *dev;
} s_fds[IOS_MAX_FDS];

void ios_ipc_init(void) {
    memset(s_fds, 0, sizeof(s_fds));
}

static const IosDevice *find_device(const char *name) {
    for (size_t i = 0; i < NUM_DEVICES; ++i)
        if (strcmp(kDevices[i].name, name) == 0) return &kDevices[i];
    return NULL;
}

static void read_guest_string(uint32_t ea, char *dst, size_t cap) {
    size_t i = 0;
    for (; i < cap - 1; ++i) {
        uint8_t c = wii_read_u8(ea + (uint32_t)i);
        if (c == 0) break;
        dst[i] = (char)c;
    }
    dst[i] = 0;
}

static int32_t do_open(uint32_t block) {
    char path[IOS_MAX_PATH];
    read_guest_string(wii_read_u32(block + IPC_OFF_ARG0), path, sizeof(path));
    int32_t mode = (int32_t)wii_read_u32(block + IPC_OFF_ARG1);

    const IosDevice *dev = find_device(path);
    if (!dev) return IOS_IPC_ENOENT;
    if (dev->open) {
        int32_t rc = dev->open(path, mode);
        if (rc < 0) return rc;
    }
    for (int32_t fd = 0; fd < IOS_MAX_FDS; ++fd) {
        if (!s_fds[fd].used) {
            s_fds[fd].used = true;
            s_fds[fd].dev  = dev;
            return fd;
        }
    }
    return IOS_IPC_EMAX;
}

static int32_t do_close(uint32_t block) {
    int32_t fd = (int32_t)wii_read_u32(block + IPC_OFF_FD);
    if (fd < 0 || fd >= IOS_MAX_FDS || !s_fds[fd].used) return IOS_IPC_EINVAL;
    if (s_fds[fd].dev->close) s_fds[fd].dev->close(fd);
    s_fds[fd].used = false;
    s_fds[fd].dev  = NULL;
    return IOS_IPC_SUCCESS;
}

static int32_t do_ioctl(uint32_t block) {
    int32_t fd = (int32_t)wii_read_u32(block + IPC_OFF_FD);
    if (fd < 0 || fd >= IOS_MAX_FDS || !s_fds[fd].used) return IOS_IPC_EINVAL;
    const IosDevice *dev = s_fds[fd].dev;
    if (!dev->ioctl) return IOS_IPC_EINVAL;
    return dev->ioctl(fd,
                      wii_read_u32(block + IPC_OFF_ARG0),
                      wii_read_u32(block + IPC_OFF_ARG1),
                      wii_read_u32(block + IPC_OFF_ARG2),
                      wii_read_u32(block + IPC_OFF_ARG3),
                      wii_read_u32(block + IPC_OFF_ARG4));
}

static int32_t do_ioctlv(uint32_t block) {
    int32_t fd = (int32_t)wii_read_u32(block + IPC_OFF_FD);
    if (fd < 0 || fd >= IOS_MAX_FDS || !s_fds[fd].used) return IOS_IPC_EINVAL;
    const IosDevice *dev = s_fds[fd].dev;
    if (!dev->ioctlv) return IOS_IPC_EINVAL;

    uint32_t request  = wii_read_u32(block + IPC_OFF_ARG0);
    uint32_t in_count = wii_read_u32(block + IPC_OFF_ARG1);
    uint32_t io_count = wii_read_u32(block + IPC_OFF_ARG2);
    uint32_t vec_base = wii_read_u32(block + IPC_OFF_ARG3);

    uint32_t total = in_count + io_count;
    if (total > IOS_IOCTLV_MAX_VECTORS) return IOS_IPC_EINVAL;
    if (total && !wii_mem_range(vec_base, total * 8)) return IOS_IPC_EINVAL;

    IosIoVector vectors[IOS_IOCTLV_MAX_VECTORS];
    for (uint32_t i = 0; i < total; ++i) {
        vectors[i].addr = wii_read_u32(vec_base + i * 8);
        vectors[i].size = wii_read_u32(vec_base + i * 8 + 4);
    }
    return dev->ioctlv(fd, request, in_count, io_count, vectors);
}

int32_t ios_ipc_dispatch(uint32_t block) {
    if (!wii_mem_range(block, 0x20)) return IOS_IPC_EINVAL;

    int32_t rc;
    switch (wii_read_u32(block + IPC_OFF_CMD)) {
    case IOS_CMD_OPEN:   rc = do_open(block);   break;
    case IOS_CMD_CLOSE:  rc = do_close(block);  break;
    case IOS_CMD_IOCTL:  rc = do_ioctl(block);  break;
    case IOS_CMD_IOCTLV: rc = do_ioctlv(block); break;
    case IOS_CMD_READ:
    case IOS_CMD_WRITE:
    case IOS_CMD_SEEK:
        rc = IOS_IPC_EINVAL;  // no device yet
        break;
    default:
        rc = IOS_IPC_EINVAL;
        break;
    }
    wii_write_u32(block + IPC_OFF_RESULT, (uint32_t)rc);
    return rc;
}

// Self test

#define ST_BLOCK 0x90200000u  // scratch command block in MEM2
#define ST_BUF   0x90200100u  // scratch data buffers
#define ST_VECS  0x90200400u  // scratch ioctlv vector array

static void build_open(uint32_t path_ea, int32_t mode) {
    wii_write_u32(ST_BLOCK + IPC_OFF_CMD, IOS_CMD_OPEN);
    wii_write_u32(ST_BLOCK + IPC_OFF_ARG0, path_ea);
    wii_write_u32(ST_BLOCK + IPC_OFF_ARG1, (uint32_t)mode);
}

static int32_t open_dev(const char *path) {
    uint32_t path_ea = ST_BUF;
    wii_mem_write(path_ea, path, (uint32_t)strlen(path) + 1);
    build_open(path_ea, IOS_OPEN_NONE);
    return ios_ipc_dispatch(ST_BLOCK);
}

IosIpcSelfTestResult ios_ipc_selftest(void) {
    ios_ipc_init();
    bool ok = true;

    // Unknown device
    if (open_dev("/dev/nope") != IOS_IPC_ENOENT) {
        WHBLogPrint("ios_ipc: open of unknown device did not return ENOENT");
        ok = false;
    }

    // Open /dev/es
    int32_t es_fd = open_dev("/dev/es");
    if (es_fd < 0) {
        WHBLogPrintf("ios_ipc: open /dev/es failed (%d)", es_fd);
        ok = false;
    } else {
        wii_write_u32(ST_BLOCK + IPC_OFF_CMD, IOS_CMD_IOCTL);
        wii_write_u32(ST_BLOCK + IPC_OFF_FD, (uint32_t)es_fd);
        wii_write_u32(ST_BLOCK + IPC_OFF_ARG0, IOCTL_ES_GETTITLEID);
        wii_write_u32(ST_BLOCK + IPC_OFF_ARG1, 0);            // in buffer
        wii_write_u32(ST_BLOCK + IPC_OFF_ARG2, 0);            // in len
        wii_write_u32(ST_BLOCK + IPC_OFF_ARG3, ST_BUF);       // io buffer
        wii_write_u32(ST_BLOCK + IPC_OFF_ARG4, 8);            // io len
        int32_t rc = ios_ipc_dispatch(ST_BLOCK);
        if (rc != IOS_IPC_SUCCESS || wii_read_u64(ST_BUF) != IOS_STUB_TITLE_ID) {
            WHBLogPrintf("ios_ipc: ES GETTITLEID wrong (rc=%d id=0x%016llX)",
                         rc, (unsigned long long)wii_read_u64(ST_BUF));
            ok = false;
        }
    }

    // Open /dev/fs
    int32_t fs_fd = open_dev("/dev/fs");
    if (fs_fd < 0) {
        WHBLogPrintf("ios_ipc: open /dev/fs failed (%d)", fs_fd);
        ok = false;
    } else {
        uint32_t path_ea  = ST_BUF;         // in[0]
        uint32_t names_ea = ST_BUF + 0x40;  // io[0]
        uint32_t count_ea = ST_BUF + 0x80;  // io[1]
        wii_mem_write(path_ea, "/title", 7);
        wii_write_u32(ST_VECS + 0x00, path_ea);  wii_write_u32(ST_VECS + 0x04, 7);
        wii_write_u32(ST_VECS + 0x08, names_ea); wii_write_u32(ST_VECS + 0x0C, 2 * ISFS_NAME_LEN);
        wii_write_u32(ST_VECS + 0x10, count_ea); wii_write_u32(ST_VECS + 0x14, 4);

        wii_write_u32(ST_BLOCK + IPC_OFF_CMD, IOS_CMD_IOCTLV);
        wii_write_u32(ST_BLOCK + IPC_OFF_FD, (uint32_t)fs_fd);
        wii_write_u32(ST_BLOCK + IPC_OFF_ARG0, ISFS_IOCTLV_READDIR);
        wii_write_u32(ST_BLOCK + IPC_OFF_ARG1, 1);        // in count
        wii_write_u32(ST_BLOCK + IPC_OFF_ARG2, 2);        // io count
        wii_write_u32(ST_BLOCK + IPC_OFF_ARG3, ST_VECS);  // vector base
        int32_t rc = ios_ipc_dispatch(ST_BLOCK);

        char name0[ISFS_NAME_LEN + 1] = {0};
        wii_mem_read(name0, names_ea, ISFS_NAME_LEN);
        if (rc != IOS_IPC_SUCCESS || wii_read_u32(count_ea) != 2 ||
            strcmp(name0, "file0.bin") != 0) {
            WHBLogPrintf("ios_ipc: FS READDIR wrong (rc=%d count=%u name0=%s)",
                         rc, wii_read_u32(count_ea), name0);
            ok = false;
        }
    }

    // Close /dev/es
    if (es_fd >= 0) {
        wii_write_u32(ST_BLOCK + IPC_OFF_CMD, IOS_CMD_CLOSE);
        wii_write_u32(ST_BLOCK + IPC_OFF_FD, (uint32_t)es_fd);
        if (ios_ipc_dispatch(ST_BLOCK) != IOS_IPC_SUCCESS) {
            WHBLogPrint("ios_ipc: close /dev/es failed");
            ok = false;
        }
        wii_write_u32(ST_BLOCK + IPC_OFF_CMD, IOS_CMD_IOCTL);
        wii_write_u32(ST_BLOCK + IPC_OFF_FD, (uint32_t)es_fd);
        wii_write_u32(ST_BLOCK + IPC_OFF_ARG0, IOCTL_ES_GETTITLEID);
        if (ios_ipc_dispatch(ST_BLOCK) != IOS_IPC_EINVAL) {
            WHBLogPrint("ios_ipc: ioctl on closed fd did not return EINVAL");
            ok = false;
        }
    }

    return ok ? IOS_IPC_SELFTEST_PASS : IOS_IPC_SELFTEST_FAIL;
}
