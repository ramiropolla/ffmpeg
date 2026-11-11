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

#include <limits.h>

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

/**
 * Vector size in bytes of the kernel implementing a standalone
 * SWS_UOP_RW_SHUFFLE, or a negative error code.
 */
static inline int ff_sws_uop_shuffle_size_x86(unsigned cpu_flags,
                                              const SwsUOp *uop)
{
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
    return bytes;
}

static inline int test_uop_x86(const SwsUOp *uop, unsigned cpu_flags)
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
    case SWS_UOP_RW_SHUFFLE: {
        const int bytes = ff_sws_uop_shuffle_size_x86(cpu_flags, uop);
        return bytes < 0 ? bytes : 0;
    }
    default:
        break;
    }

    const int mmsize = get_mmsize(cpu_flags);
    return mmsize < 0 ? mmsize : 0;
}

/**
 * Largest block size of a list. Uses at most two full YMM registers per
 * component for the largest pixel type of each uop.
 */
static inline int ff_sws_uop_block_size_x86(unsigned cpu_flags,
                                            const SwsUOpList *uops)
{
    const int mmsize = get_mmsize(cpu_flags);
    if (mmsize < 0)
        return mmsize;

    int block_size = INT_MAX;
    for (int i = 0; i < uops->num_ops; i++) {
        const int pixel_size = ff_sws_uop_pixel_size_max(&uops->ops[i]);
        block_size = FFMIN(block_size, 2 * FFMIN(mmsize, 32) / pixel_size);
    }
    return block_size;
}

/**
 * Rewrite a uop into the form of the kernel implementing it. uop->block_size
 * must be set to the block size of the list, and is adjusted to match the
 * kernel.
 *
 * Returns 0, or AVERROR(ENOTSUP) if no kernel has that form.
 */
static inline int ff_sws_uop_simplify_x86(unsigned cpu_flags, SwsUOp *uop)
{
    switch (uop->uop) {
    case SWS_UOP_READ_PLANAR:
    case SWS_UOP_WRITE_PLANAR:
    case SWS_UOP_CLEAR:
        /* Only implemented for U8, other types are executed as U8 */
        uop->block_size *= ff_sws_pixel_type_size(uop->type);
        uop->type = SWS_PIXEL_U8;
        break;
    default:
        break;
    }

    /* Only 8-bit kernels are implemented for SSE4 */
    if (get_mmsize(cpu_flags) == 16 && ff_sws_uop_pixel_size_max(uop) > 1)
        return AVERROR(ENOTSUP);
    /* All kernels process at least 16 elements at a time */
    if (uop->block_size < 16)
        return AVERROR(ENOTSUP);
    return 0;
}

/**
 * SwsOpBackend.resolve_uops of the x86 backend, for CPUs supporting
 * `cpu_flags`. A list block size of 0 picks the largest.
 */
static inline int ff_sws_uop_list_resolve_x86(SwsUOpList *uops,
                                              unsigned cpu_flags,
                                              int block_size)
{
    if (uops->num_ops == 1 && uops->ops[0].uop == SWS_UOP_RW_SHUFFLE) {
        /* The number of pixels is fixed by the shuffle pattern */
        SwsUOp *uop = &uops->ops[0];
        const SwsShuffleUOp *par = &uop->par.shuffle;
        const int group_size = FFMAX(par->read_size, par->write_size);
        const int bytes = ff_sws_uop_shuffle_size_x86(cpu_flags, uop);
        if (bytes < 0)
            return bytes;

        const int pixels = bytes / group_size * uop->data.shuffle.pixels;
        if (block_size && block_size != pixels)
            return AVERROR(ENOTSUP);

        /* SWS_UOP_RW_SHUFFLE is a u8 uop, so its block size is in bytes */
        uop->block_size = bytes;
        uops->block_size = pixels;
        return 0;
    }

    const int max = ff_sws_uop_block_size_x86(cpu_flags, uops);
    if (max < 0)
        return max;
    if (!block_size)
        block_size = max;
    else if (block_size > max)
        return AVERROR(ENOTSUP);

    for (int i = 0; i < uops->num_ops; i++) {
        SwsUOp *uop = &uops->ops[i];
        if (!ff_sws_uop_supported(test_uop_x86, cpu_flags, uop))
            return AVERROR(ENOTSUP);
        uop->block_size = block_size;
        int ret = ff_sws_uop_simplify_x86(cpu_flags, uop);
        if (ret < 0)
            return ret;
    }

    uops->block_size = block_size;
    return 0;
}

#endif /* SWSCALE_X86_OPS_H */
