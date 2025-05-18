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

#include "config.h"

#include "memops.h"

#include "libavutil/bswap.h"

static void bswap16_c(uint16_t *restrict dst, const uint16_t *restrict src, size_t count)
{
    for (size_t i = 0; i < count; i++)
        dst[i] = av_bswap16(src[i]);
}

static void bswap32_c(uint32_t *restrict dst, const uint32_t *restrict src, size_t count)
{
    for (size_t i = 0; i < count; i++)
        dst[i] = av_bswap32(src[i]);
}

static void lshift16_c(uint16_t *restrict dst, const uint16_t *restrict src, size_t count, int shift)
{
    for (size_t i = 0; i < count; i++)
        dst[i] = src[i] << shift;
}

static void lshift32_c(uint32_t *restrict dst, const uint32_t *restrict src, size_t count, int shift)
{
    for (size_t i = 0; i < count; i++)
        dst[i] = src[i] << shift;
}

static void memset16_c(uint16_t *dst, uint16_t val, size_t count)
{
    for (size_t i = 0; i < count; i++)
        dst[i] = val;
}

static void memset32_c(uint32_t *dst, uint32_t val, size_t count)
{
    for (size_t i = 0; i < count; i++)
        dst[i] = val;
}

av_cold void ff_memops_init(MemOpsContext *c)
{
    c->bswap16 = bswap16_c;
    c->bswap32 = bswap32_c;
    c->lshift16 = lshift16_c;
    c->lshift32 = lshift32_c;
    c->memset16 = memset16_c;
    c->memset32 = memset32_c;
#if ARCH_AARCH64 && HAVE_NEON
    ff_memops_init_aarch64(c);
#endif
}
