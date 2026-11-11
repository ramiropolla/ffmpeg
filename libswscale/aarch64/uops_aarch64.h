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

#ifndef SWSCALE_AARCH64_UOPS_AARCH64_H
#define SWSCALE_AARCH64_UOPS_AARCH64_H

/**
 * Translation policy of the aarch64 backend, shared with the uop list
 * generator. This must not depend on the host architecture.
 */

#include <stdbool.h>

#include "../uops.h"

static inline SwsUOpFlags ff_sws_uop_flags_aarch64(bool bitexact)
{
    return SWS_UOP_FLAG_PUNNING | SWS_UOP_FLAG_ADD |
           (bitexact ? 0 : SWS_UOP_FLAG_FMA);
}

#endif /* SWSCALE_AARCH64_UOPS_AARCH64_H */
