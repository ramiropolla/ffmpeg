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

#ifndef SWSCALE_AARCH64_OPS_H
#define SWSCALE_AARCH64_OPS_H

#include "libswscale/ops.h"

/* Collect the parameters for all functions needed to implement this SwsOpList. */
int ff_sws_collect_ops_aarch64(SwsContext *ctx, void *opaque, SwsOpList *ops);

/* Serialize SwsAArch64OpImplParams for one function. */
int ff_sws_op_print_aarch64(void *opaque, void *elem);

#endif /* SWSCALE_AARCH64_OPS_H */
