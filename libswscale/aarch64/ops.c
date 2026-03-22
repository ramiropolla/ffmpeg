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

#include "libswscale/aarch64/rasm.h"

#include "libavutil/avstring.h"

#define LOOP_ARRAY(idx, arr)          \
    for (int idx = 0; idx < 4; idx++) \
        if (arr[idx])
#define LOOP_IN(idx)  LOOP_ARRAY(idx, !op->comps.unused)

#define Q(N) ((AVRational) { N, 1 })
#define Q0   Q(0)
#define Q1   Q(1)

#include "ops_impl.h"
#include "ops_lookup.h"

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

/* Convert SwsOp to a simplified structure used to generate NEON implementations. */
static void aarch64_impl_params(const SwsOpList *ops, int block_size, int n, SwsAArch64OpImplParams *out)
{
    const SwsOp *op = &ops->ops[n];
    const SwsOp *next = n + 1 < ops->num_ops ? &ops->ops[n + 1] : op;

    out->block_size = block_size;

    /* Most SwsOp work on fields described by next->comps.unused.
     * The few that don't will override this field later.
     */
    for (int i = 0; i < 4; i++) {
        if (!next->comps.unused[i])
            MASK_SET(out->mask, i, 1);
    }

    out->type = sws_pixel_to_aarch64(op->type);

    /* Map SwsOpType to SwsAArch64OpType */
    switch (op->op) {
    case SWS_OP_READ:
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
        /* Only the element size matters */
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
    case AARCH64_SWS_OP_LINEAR: {
        /*
         * The linear mask in out->linear packs the 4x5 matrix from SwsLinearOp
         * as 2 bits per entry:
         *   00: m[i][j] == 0
         *   01: m[i][j] == 1
         *   11: m[i][j] is any other coefficient
         * The columns are reordered so that the offset is at column 0.
         */
        const int reorder_col[5] = { 1, 2, 3, 4, 0 };
        out->mask = 0;
        for (int i = 0; i < 4; i++) {
            /* skip unused or identity rows */
            if (op->comps.unused[i] || !(op->lin.mask & SWS_MASK_ROW(i)))
                continue;
            MASK_SET(out->mask, i, 1);
            for (int j = 0; j < 5; j++) {
                int reordered_j = reorder_col[j];
                if (!av_cmp_q(op->lin.m[i][j], Q1))
                    LINEAR_MASK_SET(out->linear, i, reordered_j, 1ULL);
                else if (av_cmp_q(op->lin.m[i][j], Q0))
                    LINEAR_MASK_SET(out->linear, i, reordered_j, 3ULL);
            }
        }
        break;
    }
    case AARCH64_SWS_OP_DITHER:
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
    float *coeffs = av_malloc(linear_num_vregs(p) * 4 * sizeof(float));
    if (!coeffs)
        return AVERROR(ENOMEM);

    /* Copy non-zero coefficients, reordered to match SwsAArch64LinearOpMask. */
    int i_coeff = 0;
    LOOP_LINEAR_MASK(p, i, j) {
        coeffs[i_coeff++] = (float) av_q2d(op->lin.m[i][j ? j - 1 : 4]);
    }

    res->priv.ptr = coeffs;
    res->free = ff_op_priv_free;
    return 0;
}

/*********************************************************************/
static int aarch64_setup_dither(const SwsAArch64OpImplParams *p,
                                const SwsOp *op, SwsImplResult *res)
{
    const int size_log2 = p->dither.size_log2;
    const int size      = 1 << size_log2;

    /* Find the largest y_offset among active components to determine
     * how many extra rows to append so codegen never needs to mask y. */
    int largest_y_off = 0;
    for (int i = 0; i < 4; i++) {
        if (op->dither.y_offset[i] >= 0)
            largest_y_off = FFMAX(largest_y_off, (int)op->dither.y_offset[i]);
    }

    /* Allocate (size + largest_y_off) rows × size columns.
     * The extra rows are filled by wrapping into the base matrix so that
     * codegen can advance the pointer by a compile-time byte delta without
     * any runtime masking. */
    int total = (size + largest_y_off) * size;
    float *matrix = av_malloc(total * sizeof(float));
    if (!matrix)
        return AVERROR(ENOMEM);

    int mask = (size * size) - 1;
    for (int i = 0; i < total; i++)
        matrix[i] = (float)av_q2d(op->dither.matrix[i & mask]);

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
static int aarch64_compile(SwsContext *ctx, SwsOpList *ops, SwsCompiledOp *out)
{
    int block_size = (ff_sws_op_list_max_size(ops) == 4) ? 8 : 16;
    int ret;

    const int cpu_flags = av_get_cpu_flags();
    if (!(cpu_flags & AV_CPU_FLAG_NEON))
        return AVERROR(ENOTSUP);

    SwsOpChain *chain = ff_sws_op_chain_alloc();
    if (!chain)
        return AVERROR(ENOMEM);
    chain->cpu_flags = AV_CPU_FLAG_NEON;

    *out = (SwsCompiledOp) {
        .priv        = chain,
        .slice_align = 1,
        .free        = ff_sws_op_chain_free_cb,
        .block_size  = block_size,
    };

    for (int i = 0; i < ops->num_ops; i++) {
        SwsAArch64OpImplParams params = { 0 };
        aarch64_impl_params(ops, block_size, i, &params);
        SwsFuncPtr func = ff_sws_aarch64_lookup(&params);
        if (!func) {
            ret = AVERROR(ENOTSUP);
            goto error;
        }
        SwsImplResult res = { 0 };
        ret = aarch64_setup(ops, block_size, i, &params, &res);
        if (ret < 0)
            goto error;
        ret = ff_sws_op_chain_append(chain, func, NULL, &res.priv);
        if (ret < 0)
            goto error;
    }

    const SwsOp *read  = ff_sws_op_list_input(ops);
    const SwsOp *write = ff_sws_op_list_output(ops);
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

    ret = ff_sws_op_chain_append(chain, return_func, NULL, &(SwsOpPriv) {0});
    if (ret < 0)
        goto error;

    out->func       = (SwsOpFunc) process_func;
    out->cpu_flags  = chain->cpu_flags;
    out->over_read  = chain->over_read;
    out->over_write = chain->over_write;

error:
    if (ret < 0) {
        if (ret == AVERROR(ENOTSUP)){
            av_log(ctx, AV_LOG_DEBUG, "Unsupported SwsOp for aarch64.\n");
            av_log(ctx, AV_LOG_DEBUG, "Regenerate ops_entries.c with: make sws_ops_entries_aarch64\n");
        }
        ff_sws_op_chain_free(chain);
    }
    return ret;
}

/*********************************************************************/
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

static int aarch64_collect_process(const SwsOpList *ops, struct AVTreeNode **root)
{
    const SwsOp *read  = ff_sws_op_list_input(ops);
    const SwsOp *write = ff_sws_op_list_output(ops);
    const int read_planes  = read ? (read->rw.packed ? 1 : read->rw.elems) : 0;
    const int write_planes = write->rw.packed ? 1 : write->rw.elems;
    SwsAArch64OpMask mask = 0;
    for (int i = 0; i < FFMAX(read_planes, write_planes); i++)
        MASK_SET(mask, i, 1);
    SwsAArch64OpImplParams params = {
        .op   = AARCH64_SWS_OP_PROCESS,
        .mask = mask,
    };
    aarch64_collect_op(&params, root);
    params.op = AARCH64_SWS_OP_PROCESS_RETURN;
    aarch64_collect_op(&params, root);
    return 0;
}

static int aarch64_collect_ops(const SwsOpList *ops, struct AVTreeNode **root)
{
    int ret = AVERROR(EINVAL);

    aarch64_collect_process(ops, root);

    /* Use at most two full vregs during the widest precision section */
    int block_size = (ff_sws_op_list_max_size(ops) == 4) ? 8 : 16;

    for (int i = 0; i < ops->num_ops; i++) {
        SwsAArch64OpImplParams params = { 0 };
        aarch64_impl_params(ops, block_size, i, &params);
        ret = aarch64_collect_op(&params, root);
        if (ret < 0)
            goto end;
    }

    ret = 0;

end:
    return ret;
}

/*********************************************************************/
static int aarch64_print_op(void *opaque, void *elem)
{
    SwsAArch64OpImplParams *params = (SwsAArch64OpImplParams *) elem;
    FILE *fp = (FILE *) opaque;

    char buf[256];
    sws_aarch64_op_impl_serialize(buf, sizeof(buf), params);
    fprintf(fp, "%s,\n", buf);

    av_free(params);

    return 0;
}

static int aarch64_print_ops(struct AVTreeNode **root, FILE *fp)
{
    fprintf(fp, "/*\n");
    fprintf(fp, " * This file is automatically generated. Do not edit manually.\n");
    fprintf(fp, " * To regenerate, run: make sws_ops_entries_aarch64\n");
    fprintf(fp, " */\n");
    fprintf(fp, "\n");
    fprintf(fp, "#define offsetof_exec_in       %zu\n", offsetof(SwsOpExec, in));
    fprintf(fp, "#define offsetof_exec_out      %zu\n", offsetof(SwsOpExec, out));
    fprintf(fp, "#define offsetof_exec_in_bump  %zu\n", offsetof(SwsOpExec, in_bump));
    fprintf(fp, "#define offsetof_exec_out_bump %zu\n", offsetof(SwsOpExec, out_bump));
    fprintf(fp, "#define offsetof_impl_cont     %zu\n", offsetof(SwsOpImpl, cont));
    fprintf(fp, "#define offsetof_impl_priv     %zu\n", offsetof(SwsOpImpl, priv));
    fprintf(fp, "#define sizeof_impl            %zu\n", sizeof(SwsOpImpl));
    fprintf(fp, "\n");
    av_tree_enumerate(*root, fp, NULL, aarch64_print_op);
    return 0;
}

/*********************************************************************/
const SwsOpBackend backend_aarch64 = {
    .name        = "aarch64",
    .compile     = aarch64_compile,
    .collect_ops = aarch64_collect_ops,
    .print_ops   = aarch64_print_ops,
    .hw_format   = AV_PIX_FMT_NONE,
};
