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
#include "libavutil/rational.h"
#include "libswscale/ops.h"

#include "ops_impl.h"

/**
 * The column index order for SwsLinearOp.mask follows the affine transform
 * order, where the offset is the last element. SwsAArch64LinearOpMask, on
 * the other hand, follows execution order, where the offset is the first
 * element.
 */
static int linear_index_from_sws_op(int idx)
{
    const int reorder_col[5] = { 1, 2, 3, 4, 0 };
    return reorder_col[idx];
}

static void swizzle_emit(SwsAArch64OpImplParams *out, uint8_t dst, uint8_t src, int idx)
{
    uint64_t pair = src | (dst << 4);
    out->move |= pair << (idx * 8);
    fprintf(stderr, " %d->%d", src == 4 ? -1 : src, dst == 4 ? -1 : dst);
}

static SwsCompMask sws_comp_mask_needed(const SwsOp *op)
{
    SwsCompMask mask = 0;
    for (int i = 0; i < 4; i++) {
        if (SWS_OP_NEEDED(op, i))
            mask |= SWS_COMP(i);
    }
    return mask;
}

static int count_idx(const int *arr, size_t size, int val)
{
    int num = 0;
    for (size_t i = 0; i < size; i++) {
        if (arr[i] == val)
            num++;
    }

    return num;
}

static void convert_swizzle_to_moves(const SwsOp *op, SwsAArch64OpImplParams *out)
{
    SwsAArch64OpMask swizzle = 0;
    int num_moves = 0;

    MASK_SET(swizzle, 0, op->swizzle.in[0]);
    MASK_SET(swizzle, 1, op->swizzle.in[1]);
    MASK_SET(swizzle, 2, op->swizzle.in[2]);
    MASK_SET(swizzle, 3, op->swizzle.in[3]);

fprintf(stderr, "{ %d %d %d %d } ->", op->swizzle.in[0], op->swizzle.in[1], op->swizzle.in[2], op->swizzle.in[3]);

#if 0
    /* Compute used vectors (src and dst) */
    uint8_t src_used[4] = { 0 };
    bool done[4] = { true, true, true, true };
    LOOP(out->mask, dst) {
        uint8_t src = MASK_GET(swizzle, dst);
        src_used[src]++;
        done[dst] = false;
    }

    /* First perform unobstructed copies. */
    for (bool progress = true; progress; ) {
        progress = false;
        for (int dst = 0; dst < 4; dst++) {
            if (done[dst] || src_used[dst])
                continue;
            uint8_t src = MASK_GET(swizzle, dst);
            swizzle_emit(out, dst, src, num_moves++);
            src_used[src]--;
            done[dst] = true;
            progress = true;
        }
    }

    /* Then swap and rotate remaining operations. */
    for (int dst = 0; dst < 4; dst++) {
        if (done[dst])
            continue;

        swizzle_emit(out, AARCH64_MOVE_TMP, dst, num_moves++);

        uint8_t cur_dst = dst;
        uint8_t src = MASK_GET(swizzle, cur_dst);
        while (src != dst) {
            swizzle_emit(out, cur_dst, src, num_moves++);
            done[cur_dst] = true;
            cur_dst = src;
            src = MASK_GET(swizzle, cur_dst);
        }

        swizzle_emit(out, cur_dst, AARCH64_MOVE_TMP, num_moves++);
        done[cur_dst] = true;
    }
#else
    /* Mask of components that are not yet satisfied */
    SwsCompMask todo = sws_comp_mask_needed(op);
    for (int i = 0; i < 4; i++) {
        if (op->swizzle.in[i] == i)
            todo &= ~SWS_COMP(i);
    }

    /* Mask of components whose value is required for the final output */
    SwsCompMask needed = 0;
    for (int i = 0; i < 4; i++) {
        if (SWS_OP_NEEDED(op, i))
            needed |= SWS_COMP(op->swizzle.in[i]);
    }

    /* Current mapping of registers to components */
    int idx[4 + 1] = { 0, 1, 2, 3, -1 }; /* +1 for tmp */

    /* Decompose the swizzle mask into a series of register-register moves */
    int last_src = 0;
    int last_dst = 0;
    while (todo) {
        int dst = -1, src = -1;

        /* Find next unsatisfied dst <- src move that doesn't clobber a value */
        for (dst = 0; dst < 4; dst++) {
            if (!SWS_COMP_TEST(todo, dst))
                continue; /* already satisfied */
            const int cur = idx[dst];
            if (count_idx(idx, FF_ARRAY_ELEMS(idx), cur) == 1 && SWS_COMP_TEST(needed, cur))
                continue; /* clobbers last remaining, still-needed value */
            for (src = 0; src < FF_ARRAY_ELEMS(idx); src++) {
                if (idx[src] == op->swizzle.in[dst]) {
                    /* Prevent read-after-write dependency. */
                    if (num_moves > 0 && src == last_dst)
                        src = last_src;
                    break;
                }
            }
            av_assert1(src < FF_ARRAY_ELEMS(idx));
            todo &= ~SWS_COMP(dst);
            break;
        }

        if (dst == 4) {
            /* Stuck in a cycle, break it by saving to the scratch register */
            dst = 4;
            for (src = 0; src < 4; src++) {
                if (SWS_COMP_TEST(todo, src)) {
                    needed &= ~SWS_COMP(idx[src]);
                    break;
                }
            }
            av_assert1(src < 4);
        }

        swizzle_emit(out, dst, src, num_moves++);
        last_src = src;
        last_dst = dst;
        idx[dst] = idx[src];
    }
#endif

fprintf(stderr, "\n");
}

