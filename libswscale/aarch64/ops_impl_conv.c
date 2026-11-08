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

/**
 * Whether the kernel for `uop` supports 32-bit elements with block_size 16
 * (four vector banks). This is only implemented for the kernels commonly
 * used by floating point conversions.
 */
static bool uop_supports_x4(const SwsUOp *uop)
{
    switch (uop->uop) {
    case SWS_UOP_TO_F32:
        return uop->type != SWS_PIXEL_F32;
    case SWS_UOP_LINEAR_FMA: {
        /* Up to 3 inputs can be saved with 4 banks (12 temps). */
        if (uop->type != SWS_PIXEL_F32)
            return false;
        const SwsAArch64OpImplParams params = {
            .uop = uop->uop, .mask = uop->mask, .type = uop->type, .par = uop->par,
        };
        const SwsCompMask save_mask = linear_interleaved_save_mask(&params);
        int nsaved = 0;
        for (int i = 0; i < 4; i++)
            nsaved += !!(save_mask & SWS_COMP(i));
        return nsaved <= 3;
    }
    case SWS_UOP_LINEAR:
        /* Banks 2-3 are processed after banks 0-1 (see asmgen_op_linear). */
        return uop->type == SWS_PIXEL_F32;
    case SWS_UOP_READ_PLANAR:
    case SWS_UOP_WRITE_PLANAR:
    case SWS_UOP_SWAP_BYTES:
    case SWS_UOP_CLEAR:
    case SWS_UOP_UNPACK:
    case SWS_UOP_PACK:
        return true;
    case SWS_UOP_TO_U32:
        return uop->type != SWS_PIXEL_U32;
    case SWS_UOP_TO_U8:
    case SWS_UOP_TO_U16:
        /* from f32 or u32 */
        return ff_sws_pixel_type_size(uop->type) == 4;
    case SWS_UOP_MIN:
    case SWS_UOP_MAX:
    case SWS_UOP_SCALE:
    case SWS_UOP_SAT_U8:
    case SWS_UOP_SAT_U16:
        return uop->type == SWS_PIXEL_F32;
    case SWS_UOP_DITHER:
        /* The dither matrix must be at least as wide as the block. */
        return uop->type == SWS_PIXEL_F32 && uop->par.dither.size_log2 >= 4;
    case SWS_UOP_PERMUTE:
    case SWS_UOP_COPY:
        return true;
    default:
        return false;
    }
}

/**
 * Block size for a list of uops. Lists with 32-bit elements use block_size
 * 8 (two full vector registers per component), unless all kernels that
 * handle 32-bit elements support block_size 16.
 */
static int aarch64_block_size(const SwsUOpList *uops)
{
    if (uops->pixel_size_max != 4)
        return 16;
    for (int i = 0; i < uops->num_ops; i++) {
        const SwsUOp *uop = &uops->ops[i];
        const bool wide = ff_sws_pixel_type_size(uop->type) == 4 ||
                          uop->uop == SWS_UOP_TO_F32 || uop->uop == SWS_UOP_TO_U32;
        if (wide && !uop_supports_x4(uop))
            return 8;
    }
    return 16;
}

/**
 * Convert SwsUOp to a SwsAArch64OpImplParams. Read the comments regarding
 * SwsAArch64OpImplParams in ops_impl.h for more information.
 */
static void convert_to_aarch64_impl(const SwsUOp *uop, int block_size,
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
