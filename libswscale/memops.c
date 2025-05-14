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

#include "memops.h"

#include "libavutil/bswap.h"

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

static void memswap16_c(uint16_t *dst, uint16_t *src, size_t count)
{
    for (size_t i = 0; i < count; i++)
        dst[i] = av_bswap16(src[i]);
}

static void memswap32_c(uint32_t *dst, uint32_t *src, size_t count)
{
    for (size_t i = 0; i < count; i++)
        dst[i] = av_bswap32(src[i]);
}

av_cold void ff_memops_init(MemOpsContext *c)
{
    c->memset16  = memset16_c;
    c->memset32  = memset32_c;
    c->memswap16 = memswap16_c;
    c->memswap32 = memswap32_c;
#if ARCH_AARCH64
    ff_memops_init_aarch64(c);
#endif
}
