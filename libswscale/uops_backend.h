/**
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

#ifndef SWSCALE_UOPS_BACKEND_H
#define SWSCALE_UOPS_BACKEND_H

/**
 * Uop rules of the C backend, shared with the uop list generator.
 */

#include "libavutil/error.h"

#include "uops.h"

#define SWS_BLOCK_SIZE 32

/* SwsOpBackend.resolve_uops of the C backend */
static inline int ff_sws_uop_list_resolve_c(SwsUOpList *uops,
                                            unsigned cpu_flags, int block_size)
{
    if (block_size && block_size != SWS_BLOCK_SIZE)
        return AVERROR(ENOTSUP);

    for (int i = 0; i < uops->num_ops; i++) {
        SwsUOp *uop = &uops->ops[i];
        if (!ff_sws_uop_supported(ff_sws_test_uop_c, cpu_flags, uop))
            return AVERROR(ENOTSUP);
        uop->block_size = SWS_BLOCK_SIZE;
    }

    uops->block_size = SWS_BLOCK_SIZE;
    return 0;
}

#endif /* SWSCALE_UOPS_BACKEND_H */
