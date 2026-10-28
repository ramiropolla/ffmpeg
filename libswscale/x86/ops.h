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

#include "../uops.h"

static inline int test_uop_x86(const SwsUOp *uop, unsigned cpu_flags,
                               SwsUOpLimits *limits)
{
    return 0;
}

#endif /* SWSCALE_X86_OPS_H */
