// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <whb/proc.h>
#include <whb/log.h>
#include <whb/log_console.h>
#include <whb/log_udp.h>

#include <coreinit/thread.h>

int main(int argc, char **argv) {
    WHBProcInit();
    WHBLogUdpInit();
    WHBLogConsoleInit();

    WHBLogConsoleSetColor(0x000030FF);
    WHBLogPrint("Ristretto");
    WHBLogPrint("A native Wii loader for Wii U");
    WHBLogPrint("");
    WHBLogPrint("Press HOME to exit.");
    WHBLogConsoleDraw();

    while (WHBProcIsRunning()) {
        OSSleepTicks(OSMillisecondsToTicks(100));
    }

    WHBLogConsoleFree();
    WHBLogUdpDeinit();
    WHBProcShutdown();
    return 0;
}
