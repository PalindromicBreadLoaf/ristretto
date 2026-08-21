# Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
# SPDX-License-Identifier: GPL-2.0-or-later

cmake_minimum_required(VERSION 3.13)

project(ristretto C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "CafeOS")
    message(FATAL_ERROR
        "Please configure with the devkitPro Wii U wrapper:\n"
        "  /opt/devkitpro/portlibs/wiiu/bin/powerpc-eabi-cmake -S . -B build")
endif()

add_executable(ristretto
    src/main.c
    src/boot/boot.c
    src/boot/dol.c
    src/cpu/cpu_exec.c
    src/cpu/ppc_decode.c
    src/cpu/ppc_interp.c
    src/cpu/ppc_xlate.c
    src/disc/aes.c
    src/disc/disc.c
    src/gpu/gx2_shader.c
    src/gpu/gx_fifo.c
    src/gpu/gx_state.c
    src/gpu/gx_texture.c
    src/gpu/r700_emit.c
    src/ios/ios_ipc.c
    src/mem/wii_memory.c
    src/mem/wii_mmio.c
)

target_include_directories(ristretto PRIVATE src)

target_link_libraries(ristretto PRIVATE wut)

wut_create_rpx(ristretto)

wut_create_wuhb(ristretto
    NAME      "Ristretto"
    SHORTNAME "Ristretto"
    AUTHOR    "PalindromicBreadLoaf"
)