/**
 * Convert SwsOp to a SwsAArch64OpImplParams. Read the comments regarding
 * SwsAArch64OpImplParams in ops_impl.h for more information.
 */
static int convert_to_aarch64_impl(SwsContext *ctx, const SwsOpList *ops, int n,
                                   int block_size, SwsAArch64OpImplParams *out)
{
    const SwsOp *op = &ops->ops[n];

    out->block_size = block_size;

    /**
     * Most SwsOp work on fields described by SWS_OP_NEEDED().
     * The few that don't will override this field later.
     */
    out->mask = 0;
    for (int i = 0; i < 4; i++) {
        if (SWS_OP_NEEDED(op, i))
            MASK_SET(out->mask, i, 1);
    }

    out->type = op->type;

    /* Map SwsOpType to SwsUOpType */
    switch (op->op) {
    case SWS_OP_READ:
        if (op->rw.filter.op)
            return AVERROR(ENOTSUP);
        /**
         * The different types of read operations have been split into
         * their own SwsUOpType to simplify the implementation.
         */
        if (op->rw.frac == 1)
            out->uop = SWS_UOP_READ_NIBBLE;
        else if (op->rw.frac == 3)
            out->uop = SWS_UOP_READ_BIT;
        else if (op->rw.mode == SWS_RW_PACKED)
            out->uop = SWS_UOP_READ_PACKED;
        else if (op->rw.mode == SWS_RW_PLANAR)
            out->uop = SWS_UOP_READ_PLANAR;
        else
            return AVERROR(ENOTSUP);
        break;
    case SWS_OP_WRITE:
        if (op->rw.filter.op)
            return AVERROR(ENOTSUP);
        /**
         * The different types of write operations have been split into
         * their own SwsUOpType to simplify the implementation.
         */
        if (op->rw.frac == 1)
            out->uop = SWS_UOP_WRITE_NIBBLE;
        else if (op->rw.frac == 3)
            out->uop = SWS_UOP_WRITE_BIT;
        else if (op->rw.mode == SWS_RW_PACKED)
            out->uop = SWS_UOP_WRITE_PACKED;
        else if (op->rw.mode == SWS_RW_PLANAR)
            out->uop = SWS_UOP_WRITE_PLANAR;
        else
            return AVERROR(ENOTSUP);
        break;
    case SWS_OP_SWAP_BYTES: out->uop = SWS_UOP_SWAP_BYTES; break;
    case SWS_OP_SWIZZLE:    out->uop = SWS_UOP_MOVE;       break;
    case SWS_OP_UNPACK:     out->uop = SWS_UOP_UNPACK;     break;
    case SWS_OP_PACK:       out->uop = SWS_UOP_PACK;       break;
    case SWS_OP_LSHIFT:     out->uop = SWS_UOP_LSHIFT;     break;
    case SWS_OP_RSHIFT:     out->uop = SWS_UOP_RSHIFT;     break;
    case SWS_OP_CLEAR:      out->uop = SWS_UOP_CLEAR;      break;
    case SWS_OP_CONVERT:
        if (op->convert.expand) {
            switch (op->convert.to) {
            case SWS_PIXEL_U16: out->uop = SWS_UOP_EXPAND_PAIR; break;
            case SWS_PIXEL_U32: out->uop = SWS_UOP_EXPAND_QUAD; break;
            }
        } else {
            switch (op->convert.to) {
            case SWS_PIXEL_U8:  out->uop = SWS_UOP_TO_U8;  break;
            case SWS_PIXEL_U16: out->uop = SWS_UOP_TO_U16; break;
            case SWS_PIXEL_U32: out->uop = SWS_UOP_TO_U32; break;
            case SWS_PIXEL_F32: out->uop = SWS_UOP_TO_F32; break;
            }
        }
        break;
    case SWS_OP_MIN:        out->uop = SWS_UOP_MIN;        break;
    case SWS_OP_MAX:        out->uop = SWS_UOP_MAX;        break;
    case SWS_OP_SCALE:      out->uop = SWS_UOP_SCALE;      break;
    case SWS_OP_LINEAR:
        out->uop = (ctx->flags & SWS_BITEXACT)
                 ? SWS_UOP_LINEAR
                 : SWS_UOP_LINEAR_FMA;
        break;
    case SWS_OP_DITHER:     out->uop = SWS_UOP_DITHER;     break;
    case SWS_OP_FILTER_H:
    case SWS_OP_FILTER_V:
        return AVERROR(ENOTSUP);
    }

    switch (out->uop) {
    case SWS_UOP_READ_BIT:
    case SWS_UOP_READ_NIBBLE:
    case SWS_UOP_READ_PACKED:
    case SWS_UOP_READ_PLANAR:
    case SWS_UOP_WRITE_BIT:
    case SWS_UOP_WRITE_NIBBLE:
    case SWS_UOP_WRITE_PACKED:
    case SWS_UOP_WRITE_PLANAR:
        switch (op->rw.elems) {
        case 1: out->mask = 0x0001; break;
        case 2: out->mask = 0x0011; break;
        case 3: out->mask = 0x0111; break;
        case 4: out->mask = 0x1111; break;
        };
        break;
    case SWS_UOP_SWAP_BYTES:
        /* Only the element size matters, not the type. */
        if (out->type == SWS_PIXEL_F32)
            out->type = SWS_PIXEL_U32;
        break;
    case SWS_UOP_MOVE:
        out->mask = 0;
        MASK_SET(out->mask, 0, op->swizzle.in[0] != 0);
        MASK_SET(out->mask, 1, op->swizzle.in[1] != 1);
        MASK_SET(out->mask, 2, op->swizzle.in[2] != 2);
        MASK_SET(out->mask, 3, op->swizzle.in[3] != 3);
        convert_swizzle_to_moves(op, out);
#if 0
        /* The element size and type don't matter. */
        out->block_size = block_size * ff_sws_pixel_type_size(op->type);
        out->type = SWS_PIXEL_U8;
#else
        /* Only the element size matters, not the type. */
        if (out->type == SWS_PIXEL_F32)
            out->type = SWS_PIXEL_U32;
#endif
        break;
    case SWS_UOP_UNPACK:
        MASK_SET(out->pack, 0, op->pack.pattern[0]);
        MASK_SET(out->pack, 1, op->pack.pattern[1]);
        MASK_SET(out->pack, 2, op->pack.pattern[2]);
        MASK_SET(out->pack, 3, op->pack.pattern[3]);
        break;
    case SWS_UOP_PACK:
        out->mask = 0;
        for (int i = 0; i < 4 && op->pack.pattern[i]; i++)
            MASK_SET(out->mask, i, 1);
        MASK_SET(out->pack, 0, op->pack.pattern[0]);
        MASK_SET(out->pack, 1, op->pack.pattern[1]);
        MASK_SET(out->pack, 2, op->pack.pattern[2]);
        MASK_SET(out->pack, 3, op->pack.pattern[3]);
        break;
    case SWS_UOP_LSHIFT:
    case SWS_UOP_RSHIFT:
        out->shift = op->shift.amount;
        break;
    case SWS_UOP_CLEAR:
        out->mask = 0;
        MASK_SET(out->mask, 0, !!op->clear.value[0].den);
        MASK_SET(out->mask, 1, !!op->clear.value[1].den);
        MASK_SET(out->mask, 2, !!op->clear.value[2].den);
        MASK_SET(out->mask, 3, !!op->clear.value[3].den);
        out->clear = 0;
        for (int i = 0; i < 4; i++) {
            int mask_val = 0xf;
            if (MASK_GET(out->mask, i)) {
                uint32_t val = op->clear.value[i].num / op->clear.value[i].den;
                if (val == 0) {
                    mask_val = 0;
                } else if ((op->type == SWS_PIXEL_U8  && val == UINT8_MAX)  ||
                        (op->type == SWS_PIXEL_U16 && val == UINT16_MAX) ||
                        (op->type == SWS_PIXEL_U32 && val == UINT32_MAX)) {
                    mask_val = 1;
                }
            }
            MASK_SET(out->clear, i, mask_val);
        }
        break;
    case SWS_UOP_LINEAR:
    case SWS_UOP_LINEAR_FMA:
        /**
         * The out->linear.mask field packs the 4x5 matrix from SwsLinearOp as
         * 2 bits per element:
         *   00: m[i][j] == 0
         *   01: m[i][j] == 1
         *   11: m[i][j] is any other coefficient
         */
        out->mask = 0;
        for (int i = 0; i < 4; i++) {
            /* Skip unused or identity rows */
            if (!SWS_OP_NEEDED(op, i) || !(op->lin.mask & SWS_MASK_ROW(i)))
                continue;
            MASK_SET(out->mask, i, 1);
            for (int j = 0; j < 5; j++) {
                const AVRational k = op->lin.m[i][j];
                int jj = linear_index_from_sws_op(j);
                if (j < 4 && k.num == k.den)
                    LINEAR_MASK_SET(out->linear.mask, i, jj, LINEAR_MASK_1);
                else if (k.num != 0)
                    LINEAR_MASK_SET(out->linear.mask, i, jj, LINEAR_MASK_X);
            }
        }
        out->linear.fmla = !(ctx->flags & SWS_BITEXACT);
        break;
    case SWS_UOP_DITHER:
        out->mask = 0;
        MASK_SET(out->mask, 0, op->dither.y_offset[0] >= 0);
        MASK_SET(out->mask, 1, op->dither.y_offset[1] >= 0);
        MASK_SET(out->mask, 2, op->dither.y_offset[2] >= 0);
        MASK_SET(out->mask, 3, op->dither.y_offset[3] >= 0);
        MASK_SET(out->dither.y_offset, 0, op->dither.y_offset[0]);
        MASK_SET(out->dither.y_offset, 1, op->dither.y_offset[1]);
        MASK_SET(out->dither.y_offset, 2, op->dither.y_offset[2]);
        MASK_SET(out->dither.y_offset, 3, op->dither.y_offset[3]);
        out->dither.size_log2 = op->dither.size_log2;
        break;
    }

    return 0;
}
