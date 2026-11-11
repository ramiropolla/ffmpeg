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

/**
 * NOTE: This file is #include'd directly by both the NEON backend and
 *       the sws_ops_aarch64 tool.
 */

#include "libavutil/error.h"

#include "ops_impl.h"
#include "uops_aarch64.h"

/**
 * Convert a SwsUOp simplified by ff_sws_uop_simplify_aarch64() to a
 * SwsAArch64OpImplParams. Read the comments regarding
 * SwsAArch64OpImplParams in ops_impl.h for more information.
 */
static void convert_to_aarch64_impl(const SwsUOp *uop,
                                    SwsAArch64OpImplParams *out)
{
    out->uop = uop->uop;
    out->mask = uop->mask;
    out->type = uop->type;
    out->block_size = uop->block_size;
    out->par = uop->par;
}
