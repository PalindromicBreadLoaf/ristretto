// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "ios/ios_ipc.h"

#include <stdio.h>
#include <string.h>

#include <coreinit/filesystem_fsa.h>
#include <whb/log.h>

#include "disc/disc.h"
#include "ios/es_formats.h"
#include "ios/mocha.h"
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
#define IOS_MAX_FDS 32

#define VWII_NAND_DEVICE "/dev/slccmpt01"
#define VWII_NAND_MOUNT  "/vol/storage_slccmpt01"
#define VWII_NAND_DMA_CHUNK 0x4000u

typedef enum {
    IOS_FD_DEVICE,
    IOS_FD_VWII_FILE,
} IosFdKind;

static struct {
    bool used;
    IosFdKind kind;
    const IosDevice *dev;
    FSAFileHandle vwii_file;
    uint32_t size;
    uint32_t position;
} s_fds[IOS_MAX_FDS];

static struct {
    bool fsa_initialized;
    bool mounted;
    bool ready;
    FSAClientHandle client;
    char probe_path[IOS_MAX_PATH];
} s_vwii_nand = {.client = -1};

static uint8_t s_vwii_dma[VWII_NAND_DMA_CHUNK] __attribute__((aligned(0x40)));

static int32_t vwii_fsa_to_ios(FSError error) {
    switch (error) {
    case FS_ERROR_OK:
        return IOS_IPC_SUCCESS;
    case FS_ERROR_NOT_FOUND:
    case FS_ERROR_NOT_FILE:
    case FS_ERROR_NOT_DIR:
        return IOS_IPC_ENOENT;
    case FS_ERROR_ACCESS_ERROR:
    case FS_ERROR_PERMISSION_ERROR:
    case FS_ERROR_WRITE_PROTECTED:
        return IOS_IPC_EACCES;
    case FS_ERROR_MAX_FILES:
    case FS_ERROR_OUT_OF_RESOURCES:
        return IOS_IPC_EMAX;
    default:
        return IOS_IPC_EINVAL;
    }
}

static bool vwii_make_host_path(const char *guest_path, char *host_path, size_t host_cap) {
    if (!guest_path || guest_path[0] != '/' || strstr(guest_path, "..")) return false;
    if (strcmp(guest_path, "/shared1") != 0 && strncmp(guest_path, "/shared1/", 9) != 0 &&
        strcmp(guest_path, "/shared2") != 0 && strncmp(guest_path, "/shared2/", 9) != 0 &&
        strcmp(guest_path, "/title") != 0 && strncmp(guest_path, "/title/", 7) != 0 &&
        strcmp(guest_path, "/ticket") != 0 && strncmp(guest_path, "/ticket/", 8) != 0 &&
        strcmp(guest_path, "/sys") != 0 && strncmp(guest_path, "/sys/", 5) != 0)
        return false;

    int written = snprintf(host_path, host_cap, "%s%s", VWII_NAND_MOUNT, guest_path);
    return written >= 0 && (size_t)written < host_cap;
}

static int32_t vwii_open_file(const char *guest_path, FSAFileHandle *out_file,
                              uint32_t *out_size) {
    if (!s_vwii_nand.ready) return IOS_IPC_ENOENT;

    char host_path[FS_MAX_PATH + 1];
    if (!vwii_make_host_path(guest_path, host_path, sizeof(host_path))) return IOS_IPC_EINVAL;

    FSAFileHandle file = 0;
    FSError error = FSAOpenFileEx(s_vwii_nand.client, host_path, "r", (FSMode)0,
                                  FS_OPEN_FLAG_NONE, 0, &file);
    if (error != FS_ERROR_OK) return vwii_fsa_to_ios(error);

    FSAStat stat;
    error = FSAGetStatFile(s_vwii_nand.client, file, &stat);
    if (error != FS_ERROR_OK) {
        FSACloseFile(s_vwii_nand.client, file);
        return vwii_fsa_to_ios(error);
    }

    *out_file = file;
    *out_size = stat.size;
    return IOS_IPC_SUCCESS;
}

static int32_t vwii_read_file(FSAFileHandle file, uint32_t dst_ea, uint32_t length) {
    if (!wii_mem_range(dst_ea, length)) return IOS_IPC_EINVAL;

    uint32_t copied = 0;
    while (copied < length) {
        uint32_t want = length - copied;
        if (want > sizeof(s_vwii_dma)) want = sizeof(s_vwii_dma);

        FSError got = FSAReadFile(s_vwii_nand.client, s_vwii_dma, 1, want, file, 0);
        if (got < 0) return copied ? (int32_t)copied : vwii_fsa_to_ios(got);
        if (got == 0) break;

        wii_mem_write(dst_ea + copied, s_vwii_dma, (uint32_t)got);
        copied += (uint32_t)got;
        if ((uint32_t)got < want) break;
    }
    return (int32_t)copied;
}

static void vwii_nand_release(void) {
    if (s_vwii_nand.mounted)
        FSAUnmount(s_vwii_nand.client, VWII_NAND_MOUNT, FSA_UNMOUNT_FLAG_FORCE);
    if (s_vwii_nand.client >= 0)
        FSADelClient(s_vwii_nand.client);
    if (s_vwii_nand.fsa_initialized)
        FSAShutdown();
    mocha_deinit();
    memset(&s_vwii_nand, 0, sizeof(s_vwii_nand));
    s_vwii_nand.client = -1;
}

bool ios_ipc_vwii_nand_available(void) {
    return s_vwii_nand.ready;
}

