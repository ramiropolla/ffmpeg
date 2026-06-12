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

#include <stdio.h>

#include "libavutil/mem.h"
#include "libavutil/tree.h"
#include "libswscale/ops.h"
#include "libswscale/ops_chain.h"

#include "libswscale/aarch64/ops_impl.c"
#include "libswscale/aarch64/ops_impl_conv.c"

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

typedef struct roots_t {
    struct AVTreeNode *op;
    struct AVTreeNode *uop;
} roots_t;

typedef struct SwsUOpWithBlockSize {
    SwsUOp uop;
    int block_size;
} SwsUOpWithBlockSize;

/*********************************************************************/
static int aarch64_op_impl_cmp(const void *a, const void *b)
{
    const SwsAArch64OpImplParams *pa = (const SwsAArch64OpImplParams *) a;
    const SwsAArch64OpImplParams *pb = (const SwsAArch64OpImplParams *) b;

    const ParamField **fields = op_fields[pa->uop];
    for (int i = 0; fields[i]; i++) {
        const ParamField *field = fields[i];
        int diff = field->cmp_val((void  *) (((uintptr_t) pa) + field->offset),
                                  (void  *) (((uintptr_t) pb) + field->offset));
        if (diff)
            return diff;
    }
    return 0;
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
    av_tree_insert(root, copy, aarch64_op_impl_cmp, &node);
    if (!node)
        copy = NULL;

error:
    av_free(node);
    av_free(copy);
    return ret;
}

/*********************************************************************/
/* Insert the SwsUOp structure into the AVTreeNode. */

static int ff_sws_uopbs_cmp(const void *a, const void *b)
{
    SwsUOpWithBlockSize *pa = (SwsUOpWithBlockSize *) a;
    SwsUOpWithBlockSize *pb = (SwsUOpWithBlockSize *) b;
    if (pa->block_size != pb->block_size)
        return pa->block_size - pb->block_size;
    return ff_sws_uop_cmp(&pa->uop, &pb->uop);
}

static int aarch64_collect_uop(const SwsUOp *uop, struct AVTreeNode **root, int block_size)
{
    int ret = 0;

    struct AVTreeNode *node = av_tree_node_alloc();
    SwsUOpWithBlockSize *copy = av_malloc(sizeof(SwsUOpWithBlockSize));
    if (!node || !copy) {
        ret = AVERROR(ENOMEM);
        goto error;
    }
    memcpy(&copy->uop, uop, sizeof(SwsUOp));
    copy->block_size = block_size;
    av_tree_insert(root, copy, ff_sws_uopbs_cmp, &node);
    if (!node)
        copy = NULL;

error:
    av_free(node);
    av_free(copy);
    return ret;
}

static int register_op(SwsContext *ctx, void *opaque, SwsOpList *ops)
{
    roots_t *roots = (roots_t *) opaque;
    struct AVTreeNode **root = &roots->op;
    struct AVTreeNode **root_uop = &roots->uop;
    int ret;

    /* Skip ops lists which include filtering, since this is still not
     * supported. */
    for (int i = 0; i < ops->num_ops; i++) {
        const SwsOp *op = &ops->ops[i];
        switch (op->op) {
        case SWS_OP_READ:
        case SWS_OP_WRITE:
            if (op->rw.filter.op)
                return 0;
            break;
        case SWS_OP_FILTER_H:
        case SWS_OP_FILTER_V:
            return 0;
        }
    }

    /* Use at most two full vregs during the widest precision section */
    int block_size = (ff_sws_op_list_max_size(ops) == 4) ? 8 : 16;

    for (int i = 0; i < ops->num_ops; i++) {
        SwsAArch64OpImplParams params = { 0 };
        ret = convert_to_aarch64_impl(ctx, ops, i, block_size, &params);
        if (ret == AVERROR(ENOTSUP))
            continue;
        if (ret < 0)
            goto end;
        ret = aarch64_collect_op(&params, root);
        if (ret < 0)
            goto end;
        if (params.uop == SWS_UOP_LINEAR_FMA) {
            /**
             * Generate both sets of linear op functions that do use
             * and do not use fmla (selected by SWS_BITEXACT).
             */
            params.uop = SWS_UOP_LINEAR;
            params.linear.fmla = !params.linear.fmla;
            ret = aarch64_collect_op(&params, root);
            if (ret < 0)
                goto end;
        }
    }

    SwsUOpFlags flags[] = {
        SWS_UOP_FLAG_MOVE,
        SWS_UOP_FLAG_MOVE | SWS_UOP_FLAG_FMA,
    };

    for (int f = 0; f < FF_ARRAY_ELEMS(flags); f++) {
        SwsUOpList *uops = ff_sws_uop_list_alloc();
        if (!uops) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        ret = ff_sws_ops_translate(ctx, ops, flags[f], uops);
        if (ret == AVERROR(ENOTSUP)) {
            ff_sws_uop_list_free(&uops);
            continue;
        }
        if (ret < 0) {
            ff_sws_uop_list_free(&uops);
            goto end;
        }

        for (int i = 0; i < uops->num_ops; i++) {
            ret = aarch64_collect_uop(&uops->ops[i], root_uop, block_size);
            if (ret < 0)
                goto end;
        }

        ff_sws_uop_list_free(&uops);
    }

    ret = 0;

end:
    return ret;
}

