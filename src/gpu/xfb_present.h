// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef RISTRETTO_GPU_XFB_PRESENT_H
#define RISTRETTO_GPU_XFB_PRESENT_H

#include <stdbool.h>
#include <stdint.h>

// Wii external-framebuffer (XFB) presentation helpers.
void xfb_yuv422_to_rgba(const uint8_t *yuv, uint32_t width, uint32_t height,
                        uint32_t src_pitch, uint8_t *rgba, uint32_t dst_pitch);

// Encode paired RGBA8 pixels as the XFB's YUYV layout.
bool xfb_rgba_to_yuv422(const uint8_t *rgba, uint32_t width, uint32_t height,
                        uint32_t src_pitch, uint8_t *yuv, uint32_t dst_pitch);

// Downscale an RGBA8 image by two in each dimension with an edge-clamped box
// filter.
bool xfb_rgba8_box_filter_2x(const uint8_t *src, uint32_t width, uint32_t height,
                             uint32_t src_pitch, uint8_t *dst, uint32_t dst_pitch);

// Self test.
bool xfb_present_selftest(void);

#endif  // RISTRETTO_GPU_XFB_PRESENT_H
