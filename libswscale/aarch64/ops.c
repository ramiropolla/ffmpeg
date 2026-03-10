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

#include "../ops_chain.h"

#include "libavutil/avstring.h"
#include "libavutil/tree.h"

#include "ops_impl.h"
#include "ops_lookup.h"

#include "libswscale/aarch64/ops.h"

/*********************************************************************/
typedef struct SwsAArch64BackendContext {
    SwsContext *sws;
    int block_size;
} SwsAArch64BackendContext;

/*********************************************************************/
static uint8_t sws_pixel_to_aarch64(SwsPixelType type)
{
    switch (type) {
    case SWS_PIXEL_U8:  return AARCH64_PIXEL_U8;
    case SWS_PIXEL_U16: return AARCH64_PIXEL_U16;
    case SWS_PIXEL_U32: return AARCH64_PIXEL_U32;
    case SWS_PIXEL_F32: return AARCH64_PIXEL_F32;
    }
    return 0;
}

/**
 * Convert SwsOp to a SwsAArch64OpImplParams. Read the comments regarding
 * SwsAArch64OpImplParams in ops_impl.h for more information.
 */
static void aarch64_impl_params(SwsContext *ctx, const SwsOpList *ops,
                                int block_size, int n, SwsAArch64OpImplParams *out)
{
    const SwsOp *op = &ops->ops[n];
    const SwsOp *next = n + 1 < ops->num_ops ? &ops->ops[n + 1] : op;

    out->block_size = block_size;

    /**
     * Most SwsOp work on fields described by next->comps.unused.
     * The few that don't will override this field later.
     */
    out->mask = 0;
    for (int i = 0; i < 4; i++) {
        if (!next->comps.unused[i])
            MASK_SET(out->mask, i, 1);
    }

    out->type = sws_pixel_to_aarch64(op->type);

    /* Map SwsOpType to SwsAArch64OpType */
    switch (op->op) {
    case SWS_OP_READ:
        /**
         * The different types of read operations have been split into
         * their own SwsAArch64OpType to simplify the implementation.
         */
        if (op->rw.frac == 1)
            out->op = AARCH64_SWS_OP_READ_NIBBLE;
        else if (op->rw.frac == 3)
            out->op = AARCH64_SWS_OP_READ_BIT;
        else if (op->rw.packed && op->rw.elems != 1)
            out->op = AARCH64_SWS_OP_READ_PACKED;
        else
            out->op = AARCH64_SWS_OP_READ_PLANAR;
        break;
    case SWS_OP_WRITE:
        /**
         * The different types of write operations have been split into
         * their own SwsAArch64OpType to simplify the implementation.
         */
        if (op->rw.frac == 1)
            out->op = AARCH64_SWS_OP_WRITE_NIBBLE;
        else if (op->rw.frac == 3)
            out->op = AARCH64_SWS_OP_WRITE_BIT;
        else if (op->rw.packed && op->rw.elems != 1)
            out->op = AARCH64_SWS_OP_WRITE_PACKED;
        else
            out->op = AARCH64_SWS_OP_WRITE_PLANAR;
        break;
    case SWS_OP_SWAP_BYTES: out->op = AARCH64_SWS_OP_SWAP_BYTES; break;
    case SWS_OP_SWIZZLE:    out->op = AARCH64_SWS_OP_SWIZZLE;    break;
    case SWS_OP_UNPACK:     out->op = AARCH64_SWS_OP_UNPACK;     break;
    case SWS_OP_PACK:       out->op = AARCH64_SWS_OP_PACK;       break;
    case SWS_OP_LSHIFT:     out->op = AARCH64_SWS_OP_LSHIFT;     break;
    case SWS_OP_RSHIFT:     out->op = AARCH64_SWS_OP_RSHIFT;     break;
    case SWS_OP_CLEAR:      out->op = AARCH64_SWS_OP_CLEAR;      break;
    case SWS_OP_CONVERT:
        out->op = op->convert.expand ? AARCH64_SWS_OP_EXPAND : AARCH64_SWS_OP_CONVERT;
        break;
    case SWS_OP_MIN:        out->op = AARCH64_SWS_OP_MIN;        break;
    case SWS_OP_MAX:        out->op = AARCH64_SWS_OP_MAX;        break;
    case SWS_OP_SCALE:      out->op = AARCH64_SWS_OP_SCALE;      break;
    case SWS_OP_LINEAR:     out->op = AARCH64_SWS_OP_LINEAR;     break;
    case SWS_OP_DITHER:     out->op = AARCH64_SWS_OP_DITHER;     break;
    }

    switch (out->op) {
    case AARCH64_SWS_OP_READ_BIT:
    case AARCH64_SWS_OP_READ_NIBBLE:
    case AARCH64_SWS_OP_READ_PACKED:
    case AARCH64_SWS_OP_READ_PLANAR:
    case AARCH64_SWS_OP_WRITE_BIT:
    case AARCH64_SWS_OP_WRITE_NIBBLE:
    case AARCH64_SWS_OP_WRITE_PACKED:
    case AARCH64_SWS_OP_WRITE_PLANAR:
        switch (op->rw.elems) {
        case 1: out->mask = 0x0001; break;
        case 2: out->mask = 0x0011; break;
        case 3: out->mask = 0x0111; break;
        case 4: out->mask = 0x1111; break;
        };
        break;
    case AARCH64_SWS_OP_SWAP_BYTES:
        /* Only the element size matters, not the type. */
        if (out->type == AARCH64_PIXEL_F32)
            out->type = AARCH64_PIXEL_U32;
        break;
    case AARCH64_SWS_OP_SWIZZLE:
        out->mask = 0;
        MASK_SET(out->mask, 0, op->swizzle.in[0] != 0);
        MASK_SET(out->mask, 1, op->swizzle.in[1] != 1);
        MASK_SET(out->mask, 2, op->swizzle.in[2] != 2);
        MASK_SET(out->mask, 3, op->swizzle.in[3] != 3);
        MASK_SET(out->swizzle, 0, op->swizzle.in[0]);
        MASK_SET(out->swizzle, 1, op->swizzle.in[1]);
        MASK_SET(out->swizzle, 2, op->swizzle.in[2]);
        MASK_SET(out->swizzle, 3, op->swizzle.in[3]);
        /* The element size and type don't matter. */
        out->block_size = block_size * ff_sws_pixel_type_size(op->type);
        out->type = AARCH64_PIXEL_U8;
        break;
    case AARCH64_SWS_OP_UNPACK:
        MASK_SET(out->pack, 0, op->pack.pattern[0]);
        MASK_SET(out->pack, 1, op->pack.pattern[1]);
        MASK_SET(out->pack, 2, op->pack.pattern[2]);
        MASK_SET(out->pack, 3, op->pack.pattern[3]);
        break;
    case AARCH64_SWS_OP_PACK:
        out->mask = 0;
        MASK_SET(out->mask, 0, !op->comps.unused[0]);
        MASK_SET(out->mask, 1, !op->comps.unused[1]);
        MASK_SET(out->mask, 2, !op->comps.unused[2]);
        MASK_SET(out->mask, 3, !op->comps.unused[3]);
        MASK_SET(out->pack, 0, op->pack.pattern[0]);
        MASK_SET(out->pack, 1, op->pack.pattern[1]);
        MASK_SET(out->pack, 2, op->pack.pattern[2]);
        MASK_SET(out->pack, 3, op->pack.pattern[3]);
        break;
    case AARCH64_SWS_OP_LSHIFT:
    case AARCH64_SWS_OP_RSHIFT:
        out->shift = op->c.u;
        break;
    case AARCH64_SWS_OP_CLEAR:
        out->mask = 0;
        MASK_SET(out->mask, 0, !!op->c.q4[0].den);
        MASK_SET(out->mask, 1, !!op->c.q4[1].den);
        MASK_SET(out->mask, 2, !!op->c.q4[2].den);
        MASK_SET(out->mask, 3, !!op->c.q4[3].den);
        break;
    case AARCH64_SWS_OP_EXPAND:
    case AARCH64_SWS_OP_CONVERT:
        out->to_type = sws_pixel_to_aarch64(op->convert.to);
        break;
    case AARCH64_SWS_OP_LINEAR:
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
            if (op->comps.unused[i] || !(op->lin.mask & SWS_MASK_ROW(i)))
                continue;
            MASK_SET(out->mask, i, 1);
            for (int j = 0; j < 5; j++) {
                int jj = linear_index_from_sws_op(j);
                if (!av_cmp_q(op->lin.m[i][j], av_make_q(1, 1)))
                    LINEAR_MASK_SET(out->linear.mask, i, jj, LINEAR_MASK_1);
                else if (av_cmp_q(op->lin.m[i][j], av_make_q(0, 1)))
                    LINEAR_MASK_SET(out->linear.mask, i, jj, LINEAR_MASK_X);
            }
        }
        out->linear.fmla = !(ctx->flags & SWS_BITEXACT);
        break;
    case AARCH64_SWS_OP_DITHER:
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
}