/*********************************************************************/
static void impl_func_name(char **buf, size_t *size, const SwsAArch64OpImplParams *params)
{
    buf_appendf(buf, size, "ff_sws");
    const ParamField **fields = op_fields[params->uop];
    for (int i = 0; fields[i]; i++) {
        const ParamField *field = fields[i];
        void *p = (void *) (((uintptr_t) params) + field->offset);
        field->print_str(buf, size, p);
    }
    buf_appendf(buf, size, "_neon");
}

static void serialize_op(char *buf, size_t size, const SwsAArch64OpImplParams *params)
{
    buf_appendf(&buf, &size, "ENTRY(");
    impl_func_name(&buf, &size, params);
    buf_appendf(&buf, &size, ", {");
    const ParamField **fields = op_fields[params->uop];
    for (int i = 0; fields[i]; i++) {
        const ParamField *field = fields[i];
        void *p = (void *) (((uintptr_t) params) + field->offset);
        if (i)
            buf_appendf(&buf, &size, ",");
        buf_appendf(&buf, &size, " .%s = ", field->name);
        field->print_val(&buf, &size, p);
    }
    buf_appendf(&buf, &size, " })");
    av_assert0(size && "string buffer exhausted");
}

/* Serialize SwsAArch64OpImplParams for one function. */
static int print_op(void *opaque, void *elem)
{
    SwsAArch64OpImplParams *params = (SwsAArch64OpImplParams *) elem;
    FILE *fp = (FILE *) opaque;

    char buf[256];
    serialize_op(buf, sizeof(buf), params);
    fprintf(fp, "%s\n", buf);

    av_free(params);

    return 0;
}

/*********************************************************************/
/* Serialize SwsUOp for one uop. */
static int print_uop(void *opaque, void *elem)
{
    SwsUOpWithBlockSize *uopbs = (SwsUOpWithBlockSize *) elem;
    FILE *fp = (FILE *) opaque;

    char buf[SWS_UOP_NAME_MAX];
    ff_sws_uop_name(&uopbs->uop, buf);
    fprintf(fp, "%s_%d\n", buf, uopbs->block_size);

    av_free(uopbs);

    return 0;
}

/*********************************************************************/
int main(int argc, char *argv[])
{
    roots_t roots = { 0 };
    int ret = 1;

#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    SwsContext *ctx = sws_alloc_context();
    if (!ctx)
        goto fail;

    ret = ff_sws_enum_op_lists(ctx, &roots, AV_PIX_FMT_NONE, AV_PIX_FMT_NONE,
                               register_op);

    /**
     * Generate a C file with all the unique function parameter entries
     * collected by aarch64_enum_ops().
     */
    printf("/*\n");
    printf(" * This file is automatically generated. Do not edit manually.\n");
    printf(" * To regenerate, run: make sws_ops_entries_aarch64\n");
    printf(" */\n");
    printf("\n");
    av_tree_enumerate(roots.op, stdout, NULL, print_op);
    av_tree_enumerate(roots.uop, stdout, NULL, print_uop);

fail:
    av_tree_destroy(roots.op);
    av_tree_destroy(roots.uop);
    sws_free_context(&ctx);
    return ret;
}
