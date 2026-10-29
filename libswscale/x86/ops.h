/**
 * Copyright (C) 2026 Niklas Haas
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

#ifndef SWSCALE_X86_OPS_H
#define SWSCALE_X86_OPS_H

/**
 * Note: This header is also used by uops_macros_gen on all architectures,
 * so it must not depend on any x86-specific code or build configuration.
 */

#include "libavutil/cpu.h"
#include "libavutil/error.h"
#include "libavutil/macros.h"

#include "../uops.h"

static inline int get_mmsize(unsigned cpu_flags)
{
    if (cpu_flags & AV_CPU_FLAG_AVX512)
        return 64;
    else if (cpu_flags & AV_CPU_FLAG_AVX2)
        return 32;
    else if (cpu_flags & AV_CPU_FLAG_SSE4)
        return 16;
    else
        return AVERROR(ENOTSUP);
}

static inline int test_uop_x86(const SwsUOp *uop, unsigned cpu_flags,
                               SwsUOpLimits *limits)
{
    switch (uop->uop) {
    case SWS_UOP_TO_U32:
    case SWS_UOP_TO_F32:
        /* Conversions between U32 and F32 are not implemented */
        if (uop->type == SWS_PIXEL_U32 || uop->type == SWS_PIXEL_F32)
            return AVERROR(ENOTSUP);
        break;
    case SWS_UOP_LUT_3D:
        return AVERROR(ENOTSUP);
    case SWS_UOP_READ_PLANAR:
    case SWS_UOP_WRITE_PLANAR:
    case SWS_UOP_CLEAR:
        /* Only implemented for U8, other types are executed as U8 */
        limits->type = SWS_PIXEL_U8;
        break;
    case SWS_UOP_RW_SHUFFLE: {
        /* We can't shuffle across lanes, so restrict the vector size to XMM
         * whenever the read/write size would be a subset of the full vector,
         * unless we have access to AVX-512 ICL vpermb */
        const SwsShuffleUOp *par = &uop->par.shuffle;
        const bool lane_aligned = par->read_size == par->write_size &&
                                  16 % par->read_size == 0;
        int bytes = get_mmsize(cpu_flags);
        if (bytes < 0)
            return bytes;
        if (!lane_aligned && !(cpu_flags & AV_CPU_FLAG_AVX512ICL))
            bytes = FFMIN(bytes, 16);
        limits->min_bytes = limits->max_bytes = bytes;
        return 0;
    }
    default:
        break;
    }

    int pixel_size = ff_sws_uop_pixel_size_max(uop);
    if (limits->type)
        pixel_size = ff_sws_pixel_type_size(limits->type);

    const int mmsize = get_mmsize(cpu_flags);
    if (mmsize < 0)
        return mmsize;
    else if (mmsize == 16 && pixel_size > 1)
        return AVERROR(ENOTSUP); /* Only 8-bit kernels are implemented for SSE4 */

    /**
     * Use at most two full YMM registers per component for the largest pixel
     * type. All kernels process at least 16 pixels at a time.
     */
    limits->min_bytes = 16 * pixel_size;
    limits->max_bytes = 2 * FFMIN(mmsize, 32);
    return 0;
}

#endif /* SWSCALE_X86_OPS_H */