/*********************************************************************/
static int aarch64_setup_linear(const SwsAArch64OpImplParams *p,
                                const SwsOp *op, SwsImplResult *res)
{
    /**
     * Compute number of full vector registers needed to pack all non-zero
     * coefficients.
     */
    const int num_vregs = linear_num_vregs(p);
    av_assert0(num_vregs <= 4);
    float *coeffs = av_malloc(num_vregs * 4 * sizeof(float));
    if (!coeffs)
        return AVERROR(ENOMEM);

    /**
     * Copy non-zero coefficients, reordered to match SwsAArch64LinearOpMask.
     * The coefficients are packed in sequential order. The same order must
     * be followed in asmgen_op_linear().
     */
    int i_coeff = 0;
    LOOP_LINEAR_MASK(p, i, j) {
        const int jj = linear_index_to_sws_op(j);
        coeffs[i_coeff++] = (float) op->lin.m[i][jj].num / op->lin.m[i][jj].den;
    }

    res->priv.ptr = coeffs;
    res->free = ff_op_priv_free;

    return 0;
}

/*********************************************************************/
static int aarch64_setup_dither(const SwsAArch64OpImplParams *p,
                                const SwsOp *op, SwsImplResult *res)
{
    /**
     * The input dither matrix is (1 << size_log2)² pixels large. It is
     * periodic, so the x and y offsets should be masked to fit inside
     * (1 << size_log2).
     * The width of the matrix is assumed to be at least 8, which matches
     * the maximum block_size for aarch64 asmgen when f32 operations
     * (i.e., dithering) are used. This guarantees that the x offset is
     * aligned and that reading block_size elements does not extend past
     * the end of the row. The x offset doesn't change between components,
     * so it is only required to be masked once.
     * The y offset, on the other hand, may change per component, and
     * would therefore need to be masked for every y_offset value. To
     * simplify the execution, we over-allocate the number of rows of
     * the output dither matrix by the largest y_offset value. This way,
     * we only need to mask y offset once, and can safely increment the
     * dither matrix pointer by fixed offsets for every y_offset change.
     */

    /* Find the largest y_offset value. */
    const int size = 1 << op->dither.size_log2;
    const int8_t *off = op->dither.y_offset;
    int max_offset = 0;
    for (int i = 0; i < 4; i++) {
        if (off[i] >= 0)
            max_offset = FFMAX(max_offset, off[i] & (size - 1));
    }

    /* Allocate (size + max_offset) rows to allow over-reading the matrix. */
    const int stride = size * sizeof(float);
    const int num_rows = size + max_offset;
    float *matrix = av_malloc(num_rows * stride);
    if (!matrix)
        return AVERROR(ENOMEM);

    for (int i = 0; i < size * size; i++)
        matrix[i] = (float) op->dither.matrix[i].num / op->dither.matrix[i].den;

    memcpy(&matrix[size * size], matrix, max_offset * stride);

    res->priv.ptr = matrix;
    res->free = ff_op_priv_free;

    return 0;
}

