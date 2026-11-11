/*
 * Copyright (C) 2026 Ramiro Polla
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

#ifndef SWSCALE_AARCH64_UOPS_AARCH64_H
#define SWSCALE_AARCH64_UOPS_AARCH64_H

/**
 * Uop rules of the aarch64 backend, shared with the tools that generate its
 * kernel list. This must not depend on the host architecture.
 */

#include "libavutil/error.h"

#include "../uops.h"

static inline int test_uop_aarch64(const SwsUOp *uop, unsigned cpu_flags)
{
    switch (uop->uop) {
    case SWS_UOP_READ_PLANAR_FH:
    case SWS_UOP_READ_PLANAR_FV:
    case SWS_UOP_READ_PLANAR_FV_FMA:
    case SWS_UOP_READ_PALETTE:
    case SWS_UOP_EXPAND_BIT:
    case SWS_UOP_RW_SHUFFLE:
    case SWS_UOP_LUT_3D:
        return AVERROR(ENOTSUP);
    default:
        break;
    }

    return 0;
}

/* Use at most two full vregs during the widest precision section */
static inline int ff_sws_uop_block_size_aarch64(const SwsUOpList *uops)
{
    return uops->pixel_size_max == 4 ? 8 : 16;
}

/**
 * Rewrite a uop into the form of the kernel implementing it. uop->block_size
 * must be set to the block size of the list, and is adjusted to match the
 * kernel. Parameters which make no difference to the generated code are
 * normalized, to prevent identical CPS functions from being instantiated
 * multiple times under different names.
 */
static inline void ff_sws_uop_simplify_aarch64(SwsUOp *uop)
{
    switch (uop->uop) {
    case SWS_UOP_PERMUTE:
    case SWS_UOP_COPY:
        /* Recompute mask taking identity swizzle into account */
        uop->mask = 0;
        for (int i = 0; i < uop->par.move.num_moves; i++) {
            int dst = uop->par.move.dst[i];
            if (dst >= 0)
                uop->mask |= SWS_COMP(dst);
        }

        /* The element size and type don't matter. */
        uop->block_size *= ff_sws_pixel_type_size(uop->type);
        uop->type = SWS_PIXEL_U8;
        break;
    case SWS_UOP_LINEAR_FMA:
        /* par.lin.exact is currently unused by asmgen_op_linear(). */
        uop->par.lin.exact = 0;
        break;
    default:
        break;
    }
}

/* SwsOpBackend.resolve_uops of the aarch64 backend */
static inline int ff_sws_uop_list_resolve_aarch64(SwsUOpList *uops,
                                                  unsigned cpu_flags,
                                                  int block_size)
{
    const int size = ff_sws_uop_block_size_aarch64(uops);
    if (block_size && block_size != size)
        return AVERROR(ENOTSUP);

    for (int i = 0; i < uops->num_ops; i++) {
        SwsUOp *uop = &uops->ops[i];
        if (!ff_sws_uop_supported(test_uop_aarch64, 0, uop))
            return AVERROR(ENOTSUP);
        uop->block_size = size;
        ff_sws_uop_simplify_aarch64(uop);
    }

    uops->block_size = size;
    return 0;
}

#endif /* SWSCALE_AARCH64_UOPS_AARCH64_H */
