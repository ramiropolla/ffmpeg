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
    default:
        break;
    }

    return 0;
}

#endif /* SWSCALE_X86_OPS_H */