// These return canned values with the real IOS ioctl numbers and reply layouts
// when SLCCMPT is unavailable.

// ES ioctls actually issued by early boot.
#define IOCTL_ES_LAUNCH       0x08
#define IOCTL_ES_GETVIEWCNT   0x12
#define IOCTL_ES_GETVIEWS     0x13
#define IOCTL_ES_GETTITLEID  0x20
#define IOCTL_ES_GETTITLEDIR 0x1D
#define IOCTL_ES_GETSTOREDTMDSIZE 0x34
#define IOCTL_ES_GETSTOREDTMD 0x35

#define IOS_TITLE_TYPE 0x00000001u
#define IOS_TMD_SIZE 0x1E4u
#define IOS_TMD_TITLE_ID_OFFSET 0x18Cu
#define IOS_TICKET_VIEW_SIZE 0xD8u
#define IOS_TICKET_VIEW_TITLE_ID_OFFSET 0x10u
#define IOS_VERSION_LOWMEM 0x80003140u

// Placeholder boot title
#define IOS_STUB_TITLE_ID 0x0000000152545354ULL  // 'RTST'

static const uint16_t kKnownIosVersions[] = {
    4, 9, 11, 12, 13, 14, 15, 17, 20, 21, 22, 28, 30, 31, 33, 34,
    35, 36, 37, 38, 40, 41, 43, 45, 46, 48, 50, 51, 52, 53, 55, 56,
    57, 58, 59, 60, 61, 62, 70, 80, 257,
};

static bool es_is_known_ios_title(uint64_t title_id) {
    if ((title_id >> 32) != IOS_TITLE_TYPE)
        return false;

    const uint32_t version = (uint32_t)title_id;
    for (uint32_t i = 0; i < sizeof(kKnownIosVersions) / sizeof(kKnownIosVersions[0]); ++i)
        if (kKnownIosVersions[i] == version) return true;
    return false;
}

static void es_write_ios_tmd(uint32_t dst_ea, uint64_t title_id) {
    uint8_t tmd[IOS_TMD_SIZE] = {0};
    tmd[1] = 1;
    tmd[3] = 1;
    for (uint32_t i = 0; i < 8; ++i)
        tmd[IOS_TMD_TITLE_ID_OFFSET + i] = (uint8_t)(title_id >> (56 - i * 8));
    wii_mem_write(dst_ea, tmd, sizeof(tmd));
}

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

static bool es_tmd_path(uint64_t title_id, char *path, size_t path_cap) {
    int written = snprintf(path, path_cap, "/title/%08x/%08x/content/title.tmd",
                           (unsigned)(title_id >> 32), (unsigned)title_id);
    return written >= 0 && (size_t)written < path_cap;
}

static int32_t es_get_ticket_view_count(const IosIoVector *vectors,
                                        uint32_t in_count, uint32_t io_count) {
    if (in_count != 1 || io_count != 1 || vectors[0].size < 8 || vectors[1].size < 4 ||
        !wii_mem_range(vectors[0].addr, 8) || !wii_mem_range(vectors[1].addr, 4))
        return IOS_IPC_EINVAL;

    const uint64_t title_id = wii_read_u64(vectors[0].addr);
    if (!es_is_known_ios_title(title_id)) return IOS_IPC_ENOENT;
    wii_write_u32(vectors[1].addr, 1);
    return IOS_IPC_SUCCESS;
}

static int32_t es_get_ticket_views(const IosIoVector *vectors,
                                   uint32_t in_count, uint32_t io_count) {
    if (in_count != 2 || io_count != 1 || vectors[0].size < 8 || vectors[1].size < 4 ||
        !wii_mem_range(vectors[0].addr, 8) || !wii_mem_range(vectors[1].addr, 4))
        return IOS_IPC_EINVAL;

    const uint64_t title_id = wii_read_u64(vectors[0].addr);
    const uint32_t requested_views = wii_read_u32(vectors[1].addr);
    if (!es_is_known_ios_title(title_id)) return IOS_IPC_ENOENT;
    if (requested_views == 0) return IOS_IPC_SUCCESS;
    if (vectors[2].size < IOS_TICKET_VIEW_SIZE ||
        !wii_mem_range(vectors[2].addr, IOS_TICKET_VIEW_SIZE))
        return IOS_IPC_EINVAL;

    uint8_t view[IOS_TICKET_VIEW_SIZE] = {0};
    for (uint32_t i = 0; i < 8; ++i)
        view[IOS_TICKET_VIEW_TITLE_ID_OFFSET + i] = (uint8_t)(title_id >> (56 - i * 8));
    wii_mem_write(vectors[2].addr, view, sizeof(view));
    return IOS_IPC_SUCCESS;
}

static int32_t es_launch_ios(const IosIoVector *vectors,
                             uint32_t in_count, uint32_t io_count) {
    if (in_count != 2 || io_count != 0 || vectors[0].size < 8 ||
        vectors[1].size < IOS_TICKET_VIEW_SIZE || !wii_mem_range(vectors[0].addr, 8) ||
        !wii_mem_range(vectors[1].addr, IOS_TICKET_VIEW_SIZE))
        return IOS_IPC_EINVAL;

    const uint64_t title_id = wii_read_u64(vectors[0].addr);
    if (!es_is_known_ios_title(title_id)) return IOS_IPC_ENOENT;

    const uint32_t version = (uint32_t)title_id;
    wii_write_u32(IOS_VERSION_LOWMEM, version);
    WHBLogPrintf("ios: reloaded IOS%u", version);
    return IOS_IPC_SUCCESS;
}