/*********************************************************************/
static int aarch64_setup(SwsOpList *ops, int block_size, int n,
                         const SwsAArch64OpImplParams *p, SwsImplResult *out)
{
    SwsOp *op = &ops->ops[n];
    switch (op->op) {
    case SWS_OP_READ:
        /* Negative shift values to perform right shift using ushl. */
        if (op->rw.frac == 3) {
            out->priv = (SwsOpPriv) {
                .u8 = {
                    -7, -6, -5, -4, -3, -2, -1, 0,
                    -7, -6, -5, -4, -3, -2, -1, 0,
                }
            };
        }
        break;
    case SWS_OP_WRITE:
        /* Shift values for ushl. */
        if (op->rw.frac == 3) {
            out->priv = (SwsOpPriv) {
                .u8 = {
                    7, 6, 5, 4, 3, 2, 1, 0,
                    7, 6, 5, 4, 3, 2, 1, 0,
                }
            };
        }
        break;
    case SWS_OP_CLEAR:
    case SWS_OP_MIN:
    case SWS_OP_MAX:
        ff_sws_setup_q4(&(const SwsImplParams) { .op = op }, out);
        break;
    case SWS_OP_SCALE:
        ff_sws_setup_q(&(const SwsImplParams) { .op = op }, out);
        break;
    case SWS_OP_LINEAR:
        return aarch64_setup_linear(p, op, out);
    case SWS_OP_DITHER:
        return aarch64_setup_dither(p, op, out);
    }
    return 0;
}

