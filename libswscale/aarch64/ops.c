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
            out->mask |= (1 << (i << 2));
    }

    out->type = sws_pixel_to_aarch64(op->type);

    /* Map SwsOpType to SwsAArch64OpType */
    switch (op->op) {
    case SWS_OP_READ:
        if (op->rw.frac == 1)
            out->op = AARCH64_SWS_OP_READ_BIT;
        else if (op->rw.frac == 3)
            out->op = AARCH64_SWS_OP_READ_NIBBLE;
        else if (op->rw.packed && op->rw.elems != 1)
            out->op = AARCH64_SWS_OP_READ_PACKED;
        else
            out->op = AARCH64_SWS_OP_READ_PLANAR;
        break;
    case SWS_OP_WRITE:
        if (op->rw.frac == 1)
            out->op = AARCH64_SWS_OP_WRITE_BIT;
        else if (op->rw.frac == 3)
            out->op = AARCH64_SWS_OP_WRITE_NIBBLE;
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
        out->mask = (op->swizzle.in[0] != 0)
                  | (op->swizzle.in[1] != 1) << 4
                  | (op->swizzle.in[2] != 2) << 8
                  | (op->swizzle.in[3] != 3) << 12;
        out->swizzle = op->swizzle.in[0]
                     | op->swizzle.in[1] << 4
                     | op->swizzle.in[2] << 8
                     | op->swizzle.in[3] << 12;
        out->block_size = block_size * ff_sws_pixel_type_size(op->type);
        out->type = AARCH64_PIXEL_U8;
        break;
    case AARCH64_SWS_OP_UNPACK:
        out->pack = op->pack.pattern[0]
                  | op->pack.pattern[1] << 4
                  | op->pack.pattern[2] << 8
                  | op->pack.pattern[3] << 12;
        break;
    case AARCH64_SWS_OP_PACK:
        out->mask = (!op->comps.unused[0])
                  | (!op->comps.unused[1]) << 4
                  | (!op->comps.unused[2]) << 8
                  | (!op->comps.unused[3]) << 12;
        out->pack = op->pack.pattern[0]
                  | op->pack.pattern[1] << 4
                  | op->pack.pattern[2] << 8
                  | op->pack.pattern[3] << 12;
        break;
    case AARCH64_SWS_OP_LSHIFT:
    case AARCH64_SWS_OP_RSHIFT:
        out->shift = op->c.u;
        break;
    case AARCH64_SWS_OP_CLEAR:
        out->mask = !!op->c.q4[0].den
                  | !!op->c.q4[1].den << 4
                  | !!op->c.q4[2].den << 8
                  | !!op->c.q4[3].den << 12;
        break;
    case AARCH64_SWS_OP_EXPAND:
    case AARCH64_SWS_OP_CONVERT:
        out->to_type = sws_pixel_to_aarch64(op->convert.to);
        break;
    case AARCH64_SWS_OP_LINEAR: {
        /* Check which vectors are used after this operation */
        bool used[4] = { false, false, false, false };
        LOOP_IN(i) used[i] = true;
        LOOP_ARRAY(i, used) {
            bool is_identity = true;
            for (int j = 0; j < 5; j++) {
                if (i == j) {
                    if (op->lin.m[i][j].num != 1 || op->lin.m[i][j].den != 1) {
                        is_identity = false;
                        break;
                    }
                } else {
                    if (op->lin.m[i][j].num != 0) {
                        is_identity = false;
                        break;
                    }
                }
            }
            if (is_identity)
                used[i] = false;
        }

        // TODO I'm sure something can be simplified here regarding out->mask (or not set at all and use only linear)
        out->mask = 0;
        for (int i = 0; i < 4; i++) {
            if (!used[i])
                continue;
            out->mask |= (1 << (i << 2));
            for (int j = 0; j < 5; j++) {
                if (!av_cmp_q(op->lin.m[i][j], Q1))
                    out->linear |= 1ULL << (2 * ((5 * i) + j));
                else if (av_cmp_q(op->lin.m[i][j], Q0))
                    out->linear |= 3ULL << (2 * ((5 * i) + j));
            }
        }
        break;
    }
    }
}

#if 0
/*********************************************************************/
static void aarch64_setup(SwsOpList *ops, int block_size, int n, SwsOpPriv *out)
{
    SwsOp *op = &ops->ops[n];
    switch (op->op) {
    case SWS_OP_CLEAR: {
        SwsOp copy = *op;
        ff_sws_op_list_normalize_clear(&copy);
        for (int i = 0; i < 4; i++)
            out->u32[i] = (uint32_t) copy.c.q4[i].num;
        break;
    }
    }
}
#endif

/*********************************************************************/
static int aarch64_compile(SwsContext *ctx, SwsOpList *ops, SwsCompiledOp *out)
{
    int ret;
    int block_size = (ff_sws_op_list_max_size(ops) == 4) ? 8 : 16;

    SwsOpChain *chain = ff_sws_op_chain_alloc();
    if (!chain)
        return AVERROR(ENOMEM);

    *out = (SwsCompiledOp) {
        .priv        = chain,
        .slice_align = 1,
        .free        = ff_sws_op_chain_free_cb,
        .block_size  = block_size,
    };

    for (int i = 0; i < ops->num_ops; i++) {
        SwsAArch64OpImplParams params = { 0 };
        aarch64_impl_params(ops, block_size, i, &params);
        SwsFuncPtr func = ff_sws_aarch64_find_op(&params);
        if (!func) {
            ff_sws_op_chain_free(chain);
            // TODO print message to regenerate ops_entries
            return AVERROR(ENOTSUP);
        }
        SwsOpPriv priv = { 0 };
        // aarch64_setup(ops, block_size, i, &priv);
        ret = ff_sws_op_chain_append(chain, func, NULL, &priv);
        if (ret < 0) {
            ff_sws_op_chain_free(chain);
            return ret;
        }
    }

    const SwsOp *read  = ff_sws_op_list_input(ops);
    const SwsOp *write = ff_sws_op_list_output(ops);
    const int read_planes  = read ? (read->rw.packed ? 1 : read->rw.elems) : 0;
    const int write_planes = write->rw.packed ? 1 : write->rw.elems;
    SwsAArch64OpMask mask = 0;
    for (int i = 0; i < FFMAX(read_planes, write_planes); i++)
        mask |= (1 << (i << 2));

    SwsAArch64OpImplParams process_params = { .op = AARCH64_SWS_OP_PROCESS,        .mask = mask };
    SwsAArch64OpImplParams return_params  = { .op = AARCH64_SWS_OP_PROCESS_RETURN, .mask = mask };
    SwsFuncPtr process_func = ff_sws_aarch64_find_op(&process_params);
    SwsFuncPtr return_func  = ff_sws_aarch64_find_op(&return_params);
    if (!process_func || !return_func) {
        ff_sws_op_chain_free(chain);
        // TODO print message to regenerate ops_entries
        return AVERROR(ENOTSUP);
    }

    ret = ff_sws_op_chain_append(chain, return_func, NULL, &(SwsOpPriv) {0});
    if (ret < 0) {
        ff_sws_op_chain_free(chain);
        return ret;
    }

    out->func       = (SwsOpFunc) process_func;
    out->cpu_flags  = chain->cpu_flags;
    out->over_read  = chain->over_read;
    out->over_write = chain->over_write;
    return 0;
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
        mask |= (1 << (i << 2));
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

    // TODO optimize

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
