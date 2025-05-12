/*
 * Copyright (c) 2025 Ramiro Polla
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef SWSCALE_MEMOPS_H
#define SWSCALE_MEMOPS_H

#include <stddef.h>
#include <stdint.h>

typedef void (*memset16_func)(uint16_t *dst, uint16_t val, size_t count);
typedef void (*memset32_func)(uint32_t *dst, uint32_t val, size_t count);
typedef void (*bswap16_func)(uint16_t *restrict dst, const uint16_t *restrict src, size_t count);
typedef void (*bswap32_func)(uint32_t *restrict dst, const uint32_t *restrict src, size_t count);
typedef void (*lshift16_func)(uint16_t *restrict dst, const uint16_t *restrict src, size_t count, int shift);
typedef void (*lshift32_func)(uint32_t *restrict dst, const uint32_t *restrict src, size_t count, int shift);

typedef struct MemOpsContext {
    memset16_func memset16;
    memset32_func memset32;
    bswap16_func bswap16;
    bswap32_func bswap32;
    lshift16_func lshift16;
    lshift32_func lshift32;
} MemOpsContext;

void ff_memops_init(MemOpsContext *c);

#endif /* SWSCALE_MEMOPS_H */