/*********************************************************************/
static int aarch64_optimize(SwsAArch64BackendContext *bctx, SwsOpList *ops)
{
    /* Currently, no optimization is performed. This is just a placeholder. */

    /* Use at most two full vregs during the widest precision section */
    bctx->block_size = (ff_sws_op_list_max_size(ops) == 4) ? 8 : 16;

    return 0;
}

/*********************************************************************/
static int aarch64_compile(SwsContext *ctx, SwsOpList *ops, SwsCompiledOp *out)
{
    SwsAArch64BackendContext bctx;
    int ret;

    const int cpu_flags = av_get_cpu_flags();
    if (!(cpu_flags & AV_CPU_FLAG_NEON))
        return AVERROR(ENOTSUP);

    /* Make on-stack copy of `ops` to iterate over */
    SwsOpList rest = *ops;
    bctx.sws = ctx;
    ret = aarch64_optimize(&bctx, &rest);
    if (ret < 0)
        return ret;

    SwsOpChain *chain = ff_sws_op_chain_alloc();
    if (!chain)
        return AVERROR(ENOMEM);
    chain->cpu_flags = AV_CPU_FLAG_NEON;

    *out = (SwsCompiledOp) {
        .priv        = chain,
        .slice_align = 1,
        .free        = ff_sws_op_chain_free_cb,
        .block_size  = bctx.block_size,
    };

    /* Look up kernel functions. */
    for (int i = 0; i < rest.num_ops; i++) {
        SwsAArch64OpImplParams params = { 0 };
        aarch64_impl_params(ctx, &rest, bctx.block_size, i, &params);
        SwsFuncPtr func = ff_sws_aarch64_lookup(&params);
        if (!func) {
            ret = AVERROR(ENOTSUP);
            goto error;
        }
        SwsImplResult res = { 0 };
        ret = aarch64_setup(&rest, bctx.block_size, i, &params, &res);
        if (ret < 0)
            goto error;
        ret = ff_sws_op_chain_append(chain, func, res.free, &res.priv);
        if (ret < 0)
            goto error;
    }

    /* Look up process/process_return functions. */
    const SwsOp *read  = ff_sws_op_list_input(&rest);
    const SwsOp *write = ff_sws_op_list_output(&rest);
    const int read_planes  = read ? (read->rw.packed ? 1 : read->rw.elems) : 0;
    const int write_planes = write->rw.packed ? 1 : write->rw.elems;
    SwsAArch64OpMask mask = 0;
    for (int i = 0; i < FFMAX(read_planes, write_planes); i++)
        MASK_SET(mask, i, 1);

    SwsAArch64OpImplParams process_params = { .op = AARCH64_SWS_OP_PROCESS,        .mask = mask };
    SwsAArch64OpImplParams return_params  = { .op = AARCH64_SWS_OP_PROCESS_RETURN, .mask = mask };
    SwsFuncPtr process_func = ff_sws_aarch64_lookup(&process_params);
    SwsFuncPtr return_func  = ff_sws_aarch64_lookup(&return_params);
    if (!process_func || !return_func) {
        ret = AVERROR(ENOTSUP);
        goto error;
    }

    ret = ff_sws_op_chain_append(chain, return_func, NULL, &(SwsOpPriv) { 0 });
    if (ret < 0)
        goto error;

    out->func      = (SwsOpFunc) process_func;
    out->cpu_flags = chain->cpu_flags;

error:
    if (ret < 0)
        ff_sws_op_chain_free(chain);
    return ret;
}

