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
 * Lane mode for SWS_UOP_RW_SHUFFLE: if the read and write sizes are equal
 * and the shuffle mask repeats every `period` bytes, with `period` dividing
 * both 16 and the read size, then each 16-byte vector register can be
 * shuffled independently with the same single-register tbl. This is the
 * case for byte swapping of formats with pixel sizes that do not divide 16
 * (e.g. rgb48be <-> rgb48le).
 *
 * Returns the number of bytes per iteration (a multiple of both 16 and the
 * read size), or 0 if the lane mode does not apply.
 */
static int shuffle_lane_bytes(const SwsUOp *uop, int *period)
{
    const SwsShuffleUOp *par = &uop->par.shuffle;
    const int8_t *mask = uop->data.shuffle.mask;
    const int size = par->read_size;
    if (par->read_size != par->write_size || size >= 16)
        return 0; /* only for R == W, and R == 16 already is a single lane */

    int p;
    for (p = 1; p < 16; p <<= 1) {
        if (size % p)
            continue;
        bool ok = true;
        for (int i = 0; i < size && ok; i++) {
            const int m  = mask[i];
            const int m0 = mask[i % p];
            if ((m < 0) != (m0 < 0))
                ok = false;
            else if (m >= 0 && (m0 >= p || m != m0 + i - i % p))
                ok = false;
        }
        if (ok)
            break;
    }
    if (p == 16)
        return 0;

    /**
     * Least common multiple of 16 and the read size, repeated up to 64
     * bytes (at most 4 vector registers are needed).
     */
    const int max_bytes = 64;
    int lcm = 16;
    while (lcm % size)
        lcm += 16;
    if (lcm > max_bytes)
        return 0;
    if (period)
        *period = p;
    return max_bytes / lcm * lcm;
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
    case SWS_UOP_RW_SHUFFLE:
        /**
         * The clear value is loaded from impl->priv, so the code only
         * depends on whether there is anything to clear to. The block
         * size field holds the number of bytes per iteration for the lane
         * mode (and is 0 otherwise, which uses SHUFFLE_MAX_BYTES). In lane
         * mode, each 16-byte vector register is its own group.
         */
        out->par.shuffle.clear_value = !!out->par.shuffle.clear_value;
        out->block_size = shuffle_lane_bytes(uop, NULL);
        if (out->block_size) {
            out->par.shuffle.read_size  = 16;
            out->par.shuffle.write_size = 16;
        }
        break;
    default:
        break;
    }
}
