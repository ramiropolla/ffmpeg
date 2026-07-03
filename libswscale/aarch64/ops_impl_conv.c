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
 * NOTE: This file is #include'd directly by the sws_ops_aarch64 tool,
 *       and also built as part of libswscale when targeting aarch64.
 */

#include "libavutil/error.h"

#include "ops_impl.h"

void ff_sws_aarch64_ops_translate(const SwsUOp *uop, int block_size,
                                  SwsAArch64OpImplParams *out)
{
    out->uop = uop->uop;
    out->mask = uop->mask;
    out->type = uop->type;
    out->block_size = block_size;
    out->par = uop->par;

    /**
     * Deduplicate params to prevent identical CPS functions from being
     * instantiated multiple times under different names.
     */
    switch (out->uop) {
    case SWS_UOP_PERMUTE:
    case SWS_UOP_COPY: {
        /* Recompute mask taking identity swizzle into account */
        out->mask = 0;
        for (int i = 0; i < out->par.move.num_moves; i++) {
            int dst = out->par.move.dst[i];
            if (dst >= 0)
                out->mask |= SWS_COMP(dst);
        }

        /* The element size and type don't matter. */
        out->block_size = block_size * ff_sws_pixel_type_size(out->type);
        out->type = SWS_PIXEL_U8;
        break;
    }
    case SWS_UOP_LINEAR_FMA:
        /* par.lin.exact is currently unused by asmgen_op_linear(). */
        out->par.lin.exact = 0;
        break;
    default:
        break;
    }
}
