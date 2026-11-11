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

static inline int test_uop_aarch64(const SwsUOp *uop, unsigned cpu_flags,
                                   SwsUOpLimits *limits)
{
    switch (uop->uop) {
    case SWS_UOP_READ_PALETTE:
    case SWS_UOP_EXPAND_BIT:
    case SWS_UOP_RW_SHUFFLE:
        return AVERROR(ENOTSUP);
    default:
        break;
    }

    return 0;
}

#endif /* SWSCALE_AARCH64_UOPS_AARCH64_H */
