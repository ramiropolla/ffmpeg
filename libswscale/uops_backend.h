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
 * Translation policy of the C backend, shared with the uop list generator.
 */

#include "libavutil/attributes.h"

#include "uops.h"

#define SWS_BLOCK_SIZE 32

static inline SwsUOpFlags ff_sws_uop_flags_c(void)
{
    return SWS_UOP_FLAG_PUNNING;
}

static inline SwsUOpFlags c_policy_uop_flags(const SwsUOpTarget *target)
{
    return ff_sws_uop_flags_c();
}

static inline bool c_policy_supported(const SwsUOpTarget *target,
                                      const SwsUOp *uop)
{
    return true;
}

static inline int c_policy_block_size(const SwsUOpTarget *target,
                                      const SwsUOpList *uops)
{
    return SWS_BLOCK_SIZE;
}

static inline int c_policy_simplify(const SwsUOpTarget *target, SwsUOp *uop)
{
    return 0;
}

static inline void c_policy_name(const SwsUOp *uop, int list,
                                 char buf[SWS_UOP_NAME_MAX])
{
    ff_sws_uop_name(uop, buf);
}

av_unused static const SwsUOpPolicy ff_sws_uop_policy_c = {
    .uop_flags  = c_policy_uop_flags,
    .supported  = c_policy_supported,
    .block_size = c_policy_block_size,
    .simplify   = c_policy_simplify,
    .name       = c_policy_name,
};

#endif /* SWSCALE_UOPS_BACKEND_H */
