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

#define UOPSIE 0

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

#if UOPSIE
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
#endif

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

#if 0
typedef struct SwsAArch64OpImplParams {
    SwsUOpType          uop;
    SwsAArch64OpMask    mask;
    SwsPixelType        type;
    uint8_t block_size;
    union {
        uint8_t             shift;
        SwsAArch64MoveOp    move;
        SwsAArch64OpMask    pack;
        SwsAArch64LinearOp  linear;
        SwsAArch64DitherOp  dither;
    };
} SwsAArch64OpImplParams;
typedef enum SwsPixelType {
    SWS_PIXEL_NONE = 0,
    SWS_PIXEL_U8,
    SWS_PIXEL_U16,
    SWS_PIXEL_U32,
    SWS_PIXEL_F32,
    SWS_PIXEL_TYPE_NB
} SwsPixelType;
#endif
static void serialize_op(char *buf, size_t size, const SwsAArch64OpImplParams *params)
{
#if 0
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
#else
    // f32_linear_xyzw_xxx0x_xxx0x_xxx0x_000x0_8
    const SwsAArch64OpImplParams *p = params;
    switch (p->type) {
    case SWS_PIXEL_U8:  buf_appendf(&buf, &size, "u8_");  break;
    case SWS_PIXEL_U16: buf_appendf(&buf, &size, "u16_"); break;
    case SWS_PIXEL_U32: buf_appendf(&buf, &size, "u32_"); break;
    case SWS_PIXEL_F32: buf_appendf(&buf, &size, "f32_"); break;
    }
    switch (p->uop) {
    case SWS_UOP_READ_PLANAR:        buf_appendf(&buf, &size, "read_planar");        break;
    case SWS_UOP_READ_PLANAR_FH:     buf_appendf(&buf, &size, "read_planar_fh");     break;
    case SWS_UOP_READ_PLANAR_FV:     buf_appendf(&buf, &size, "read_planar_fv");     break;
    case SWS_UOP_READ_PLANAR_FV_FMA: buf_appendf(&buf, &size, "read_planar_fv_fma"); break;
    case SWS_UOP_READ_PACKED:        buf_appendf(&buf, &size, "read_packed");        break;
    case SWS_UOP_READ_NIBBLE:        buf_appendf(&buf, &size, "read_nibble");        break;
    case SWS_UOP_READ_BIT:           buf_appendf(&buf, &size, "read_bit");           break;
    case SWS_UOP_WRITE_PLANAR:       buf_appendf(&buf, &size, "write_planar");       break;
    case SWS_UOP_WRITE_PACKED:       buf_appendf(&buf, &size, "write_packed");       break;
    case SWS_UOP_WRITE_NIBBLE:       buf_appendf(&buf, &size, "write_nibble");       break;
    case SWS_UOP_WRITE_BIT:          buf_appendf(&buf, &size, "write_bit");          break;
    case SWS_UOP_PERMUTE:            buf_appendf(&buf, &size, "permute");            break;
    case SWS_UOP_COPY:               buf_appendf(&buf, &size, "copy");               break;
    case SWS_UOP_MOVE:               buf_appendf(&buf, &size, "move");               break;
    case SWS_UOP_SWAP_BYTES:         buf_appendf(&buf, &size, "swap_bytes");         break;
    case SWS_UOP_EXPAND_BIT:         buf_appendf(&buf, &size, "expand_bit");         break;
    case SWS_UOP_EXPAND_PAIR:        buf_appendf(&buf, &size, "expand_pair");        break;
    case SWS_UOP_EXPAND_QUAD:        buf_appendf(&buf, &size, "expand_quad");        break;
    case SWS_UOP_TO_U8:              buf_appendf(&buf, &size, "to_u8");              break;
    case SWS_UOP_TO_U16:             buf_appendf(&buf, &size, "to_u16");             break;
    case SWS_UOP_TO_U32:             buf_appendf(&buf, &size, "to_u32");             break;
    case SWS_UOP_TO_F32:             buf_appendf(&buf, &size, "to_f32");             break;
    case SWS_UOP_SCALE:              buf_appendf(&buf, &size, "scale");              break;
    case SWS_UOP_ADD:                buf_appendf(&buf, &size, "add");                break;
    case SWS_UOP_MIN:                buf_appendf(&buf, &size, "min");                break;
    case SWS_UOP_MAX:                buf_appendf(&buf, &size, "max");                break;
    case SWS_UOP_UNPACK:             buf_appendf(&buf, &size, "unpack");             break;
    case SWS_UOP_PACK:               buf_appendf(&buf, &size, "pack");               break;
    case SWS_UOP_LSHIFT:             buf_appendf(&buf, &size, "lshift");             break;
    case SWS_UOP_RSHIFT:             buf_appendf(&buf, &size, "rshift");             break;
    case SWS_UOP_CLEAR:              buf_appendf(&buf, &size, "clear");              break;
    case SWS_UOP_LINEAR:             buf_appendf(&buf, &size, "linear");             break;
    case SWS_UOP_LINEAR_FMA:         buf_appendf(&buf, &size, "linear_fma");         break;
    case SWS_UOP_DITHER:             buf_appendf(&buf, &size, "dither");             break;
    }
    if (p->mask) {
        buf_appendf(&buf, &size, "_");
        LOOP(p->mask, i) {
            buf_appendf(&buf, &size, "%c", "xyzw"[i]);
        }
    }
    switch (p->uop) {
    case SWS_UOP_READ_PLANAR:
        break;
    case SWS_UOP_READ_PLANAR_FH:
        break;
    case SWS_UOP_READ_PLANAR_FV:
        break;
    case SWS_UOP_READ_PLANAR_FV_FMA:
        break;
    case SWS_UOP_READ_PACKED:
        break;
    case SWS_UOP_READ_NIBBLE:
        break;
    case SWS_UOP_READ_BIT:
        break;
    case SWS_UOP_WRITE_PLANAR:
        break;
    case SWS_UOP_WRITE_PACKED:
        break;
    case SWS_UOP_WRITE_NIBBLE:
        break;
    case SWS_UOP_WRITE_BIT:
        break;
    case SWS_UOP_PERMUTE:
        break;
    case SWS_UOP_COPY:
        break;
    case SWS_UOP_MOVE:
        break;
    case SWS_UOP_SWAP_BYTES:
        break;
    case SWS_UOP_EXPAND_BIT:
        break;
    case SWS_UOP_EXPAND_PAIR:
        break;
    case SWS_UOP_EXPAND_QUAD:
        break;
    case SWS_UOP_TO_U8:
        break;
    case SWS_UOP_TO_U16:
        break;
    case SWS_UOP_TO_U32:
        break;
    case SWS_UOP_TO_F32:
        break;
    case SWS_UOP_SCALE:
        break;
    case SWS_UOP_ADD:
        break;
    case SWS_UOP_MIN:
        break;
    case SWS_UOP_MAX:
        break;
    case SWS_UOP_UNPACK:
    case SWS_UOP_PACK:
        buf_appendf(&buf, &size, "_");
        for (int i = 0; i < 4 && MASK_GET(p->pack, i); i++)
            buf_appendf(&buf, &size, "%x", MASK_GET(p->pack, i));
        break;
    case SWS_UOP_LSHIFT:
    case SWS_UOP_RSHIFT:
        buf_appendf(&buf, &size, "_%d", p->shift);
        break;
    case SWS_UOP_CLEAR:
        break;
    case SWS_UOP_LINEAR:
    case SWS_UOP_LINEAR_FMA:
#if 1
        LOOP(p->mask, i) {
            buf_appendf(&buf, &size, "_");
            for (int j = 0; j < 5; j++) {
                int val = LINEAR_MASK_GET(p->linear.mask, i, j);
                if (val == 1)
                    buf_appendf(&buf, &size, "1");
                else if (val == 0)
                    buf_appendf(&buf, &size, "0");
#if 0
                else if (par->lin.exact & SWS_MASK(i, j))
                    buf_appendf(&buf, &size, "X");
#endif
                else
                    buf_appendf(&buf, &size, "x");
            }
        }
#else
        for (int i = 0; i < 4; i++) {
            if (!SWS_COMP_TEST(op->mask, i))
                continue;
            av_bprint_chars(&bp, '_', 1);
            for (int j = 0; j < 5; j++) {
                if (par->lin.one & SWS_MASK(i, j))
                    av_bprint_chars(&bp, '1', 1);
                else if (par->lin.zero & SWS_MASK(i, j))
                    av_bprint_chars(&bp, '0', 1);
                else if (par->lin.exact & SWS_MASK(i, j))
                    av_bprint_chars(&bp, 'X', 1);
                else
                    av_bprint_chars(&bp, 'x', 1);
            }
        }
#endif
        break;
    case SWS_UOP_DITHER:
        LOOP(p->mask, i) {
            buf_appendf(&buf, &size, "_%d", MASK_GET(p->dither.y_offset, i));
        }
        buf_appendf(&buf, &size, "_16x16");
        break;
    }
    buf_appendf(&buf, &size, "_%d", p->block_size);
#endif
    av_assert0(size && "string buffer exhausted");
}

