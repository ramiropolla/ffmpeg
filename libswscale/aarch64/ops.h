/*
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

#ifndef SWSCALE_AARCH64_OPS_H
#define SWSCALE_AARCH64_OPS_H

#include "libswscale/ops_chain.h"

#include "ops_impl.h"

int ff_sws_aarch64_setup(const SwsOpList *ops, int block_size, int n,
                         const SwsAArch64OpImplParams *p, SwsImplResult *out);

#endif /* SWSCALE_AARCH64_OPS_H */
