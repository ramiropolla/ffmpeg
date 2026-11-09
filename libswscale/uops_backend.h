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

#include "uops.h"

#define SWS_BLOCK_SIZE 32

static inline SwsUOpFlags ff_sws_uop_flags_c(void)
{
    return SWS_UOP_FLAG_EXPAND_BIT
         | SWS_UOP_FLAG_READ_PALETTE
         | SWS_UOP_FLAG_ADD
         | SWS_UOP_FLAG_PUNNING;
}

#endif /* SWSCALE_UOPS_BACKEND_H */