#if 0
void ff_sws_uop_name(const SwsUOp *op, char buf[SWS_UOP_NAME_MAX])
{
    AVBPrint bp;
    av_bprint_init_for_buffer(&bp, buf, SWS_UOP_NAME_MAX);

    if (op->type != SWS_PIXEL_NONE)
        av_bprintf(&bp, "%s_", ff_sws_pixel_type_name(op->type));
    av_bprintf(&bp, "%s", uop_names[op->uop].abbr);

    if (op->mask) {
        av_bprint_chars(&bp, '_', 1);
        for (int i = 0; i < 4; i++) {
            if (SWS_COMP_TEST(op->mask, i))
                av_bprint_chars(&bp, "xyzw"[i], 1);
        }
    }

    const SwsUOpParams *par = &op->par;
    switch (op->uop) {
    case SWS_UOP_READ_PLANAR_FH:
    case SWS_UOP_READ_PLANAR_FV:
    case SWS_UOP_READ_PLANAR_FV_FMA:
        av_bprintf(&bp, "_%s", ff_sws_pixel_type_name(par->filter.type));
        break;
    case SWS_UOP_LSHIFT:
    case SWS_UOP_RSHIFT:
        av_bprintf(&bp, "_%u", par->shift.amount);
        break;
    case SWS_UOP_PERMUTE:
    case SWS_UOP_COPY:
        av_bprint_chars(&bp, '_', 1);
        for (int i = 0; i < 4; i++) {
            if (SWS_COMP_TEST(op->mask, i))
                av_bprint_chars(&bp, "xyzw"[par->swizzle.in[i]], 1);
        }
        break;
    case SWS_UOP_MOVE:
        av_bprint_chars(&bp, '_', 1);
        for (int i = 0; i < par->move.num_moves; i++)
            av_bprint_chars(&bp, "txyzw"[par->move.dst[i] + 1], 1);
        av_bprint_chars(&bp, '_', 1);
        for (int i = 0; i < par->move.num_moves; i++)
            av_bprint_chars(&bp, "txyzw"[par->move.src[i] + 1], 1);
        break;
    case SWS_UOP_PACK:
    case SWS_UOP_UNPACK:
        av_bprint_chars(&bp, '_', 1);
        for (int i = 0; i < 4 && par->pack.pattern[i]; i++)
            av_bprintf(&bp, "%x", par->pack.pattern[i]);
        break;
    case SWS_UOP_CLEAR:
        av_bprint_chars(&bp, '_', 1);
        for (int i = 0; i < 4; i++) {
            if (!SWS_COMP_TEST(op->mask, i))
                continue;
            else if (SWS_COMP_TEST(par->clear.one, i))
                av_bprint_chars(&bp, '1', 1);
            else if (SWS_COMP_TEST(par->clear.zero, i))
                av_bprint_chars(&bp, '0', 1);
            else
                av_bprint_chars(&bp, 'x', 1);
        }
        break;
    case SWS_UOP_LINEAR:
    case SWS_UOP_LINEAR_FMA:
        for (int i = 0; i < 4; i++) {
            if (!SWS_COMP_TEST(op->mask, i))
                continue;
            av_bprint_chars(&bp, '_', 1);
            for (int j = 0; j < 5; j++) {
                if (par->lin.one & SWS_MASK(i, j))
                    av_bprint_chars(&bp, '1', 1);
                else if (par->lin.zero & SWS_MASK(i, j))
                    av_bprint_chars(&bp, '0', 1);
                else if (par->lin.exact & SWS_MASK(i, j))
                    av_bprint_chars(&bp, 'X', 1);
                else
                    av_bprint_chars(&bp, 'x', 1);
            }
        }
        break;
    case SWS_UOP_DITHER:
        for (int i = 0; i < 4; i++) {
            if (SWS_COMP_TEST(op->mask, i))
                av_bprintf(&bp, "_%d", par->dither.y_offset[i]);
        }
        const unsigned size = 1u << par->dither.size_log2;
        av_bprintf(&bp, "_%ux%u", size, size);
        break;
    }

    av_assert0(av_bprint_is_complete(&bp));
}
#endif

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
#if 0
    printf("/*\n");
    printf(" * This file is automatically generated. Do not edit manually.\n");
    printf(" * To regenerate, run: make sws_ops_entries_aarch64\n");
    printf(" */\n");
    printf("\n");
#endif
    av_tree_enumerate(roots.op, stdout, NULL, print_op);
#if UOPSIE
    printf("UOPS\n");
    av_tree_enumerate(roots.uop, stdout, NULL, print_uop);
#endif

fail:
    av_tree_destroy(roots.op);
    av_tree_destroy(roots.uop);
    sws_free_context(&ctx);
    return ret;
}
