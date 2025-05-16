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

#include "libavutil/aarch64/cpu.h"
#include "libswscale/memops.h"

void ff_memset16_neon(uint16_t *dst, uint16_t val, size_t count);
void ff_memset32_neon(uint32_t *dst, uint32_t val, size_t count);
void ff_memswap16_neon(uint16_t *restrict dst, const uint16_t *restrict src, size_t count);
void ff_memswap32_neon(uint32_t *restrict dst, const uint32_t *restrict src, size_t count);
void ff_memlshift16_neon(uint16_t *restrict dst, const uint16_t *restrict src, size_t count, int shift);
void ff_memlshift32_neon(uint32_t *restrict dst, const uint32_t *restrict src, size_t count, int shift);

void ff_memops_init_aarch64(MemOpsContext *c)
{
    int cpu_flags = av_get_cpu_flags();

    if (have_neon(cpu_flags)) {
        c->memset16 = ff_memset16_neon;
        c->memset32 = ff_memset32_neon;
        c->memswap16 = ff_memswap16_neon;
        c->memswap32 = ff_memswap32_neon;
        c->memlshift16 = ff_memlshift16_neon;
        c->memlshift32 = ff_memlshift32_neon;
    }
}