/*********************************************************************/
/* Insert the SwsAArch64OpImplParams structure into the AVTreeNode. */
static int aarch64_collect_op(const SwsAArch64OpImplParams *params, struct AVTreeNode **root)
{
    int ret = 0;

    struct AVTreeNode *node = av_tree_node_alloc();
    SwsAArch64OpImplParams *copy = av_memdup(params, sizeof(*params));
    if (!node || !copy) {
        ret = AVERROR(ENOMEM);
        goto error;
    }
    av_tree_insert(root, copy, sws_aarch64_op_impl_cmp, &node);
    if (!node)
        copy = NULL;

error:
    av_free(node);
    av_free(copy);
    return ret;
}

/* Collect the parameters for the process/process_return functions. */
static int aarch64_collect_process(const SwsOpList *ops, struct AVTreeNode **root)
{
    const SwsOp *read  = ff_sws_op_list_input(ops);
    const SwsOp *write = ff_sws_op_list_output(ops);
    const int read_planes  = read ? (read->rw.packed ? 1 : read->rw.elems) : 0;
    const int write_planes = write->rw.packed ? 1 : write->rw.elems;
    int ret;

    SwsAArch64OpMask mask = 0;
    for (int i = 0; i < FFMAX(read_planes, write_planes); i++)
        MASK_SET(mask, i, 1);
    SwsAArch64OpImplParams params = {
        .op   = AARCH64_SWS_OP_PROCESS,
        .mask = mask,
    };

    ret = aarch64_collect_op(&params, root);
    if (ret < 0)
        return ret;

    params.op = AARCH64_SWS_OP_PROCESS_RETURN;
    ret = aarch64_collect_op(&params, root);
    if (ret < 0)
        return ret;

    return 0;
}

int ff_sws_collect_ops_aarch64(SwsContext *ctx, void *opaque, SwsOpList *ops)
{
    struct AVTreeNode **root = (struct AVTreeNode **) opaque;
    SwsAArch64BackendContext bctx;
    int ret;

    /* Make on-stack copy of `ops` to iterate over */
    SwsOpList rest = *ops;
    bctx.sws = ctx;
    ret = aarch64_optimize(&bctx, &rest);
    if (ret < 0)
        return ret;

    ret = aarch64_collect_process(&rest, root);
    if (ret < 0)
        return ret;

    for (int i = 0; i < rest.num_ops; i++) {
        SwsAArch64OpImplParams params = { 0 };
        aarch64_impl_params(ctx, &rest, bctx.block_size, i, &params);
        ret = aarch64_collect_op(&params, root);
        if (ret < 0)
            goto end;
        if (params.op == AARCH64_SWS_OP_LINEAR) {
            /**
             * Generate both sets of linear op functions that do use
             * and do not use fmla (selected by SWS_BITEXACT).
             */
            params.linear.fmla = !params.linear.fmla;
            ret = aarch64_collect_op(&params, root);
            if (ret < 0)
                goto end;
        }
    }

    ret = 0;

end:
    return ret;
}

/*********************************************************************/
/* Serialize SwsAArch64OpImplParams for one function. */
int ff_sws_op_print_aarch64(void *opaque, void *elem)
{
    SwsAArch64OpImplParams *params = (SwsAArch64OpImplParams *) elem;
    FILE *fp = (FILE *) opaque;

    char buf[256];
    sws_aarch64_op_impl_serialize(buf, sizeof(buf), params);
    fprintf(fp, "%s,\n", buf);

    av_free(params);

    return 0;
}

/*********************************************************************/
const SwsOpBackend backend_aarch64 = {
    .name      = "aarch64",
    .compile   = aarch64_compile,
    .hw_format = AV_PIX_FMT_NONE,
};