static int32_t es_ioctlv(int32_t fd, uint32_t request,
                         uint32_t in_count, uint32_t io_count,
                         const IosIoVector *vectors) {
    (void)fd;
    if (request == IOCTL_ES_GETVIEWCNT)
        return es_get_ticket_view_count(vectors, in_count, io_count);
    if (request == IOCTL_ES_GETVIEWS)
        return es_get_ticket_views(vectors, in_count, io_count);
    if (request == IOCTL_ES_LAUNCH)
        return es_launch_ios(vectors, in_count, io_count);

    if (request == IOCTL_ES_GETSTOREDTMDSIZE) {
        if (in_count != 1 || io_count != 1 || vectors[0].size < 8 || vectors[1].size < 4 ||
            !wii_mem_range(vectors[0].addr, 8) || !wii_mem_range(vectors[1].addr, 4))
            return IOS_IPC_EINVAL;
        const uint64_t title_id = wii_read_u64(vectors[0].addr);
        if (es_is_known_ios_title(title_id)) {
            wii_write_u32(vectors[1].addr, IOS_TMD_SIZE);
            return IOS_IPC_SUCCESS;
        }
        if (!s_vwii_nand.ready) return IOS_IPC_ENOENT;
        char path[IOS_MAX_PATH];
        if (!es_tmd_path(title_id, path, sizeof(path))) return IOS_IPC_EINVAL;
        FSAFileHandle file = 0;
        uint32_t size = 0;
        int32_t rc = vwii_open_file(path, &file, &size);
        if (rc < 0) return rc;
        FSACloseFile(s_vwii_nand.client, file);
        wii_write_u32(vectors[1].addr, size);
        return IOS_IPC_SUCCESS;
    }

    if (request == IOCTL_ES_GETSTOREDTMD) {
        if (in_count != 2 || io_count != 1 || vectors[0].size < 8 || vectors[1].size < 4 ||
            !wii_mem_range(vectors[0].addr, 8) || !wii_mem_range(vectors[1].addr, 4) ||
            !wii_mem_range(vectors[2].addr, vectors[2].size))
            return IOS_IPC_EINVAL;
        const uint64_t title_id = wii_read_u64(vectors[0].addr);
        if (es_is_known_ios_title(title_id)) {
            if (wii_read_u32(vectors[1].addr) < IOS_TMD_SIZE || vectors[2].size < IOS_TMD_SIZE)
                return IOS_IPC_EINVAL;
            es_write_ios_tmd(vectors[2].addr, title_id);
            return IOS_IPC_SUCCESS;
        }
        if (!s_vwii_nand.ready) return IOS_IPC_ENOENT;
        char path[IOS_MAX_PATH];
        if (!es_tmd_path(title_id, path, sizeof(path))) return IOS_IPC_EINVAL;
        FSAFileHandle file = 0;
        uint32_t size = 0;
        int32_t rc = vwii_open_file(path, &file, &size);
        if (rc < 0) return rc;
        if (wii_read_u32(vectors[1].addr) < size || vectors[2].size < size) {
            FSACloseFile(s_vwii_nand.client, file);
            return IOS_IPC_EINVAL;
        }
        rc = vwii_read_file(file, vectors[2].addr, size);
        FSACloseFile(s_vwii_nand.client, file);
        if (rc != (int32_t)size) return rc;
        EsTmdInfo info;
        void *raw_tmd = wii_mem_range(vectors[2].addr, size);
        if (!es_tmd_parse(raw_tmd, size, &info) || info.title_id != title_id)
            return IOS_IPC_EINVAL;
        return IOS_IPC_SUCCESS;
    }

    return IOS_IPC_EINVAL;
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

void ios_ipc_vwii_nand_spike(void) {
    if (s_vwii_nand.ready) return;

    FSError init = FSAInit();
    if (init != FS_ERROR_OK) {
        WHBLogPrintf("vwii nand: NO ACCESS (FSAInit=%d)", (int)init);
        return;
    }
    s_vwii_nand.fsa_initialized = true;

    FSAClientHandle client = FSAAddClient(NULL);
    if (client < 0) {
        WHBLogPrintf("vwii nand: NO ACCESS (FSAAddClient=%d)", (int)client);
        vwii_nand_release();
        return;
    }
    s_vwii_nand.client = client;

    MochaStatus mocha = mocha_init();
    if (mocha == MOCHA_OK) {
        mocha = mocha_unlock_fsa_client(client);
        if (mocha == MOCHA_OK)
            WHBLogPrint("vwii nand: FSA client elevated via mocha");
        else
            WHBLogPrintf("vwii nand: mocha unlock failed (%d)", (int)mocha);
    } else {
        WHBLogPrintf("vwii nand: no CFW FSA elevation (mocha=%d)", (int)mocha);
    }

    FSError mount = FSAMount(client, VWII_NAND_DEVICE, VWII_NAND_MOUNT,
                             FSA_MOUNT_FLAG_GLOBAL_MOUNT, NULL, 0);
    if (mount != FS_ERROR_OK) {
        WHBLogPrintf("vwii nand: NO ACCESS (FSAMount %s=%d)",
                     VWII_NAND_DEVICE, (int)mount);
        vwii_nand_release();
        return;
    }
    s_vwii_nand.mounted = true;

    static const char *const probe_paths[] = {
        VWII_NAND_MOUNT "/shared1/content.map",
        VWII_NAND_MOUNT "/shared2/sys/SYSCONF",
    };
    static const char *const guest_probe_paths[] = {
        "/shared1/content.map",
        "/shared2/sys/SYSCONF",
    };
    static uint8_t read_buf[512] __attribute__((aligned(0x40)));

    int32_t bytes_read = -1;
    const char *read_path = NULL;
    FSError read_error = FS_ERROR_NOT_FOUND;
    for (uint32_t i = 0; i < sizeof(probe_paths) / sizeof(probe_paths[0]); ++i) {
        FSAFileHandle file = 0;
        read_error = FSAOpenFileEx(client, probe_paths[i], "r", (FSMode)0,
                                    FS_OPEN_FLAG_NONE, 0, &file);
        if (read_error != FS_ERROR_OK)
            continue;

        read_error = FSAReadFile(client, read_buf, 1, sizeof(read_buf), file, 0);
        FSACloseFile(client, file);
        if (read_error >= 0) {
            bytes_read = read_error;
            read_path = guest_probe_paths[i];
            break;
        }
    }

    if (bytes_read >= 0) {
        s_vwii_nand.ready = true;
        snprintf(s_vwii_nand.probe_path, sizeof(s_vwii_nand.probe_path), "%s", read_path);
        WHBLogPrintf("vwii nand: MOUNTED read=%d bytes (%s)", bytes_read, read_path);
        mocha_deinit();
    } else {
        WHBLogPrintf("vwii nand: NO ACCESS (FSAOpenFile/ReadFile=%d)", (int)read_error);
        vwii_nand_release();
    }
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
#define ISFS_IOCTL_GETFILESTATS 11

// ISFS directory names are fixed 13-byte NUL-padded fields.
#define ISFS_NAME_LEN 13

static int32_t fs_read_vwii_dir(const char *guest_path, const IosIoVector *names,
                                uint32_t max_names, uint32_t *out_count) {
    char host_path[FS_MAX_PATH + 1];
    if (!vwii_make_host_path(guest_path, host_path, sizeof(host_path))) return IOS_IPC_EINVAL;

    FSADirectoryHandle dir = 0;
    FSError error = FSAOpenDir(s_vwii_nand.client, host_path, &dir);
    if (error != FS_ERROR_OK) return vwii_fsa_to_ios(error);

    static FSADirectoryEntry entry __attribute__((aligned(0x40)));
    uint32_t total = 0;
    for (;;) {
        error = FSAReadDir(s_vwii_nand.client, dir, &entry);
        if (error == FS_ERROR_END_OF_DIR) break;
        if (error != FS_ERROR_OK) {
            FSACloseDir(s_vwii_nand.client, dir);
            return vwii_fsa_to_ios(error);
        }

        size_t name_len = 0;
        while (name_len < sizeof(entry.name) && entry.name[name_len]) ++name_len;
        if (name_len == 0 || name_len >= ISFS_NAME_LEN) continue;

        if (names && total < max_names) {
            char field[ISFS_NAME_LEN] = {0};
            memcpy(field, entry.name, name_len);
            wii_mem_write(names->addr + total * ISFS_NAME_LEN, field, sizeof(field));
        }
        ++total;
    }
    FSACloseDir(s_vwii_nand.client, dir);

    *out_count = names && total > max_names ? max_names : total;
    return IOS_IPC_SUCCESS;
}

static int32_t fs_ioctl(int32_t fd, uint32_t request,
                        uint32_t in_ea, uint32_t in_len,
                        uint32_t io_ea, uint32_t io_len) {
    (void)in_ea; (void)in_len;
    if (request == ISFS_IOCTL_GETFILESTATS) {
        if (fd < 0 || fd >= IOS_MAX_FDS || !s_fds[fd].used ||
            s_fds[fd].kind != IOS_FD_VWII_FILE ||
            io_len < 8 || !wii_mem_range(io_ea, 8))
            return IOS_IPC_EINVAL;
        wii_write_u32(io_ea + 0, s_fds[fd].size);
        wii_write_u32(io_ea + 4, s_fds[fd].position);
        return IOS_IPC_SUCCESS;
    }
    if (request == ISFS_IOCTL_GETSTATS) {
        if (io_len < 28 || !wii_mem_range(io_ea, 28)) return IOS_IPC_EINVAL;
        uint32_t free_clusters = 0x00007F00u;
        if (s_vwii_nand.ready) {
            uint64_t free_bytes = 0;
            if (FSAGetFreeSpaceSize(s_vwii_nand.client, VWII_NAND_MOUNT, &free_bytes) == FS_ERROR_OK)
                free_clusters = free_bytes / 0x4000u > UINT32_MAX ? UINT32_MAX :
                                (uint32_t)(free_bytes / 0x4000u);
        }
        wii_write_u32(io_ea + 0x00, 0x00004000u);  // cluster_size
        wii_write_u32(io_ea + 0x04, free_clusters);
        wii_write_u32(io_ea + 0x08, 0x00000100u);  // used_clusters
        wii_write_u32(io_ea + 0x0C, 0x00000000u);  // bad_clusters
        wii_write_u32(io_ea + 0x10, 0x00000000u);  // reserved_clusters
        wii_write_u32(io_ea + 0x14, 0x00001700u);  // free_inodes
        wii_write_u32(io_ea + 0x18, 0x00000100u);  // used_inodes
        return IOS_IPC_SUCCESS;
    }
    return IOS_IPC_EINVAL;
}

static const char *const kFsStubDir[] = {"file0.bin", "file1.bin"};

static int32_t fs_ioctlv(int32_t fd, uint32_t request,
                         uint32_t in_count, uint32_t io_count,
                         const IosIoVector *vectors) {
    (void)fd;
    if (request != ISFS_IOCTLV_READDIR) return IOS_IPC_EINVAL;

    const IosIoVector *names = NULL;
    const IosIoVector *count = NULL;
    uint32_t max_names = 0;
    if (in_count == 1 && io_count == 1) {
        count = &vectors[1];
    } else if (in_count == 1 && io_count == 2) {
        names = &vectors[1];
        count = &vectors[2];
        if (names->size % ISFS_NAME_LEN) return IOS_IPC_EINVAL;
        max_names = names->size / ISFS_NAME_LEN;
    } else if (in_count == 2 && io_count == 2) {
        if (vectors[1].size < 4 || !wii_mem_range(vectors[1].addr, 4)) return IOS_IPC_EINVAL;
        names = &vectors[2];
        count = &vectors[3];
        max_names = wii_read_u32(vectors[1].addr);
        if (max_names > UINT32_MAX / ISFS_NAME_LEN) return IOS_IPC_EINVAL;
        if (names->size < max_names * ISFS_NAME_LEN) return IOS_IPC_EINVAL;
    } else {
        return IOS_IPC_EINVAL;
    }
    if (count->size < 4 || !wii_mem_range(count->addr, 4)) return IOS_IPC_EINVAL;
    if (names && !wii_mem_range(names->addr, names->size))
        return IOS_IPC_EINVAL;

    char path[IOS_MAX_PATH];
    if (vectors[0].size == 0 || !wii_mem_range(vectors[0].addr, vectors[0].size))
        return IOS_IPC_EINVAL;
    uint32_t path_bytes = vectors[0].size < sizeof(path) ? vectors[0].size : sizeof(path);
    wii_mem_read(path, vectors[0].addr, path_bytes);
    if (!memchr(path, 0, path_bytes)) return IOS_IPC_EINVAL;
    path[sizeof(path) - 1] = 0;

    if (s_vwii_nand.ready) {
        uint32_t n = 0;
        int32_t rc = fs_read_vwii_dir(path, names, max_names, &n);
        if (rc == IOS_IPC_SUCCESS) wii_write_u32(count->addr, n);
        return rc;
    }

    uint32_t n = (uint32_t)(sizeof(kFsStubDir) / sizeof(kFsStubDir[0]));
    if (!names) {
        wii_write_u32(count->addr, n);
        return IOS_IPC_SUCCESS;
    }
    if (max_names < n) n = max_names;

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
    {"/dev/es",          NULL, NULL, es_ioctl,  es_ioctlv},
    {"/dev/di",          NULL, NULL, di_ioctl,  di_ioctlv},
    {"/dev/fs",          NULL, NULL, fs_ioctl,  fs_ioctlv},
    {"/dev/sdio/slot0",  NULL, NULL, sdi_ioctl, NULL},
};
#define NUM_DEVICES (sizeof(kDevices) / sizeof(kDevices[0]))

void ios_ipc_init(void) {
    memset(s_fds, 0, sizeof(s_fds));
}

void ios_ipc_shutdown(void) {
    for (int32_t fd = 0; fd < IOS_MAX_FDS; ++fd) {
        if (s_fds[fd].used && s_fds[fd].kind == IOS_FD_VWII_FILE)
            FSACloseFile(s_vwii_nand.client, s_fds[fd].vwii_file);
    }
    memset(s_fds, 0, sizeof(s_fds));
    vwii_nand_release();
}

static const IosDevice *find_device(const char *name) {
    for (size_t i = 0; i < NUM_DEVICES; ++i)
        if (strcmp(kDevices[i].name, name) == 0) return &kDevices[i];
    return NULL;
}

static bool read_guest_string(uint32_t ea, char *dst, size_t cap) {
    if (!cap) return false;
    size_t i = 0;
    for (; i < cap - 1; ++i) {
        if (!wii_mem_range(ea + (uint32_t)i, 1)) {
            dst[i] = 0;
            return false;
        }
        uint8_t c = wii_read_u8(ea + (uint32_t)i);
        if (c == 0) {
            dst[i] = 0;
            return true;
        }
        dst[i] = (char)c;
    }
    dst[i] = 0;
    return false;
}

static int32_t do_open(uint32_t block) {
    char path[IOS_MAX_PATH];
    if (!read_guest_string(wii_read_u32(block + IPC_OFF_ARG0), path, sizeof(path)))
        return IOS_IPC_EINVAL;
    int32_t mode = (int32_t)wii_read_u32(block + IPC_OFF_ARG1);

    int32_t fd = -1;
    for (int32_t i = 0; i < IOS_MAX_FDS; ++i) {
        if (!s_fds[i].used) {
            fd = i;
            break;
        }
    }
    if (fd < 0) return IOS_IPC_EMAX;

    const IosDevice *dev = find_device(path);
    if (dev) {
        if (dev->open) {
            int32_t rc = dev->open(path, mode);
            if (rc < 0) return rc;
        }
        s_fds[fd].used = true;
        s_fds[fd].kind = IOS_FD_DEVICE;
        s_fds[fd].dev = dev;
        return fd;
    }

    if (mode != IOS_GUEST_OPEN_NONE && mode != IOS_GUEST_OPEN_READ)
        return IOS_IPC_EACCES;
    FSAFileHandle file = 0;
    uint32_t size = 0;
    int32_t rc = vwii_open_file(path, &file, &size);
    if (rc < 0) return rc;

    s_fds[fd].used = true;
    s_fds[fd].kind = IOS_FD_VWII_FILE;
    s_fds[fd].dev = find_device("/dev/fs");
    s_fds[fd].vwii_file = file;
    s_fds[fd].size = size;
    s_fds[fd].position = 0;
    return fd;
}

static int32_t do_close(uint32_t block) {
    int32_t fd = (int32_t)wii_read_u32(block + IPC_OFF_FD);
    if (fd < 0 || fd >= IOS_MAX_FDS || !s_fds[fd].used) return IOS_IPC_EINVAL;
    int32_t rc = IOS_IPC_SUCCESS;
    if (s_fds[fd].kind == IOS_FD_VWII_FILE) {
        FSError error = FSACloseFile(s_vwii_nand.client, s_fds[fd].vwii_file);
        rc = vwii_fsa_to_ios(error);
    } else if (s_fds[fd].dev->close) {
        s_fds[fd].dev->close(fd);
    }
    memset(&s_fds[fd], 0, sizeof(s_fds[fd]));
    return rc;
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

static int32_t do_read(uint32_t block) {
    int32_t fd = (int32_t)wii_read_u32(block + IPC_OFF_FD);
    if (fd < 0 || fd >= IOS_MAX_FDS || !s_fds[fd].used ||
        s_fds[fd].kind != IOS_FD_VWII_FILE)
        return IOS_IPC_EINVAL;

    int32_t rc = vwii_read_file(s_fds[fd].vwii_file,
                                wii_read_u32(block + IPC_OFF_ARG0),
                                wii_read_u32(block + IPC_OFF_ARG1));
    if (rc > 0) s_fds[fd].position += (uint32_t)rc;
    return rc;
}

static int32_t do_write(uint32_t block) {
    int32_t fd = (int32_t)wii_read_u32(block + IPC_OFF_FD);
    if (fd < 0 || fd >= IOS_MAX_FDS || !s_fds[fd].used) return IOS_IPC_EINVAL;
    return s_fds[fd].kind == IOS_FD_VWII_FILE ? IOS_IPC_EACCES : IOS_IPC_EINVAL;
}

static int32_t do_seek(uint32_t block) {
    int32_t fd = (int32_t)wii_read_u32(block + IPC_OFF_FD);
    if (fd < 0 || fd >= IOS_MAX_FDS || !s_fds[fd].used ||
        s_fds[fd].kind != IOS_FD_VWII_FILE)
        return IOS_IPC_EINVAL;

    int64_t base;
    switch (wii_read_u32(block + IPC_OFF_ARG1)) {
    case 0: base = 0; break;
    case 1: base = s_fds[fd].position; break;
    case 2: base = s_fds[fd].size; break;
    default: return IOS_IPC_EINVAL;
    }
    int64_t target = base + (int32_t)wii_read_u32(block + IPC_OFF_ARG0);
    if (target < 0 || target > UINT32_MAX) return IOS_IPC_EINVAL;

    FSError error = FSASetPosFile(s_vwii_nand.client, s_fds[fd].vwii_file, (uint32_t)target);
    if (error != FS_ERROR_OK) return vwii_fsa_to_ios(error);
    s_fds[fd].position = (uint32_t)target;
    return (int32_t)target;
}

int32_t ios_ipc_dispatch(uint32_t block) {
    if (!wii_mem_range(block, 0x20)) return IOS_IPC_EINVAL;

    int32_t rc;
    switch (wii_read_u32(block + IPC_OFF_CMD)) {
    case IOS_CMD_OPEN:   rc = do_open(block);   break;
    case IOS_CMD_CLOSE:  rc = do_close(block);  break;
    case IOS_CMD_READ:   rc = do_read(block);   break;
    case IOS_CMD_WRITE:  rc = do_write(block);  break;
    case IOS_CMD_SEEK:   rc = do_seek(block);   break;
    case IOS_CMD_IOCTL:  rc = do_ioctl(block);  break;
    case IOS_CMD_IOCTLV: rc = do_ioctlv(block); break;
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
#define ST_IOS_TMD 0x90201000u

static void build_open(uint32_t path_ea, int32_t mode) {
    wii_write_u32(ST_BLOCK + IPC_OFF_CMD, IOS_CMD_OPEN);
    wii_write_u32(ST_BLOCK + IPC_OFF_ARG0, path_ea);
    wii_write_u32(ST_BLOCK + IPC_OFF_ARG1, (uint32_t)mode);
}

static int32_t open_dev(const char *path) {
    uint32_t path_ea = ST_BUF;
    wii_mem_write(path_ea, path, (uint32_t)strlen(path) + 1);
    build_open(path_ea, IOS_GUEST_OPEN_NONE);
    return ios_ipc_dispatch(ST_BLOCK);
}

bool ios_ipc_vwii_nand_selftest(void) {
    if (!s_vwii_nand.ready) return false;

    wii_mem_write(ST_BUF, s_vwii_nand.probe_path,
                  (uint32_t)strlen(s_vwii_nand.probe_path) + 1);
    build_open(ST_BUF, IOS_GUEST_OPEN_READ);
    int32_t fd = ios_ipc_dispatch(ST_BLOCK);
    if (fd < 0) {
        WHBLogPrintf("vwii nand backing: open %s failed (%d)", s_vwii_nand.probe_path, fd);
        return false;
    }

    wii_write_u32(ST_BLOCK + IPC_OFF_CMD, IOS_CMD_READ);
    wii_write_u32(ST_BLOCK + IPC_OFF_FD, (uint32_t)fd);
    wii_write_u32(ST_BLOCK + IPC_OFF_ARG0, ST_BUF + 0x80);
    wii_write_u32(ST_BLOCK + IPC_OFF_ARG1, 32);
    int32_t first = ios_ipc_dispatch(ST_BLOCK);

    wii_write_u32(ST_BLOCK + IPC_OFF_CMD, IOS_CMD_SEEK);
    wii_write_u32(ST_BLOCK + IPC_OFF_ARG0, 0);
    wii_write_u32(ST_BLOCK + IPC_OFF_ARG1, 0);
    int32_t seek = ios_ipc_dispatch(ST_BLOCK);

    wii_write_u32(ST_BLOCK + IPC_OFF_CMD, IOS_CMD_READ);
    wii_write_u32(ST_BLOCK + IPC_OFF_ARG0, ST_BUF + 0xC0);
    wii_write_u32(ST_BLOCK + IPC_OFF_ARG1, 32);
    int32_t second = ios_ipc_dispatch(ST_BLOCK);

    uint8_t first_bytes[32];
    uint8_t second_bytes[32];
    if (first > 0) wii_mem_read(first_bytes, ST_BUF + 0x80, (uint32_t)first);
    if (second > 0) wii_mem_read(second_bytes, ST_BUF + 0xC0, (uint32_t)second);

    wii_write_u32(ST_BLOCK + IPC_OFF_CMD, IOS_CMD_CLOSE);
    wii_write_u32(ST_BLOCK + IPC_OFF_FD, (uint32_t)fd);
    int32_t close = ios_ipc_dispatch(ST_BLOCK);

    const char *dir_path = strncmp(s_vwii_nand.probe_path, "/shared1/", 9) == 0 ?
                           "/shared1" : "/shared2/sys";
    int32_t fs_fd = open_dev("/dev/fs");
    int32_t read_dir = IOS_IPC_EINVAL;
    uint32_t entry_count = 0;
    int32_t fs_close = IOS_IPC_EINVAL;
    if (fs_fd >= 0) {
        wii_mem_write(ST_BUF, dir_path, (uint32_t)strlen(dir_path) + 1);
        wii_write_u32(ST_VECS + 0x00, ST_BUF);
        wii_write_u32(ST_VECS + 0x04, (uint32_t)strlen(dir_path) + 1);
        wii_write_u32(ST_VECS + 0x08, ST_BUF + 0x80);
        wii_write_u32(ST_VECS + 0x0C, 4);
        wii_write_u32(ST_BLOCK + IPC_OFF_CMD, IOS_CMD_IOCTLV);
        wii_write_u32(ST_BLOCK + IPC_OFF_FD, (uint32_t)fs_fd);
        wii_write_u32(ST_BLOCK + IPC_OFF_ARG0, ISFS_IOCTLV_READDIR);
        wii_write_u32(ST_BLOCK + IPC_OFF_ARG1, 1);
        wii_write_u32(ST_BLOCK + IPC_OFF_ARG2, 1);
        wii_write_u32(ST_BLOCK + IPC_OFF_ARG3, ST_VECS);
        read_dir = ios_ipc_dispatch(ST_BLOCK);
        entry_count = wii_read_u32(ST_BUF + 0x80);
        wii_write_u32(ST_BLOCK + IPC_OFF_CMD, IOS_CMD_CLOSE);
        wii_write_u32(ST_BLOCK + IPC_OFF_FD, (uint32_t)fs_fd);
        fs_close = ios_ipc_dispatch(ST_BLOCK);
    }

    bool ok = first >= 0 && seek == 0 && second == first && close == IOS_IPC_SUCCESS &&
              (first == 0 || memcmp(first_bytes, second_bytes, (size_t)first) == 0) &&
              read_dir == IOS_IPC_SUCCESS && entry_count > 0 && fs_close == IOS_IPC_SUCCESS;
    if (!ok)
        WHBLogPrintf("vwii nand backing: read=%d seek=%d reread=%d close=%d readdir=%d entries=%u fsclose=%d",
                     first, seek, second, close, read_dir, entry_count, fs_close);
    return ok;
}

IosIpcSelfTestResult ios_ipc_selftest(void) {
    ios_ipc_init();
    bool ok = true;
    const uint32_t prior_ios_version = wii_read_u32(IOS_VERSION_LOWMEM);

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

        const uint64_t ios58_title = 0x000000010000003AULL;
        const uint64_t invalid_ios_title = 0x00000001000000A7ULL;
        const uint32_t count_ea = ST_BUF + 0x10;
        const uint32_t view_ea = ST_BUF + 0x100;
        const uint32_t tmd_ea = ST_IOS_TMD;

        wii_write_u64(ST_BUF, ios58_title);
        wii_write_u32(ST_VECS + 0x00, ST_BUF);      wii_write_u32(ST_VECS + 0x04, 8);
        wii_write_u32(ST_VECS + 0x08, count_ea);    wii_write_u32(ST_VECS + 0x0C, 4);
        wii_write_u32(ST_BLOCK + IPC_OFF_CMD, IOS_CMD_IOCTLV);
        wii_write_u32(ST_BLOCK + IPC_OFF_FD, (uint32_t)es_fd);
        wii_write_u32(ST_BLOCK + IPC_OFF_ARG0, IOCTL_ES_GETVIEWCNT);
        wii_write_u32(ST_BLOCK + IPC_OFF_ARG1, 1);
        wii_write_u32(ST_BLOCK + IPC_OFF_ARG2, 1);
        wii_write_u32(ST_BLOCK + IPC_OFF_ARG3, ST_VECS);
        rc = ios_ipc_dispatch(ST_BLOCK);
        if (rc != IOS_IPC_SUCCESS || wii_read_u32(count_ea) != 1) {
            WHBLogPrintf("ios_ipc: IOS58 view count wrong (rc=%d count=%u)",
                         rc, wii_read_u32(count_ea));
            ok = false;
        }

        wii_write_u32(ST_VECS + 0x00, ST_BUF);      wii_write_u32(ST_VECS + 0x04, 8);
        wii_write_u32(ST_VECS + 0x08, count_ea);    wii_write_u32(ST_VECS + 0x0C, 4);
        wii_write_u32(ST_VECS + 0x10, view_ea);
        wii_write_u32(ST_VECS + 0x14, IOS_TICKET_VIEW_SIZE);
        wii_write_u32(ST_BLOCK + IPC_OFF_ARG0, IOCTL_ES_GETVIEWS);
        wii_write_u32(ST_BLOCK + IPC_OFF_ARG1, 2);
        wii_write_u32(ST_BLOCK + IPC_OFF_ARG2, 1);
        rc = ios_ipc_dispatch(ST_BLOCK);
        if (rc != IOS_IPC_SUCCESS ||
            wii_read_u64(view_ea + IOS_TICKET_VIEW_TITLE_ID_OFFSET) != ios58_title) {
            WHBLogPrintf("ios_ipc: IOS58 view wrong (rc=%d title=0x%016llX)", rc,
                         (unsigned long long)wii_read_u64(view_ea + IOS_TICKET_VIEW_TITLE_ID_OFFSET));
            ok = false;
        }

        wii_write_u32(ST_VECS + 0x00, ST_BUF);      wii_write_u32(ST_VECS + 0x04, 8);
        wii_write_u32(ST_VECS + 0x08, view_ea);
        wii_write_u32(ST_VECS + 0x0C, IOS_TICKET_VIEW_SIZE);
        wii_write_u32(ST_BLOCK + IPC_OFF_ARG0, IOCTL_ES_LAUNCH);
        wii_write_u32(ST_BLOCK + IPC_OFF_ARG1, 2);
        wii_write_u32(ST_BLOCK + IPC_OFF_ARG2, 0);
        rc = ios_ipc_dispatch(ST_BLOCK);
        if (rc != IOS_IPC_SUCCESS || wii_read_u32(IOS_VERSION_LOWMEM) != 58) {
            WHBLogPrintf("ios_ipc: IOS58 launch wrong (rc=%d version=%u)", rc,
                         wii_read_u32(IOS_VERSION_LOWMEM));
            ok = false;
        }

        wii_write_u32(ST_VECS + 0x00, ST_BUF);      wii_write_u32(ST_VECS + 0x04, 8);
        wii_write_u32(ST_VECS + 0x08, count_ea);    wii_write_u32(ST_VECS + 0x0C, 4);
        wii_write_u32(ST_BLOCK + IPC_OFF_ARG0, IOCTL_ES_GETSTOREDTMDSIZE);
        wii_write_u32(ST_BLOCK + IPC_OFF_ARG1, 1);
        wii_write_u32(ST_BLOCK + IPC_OFF_ARG2, 1);
        rc = ios_ipc_dispatch(ST_BLOCK);
        if (rc != IOS_IPC_SUCCESS || wii_read_u32(count_ea) != IOS_TMD_SIZE) {
            WHBLogPrintf("ios_ipc: IOS58 TMD size wrong (rc=%d size=%u)",
                         rc, wii_read_u32(count_ea));
            ok = false;
        }

        wii_write_u32(ST_VECS + 0x00, ST_BUF);      wii_write_u32(ST_VECS + 0x04, 8);
        wii_write_u32(ST_VECS + 0x08, count_ea);    wii_write_u32(ST_VECS + 0x0C, 4);
        wii_write_u32(ST_VECS + 0x10, tmd_ea);
        wii_write_u32(ST_VECS + 0x14, IOS_TMD_SIZE);
        wii_write_u32(ST_BLOCK + IPC_OFF_ARG0, IOCTL_ES_GETSTOREDTMD);
        wii_write_u32(ST_BLOCK + IPC_OFF_ARG1, 2);
        wii_write_u32(ST_BLOCK + IPC_OFF_ARG2, 1);
        rc = ios_ipc_dispatch(ST_BLOCK);
        EsTmdInfo tmd_info;
        if (rc != IOS_IPC_SUCCESS || !es_tmd_parse(wii_mem_ptr(tmd_ea), IOS_TMD_SIZE, &tmd_info) ||
            tmd_info.title_id != ios58_title) {
            WHBLogPrintf("ios_ipc: IOS58 TMD wrong (rc=%d)", rc);
            ok = false;
        }

        wii_write_u64(ST_BUF, invalid_ios_title);
        wii_write_u32(ST_BLOCK + IPC_OFF_ARG0, IOCTL_ES_GETVIEWCNT);
        wii_write_u32(ST_BLOCK + IPC_OFF_ARG1, 1);
        wii_write_u32(ST_BLOCK + IPC_OFF_ARG2, 1);
        rc = ios_ipc_dispatch(ST_BLOCK);
        if (rc != IOS_IPC_ENOENT) {
            WHBLogPrintf("ios_ipc: invalid IOS view count wrong (rc=%d)", rc);
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

    wii_write_u32(IOS_VERSION_LOWMEM, prior_ios_version);
    return ok ? IOS_IPC_SELFTEST_PASS : IOS_IPC_SELFTEST_FAIL;
}
