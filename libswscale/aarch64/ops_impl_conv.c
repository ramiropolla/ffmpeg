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

#include <string.h>

#include "libavutil/error.h"
#include "libavutil/macros.h"

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
 * Structured load/store variant for SWS_UOP_RW_SHUFFLE: if every output
 * pixel consists of whole elements (of 1, 2, or 4 bytes) of its input pixel
 * or of cleared elements, with at most 4 elements per input and output
 * pixel, then the shuffle can be performed with ldN and stN. This is only
 * accepted if the register lists of ldN and stN can line up without any
 * moves in between (see shuffle_ldst_offset()), so that the cleared
 * elements can be set once outside of the loop.
 *
 * Returns the element type, or SWS_PIXEL_NONE if the variant does not
 * apply.
 */
static SwsPixelType shuffle_ldst(const SwsUOp *uop, SwsAArch64ShuffleLdSt *ldst)
{
    static const SwsPixelType types[] = { SWS_PIXEL_U32, SWS_PIXEL_U16, SWS_PIXEL_U8 };
    const SwsShuffleUOp *par = &uop->par.shuffle;
    const SwsShuffleMask *data = &uop->data.shuffle;
    if (!data->pixels || par->read_size % data->pixels || par->write_size % data->pixels)
        return SWS_PIXEL_NONE;
    const int in_size  = par->read_size  / data->pixels;
    const int out_size = par->write_size / data->pixels;

    for (int t = 0; t < FF_ARRAY_ELEMS(types); t++) {
        const int size = ff_sws_pixel_type_size(types[t]);
        if (in_size % size || out_size % size ||
            in_size > 4 * size || out_size > 4 * size)
            continue;

        *ldst = (SwsAArch64ShuffleLdSt) {
            .num_in  = in_size  / size,
            .num_out = out_size / size,
        };
        bool ok = true;
        for (int k = 0; k < ldst->num_out && ok; k++) {
            const int first = data->mask[k * size];
            ldst->src[k] = first < 0 ? -1 : first / size;
            if (first >= 0 && first % size)
                ok = false;
            for (int p = 0; p < data->pixels && ok; p++) {
                for (int i = 0; i < size && ok; i++) {
                    const int m = data->mask[p * out_size + k * size + i];
                    if (first < 0)
                        ok = m < 0;
                    else
                        ok = m == p * in_size + first + i;
                }
            }
        }
        if (!ok)
            continue;

        const int offset = shuffle_ldst_offset(ldst);
        for (int k = 0; k < ldst->num_out && ok; k++) {
            const int j = k - offset;
            if (ldst->src[k] >= 0)
                ok = ldst->src[k] == j;
            else
                ok = j < 0 || j >= ldst->num_in;
        }
        if (ok)
            return types[t];
    }

    return SWS_PIXEL_NONE;
}

/**
 * Convert SWS_UOP_RW_SHUFFLE to the SwsAArch64OpImplParams of its structured
 * load/store variant, if possible (see shuffle_ldst()). The clear value is
 * loaded from impl->priv, so the code only depends on whether there are
 * cleared elements, and whether they are cleared to a non-zero value.
 */
static bool convert_to_aarch64_ldst(const SwsUOp *uop, SwsAArch64OpImplParams *out)
{
    SwsAArch64ShuffleLdSt ldst;
    const SwsPixelType type = shuffle_ldst(uop, &ldst);
    if (type == SWS_PIXEL_NONE)
        return false;

    bool clear = false;
    for (int k = 0; k < ldst.num_out; k++)
        if (ldst.src[k] < 0)
            clear = true;

    /* aarch64_lookup() compares the whole structure, including padding. */
    memset(out, 0, sizeof(*out));
    out->uop  = SWS_UOP_RW_SHUFFLE;
    out->mask = uop->mask;
    out->type = type;
    out->par.shuffle.clear_value = clear && uop->par.shuffle.clear_value;
    out->ldst = ldst;
    return true;
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
