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

#include "libavutil/avassert.h"
#include "libavutil/bprint.h"
#include "libavutil/mem.h"
#include "libavutil/tree.h"
#include "libswscale/ops.h"
#include "libswscale/ops_chain.h"

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
static uint16_t clear_to_mask(const SwsClearUOp *clear)
{
    uint16_t mask = 0;
    for (int i = 0; i < 4; i++) {
        if (SWS_COMP_TEST(clear->zero, i)) {
            /* no-op */
        } else if (SWS_COMP_TEST(clear->one, i)) {
            mask |= 1 << (i << 2);
        } else {
            mask |= 0xf << (i << 2);
        }
    }
    return mask;
}

static uint64_t move_to_mask(const SwsMoveUOp *move)
{
    uint64_t mask = 0;
    for (int i = 0; i < move->num_moves; i++) {
        uint8_t dst = move->dst[i] < 0 ? 0xf : move->dst[i];
        uint8_t src = move->src[i] < 0 ? 0xf : move->src[i];
        uint64_t pair = src | (dst << 4);
        mask |= pair << (i * 8);
    }
    return mask;
}

static uint16_t pack_to_mask(const SwsPackUOp *pack)
{
    uint16_t mask = 0;
    for (int i = 0; i < 4; i++)
        mask |= pack->pattern[i] << (i << 2);
    return mask;
}

static uint64_t linear_to_mask(const SwsLinearUOp *linear)
{
    uint64_t mask = 0;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 5; j++) {
            int jj = (j == 0) ? 4 : (j - 1);
            if (linear->one & SWS_MASK(i, jj))
                mask |= 1ULL << (2 * ((5 * i + j)));
            else if (!(linear->zero & SWS_MASK(i, jj)))
                mask |= 3ULL << (2 * ((5 * i + j)));
        }
    }
    return mask;
}

static uint16_t dither_to_mask(const SwsDitherUOp *dither, SwsCompMask omask)
{
    uint16_t mask = 0;
    for (int i = 0; i < 4; i++)
        if (omask & SWS_COMP(i))
            mask |= dither->y_offset[i] << (i << 2);
        else
            mask |= 0xf << (i << 2);
    return mask;
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
            int cur_block_size = block_size;
            SwsUOp *uop = &uops->ops[i];
            switch (uop->uop) {
            case SWS_UOP_MOVE:
                /* The element size and type don't matter. */
                cur_block_size = cur_block_size * ff_sws_pixel_type_size(uop->type);
                uop->type = SWS_PIXEL_U8;
                break;
            }
            ret = aarch64_collect_uop(uop, root_uop, cur_block_size);
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
static const char op_type_names[SWS_UOP_TYPE_NB][16] = {
    [SWS_UOP_READ_BIT      ] = "read_bit",
    [SWS_UOP_READ_NIBBLE   ] = "read_nibble",
    [SWS_UOP_READ_PACKED   ] = "read_packed",
    [SWS_UOP_READ_PLANAR   ] = "read_planar",
    [SWS_UOP_WRITE_BIT     ] = "write_bit",
    [SWS_UOP_WRITE_NIBBLE  ] = "write_nibble",
    [SWS_UOP_WRITE_PACKED  ] = "write_packed",
    [SWS_UOP_WRITE_PLANAR  ] = "write_planar",
    [SWS_UOP_SWAP_BYTES    ] = "swap_bytes",
    [SWS_UOP_MOVE          ] = "move",
    [SWS_UOP_UNPACK        ] = "unpack",
    [SWS_UOP_PACK          ] = "pack",
    [SWS_UOP_LSHIFT        ] = "lshift",
    [SWS_UOP_RSHIFT        ] = "rshift",
    [SWS_UOP_CLEAR         ] = "clear",
    [SWS_UOP_TO_U8         ] = "to_u8",
    [SWS_UOP_TO_U16        ] = "to_u16",
    [SWS_UOP_TO_U32        ] = "to_u32",
    [SWS_UOP_TO_F32        ] = "to_f32",
    [SWS_UOP_EXPAND_PAIR   ] = "expand_pair",
    [SWS_UOP_EXPAND_QUAD   ] = "expand_quad",
    [SWS_UOP_MIN           ] = "min",
    [SWS_UOP_MAX           ] = "max",
    [SWS_UOP_SCALE         ] = "scale",
    [SWS_UOP_LINEAR        ] = "linear",
    [SWS_UOP_LINEAR_FMA    ] = "linear_fma",
    [SWS_UOP_DITHER        ] = "dither",
};

static const char pixel_type_names[SWS_PIXEL_TYPE_NB][4] = {
    [SWS_PIXEL_U8 ] = "u8",
    [SWS_PIXEL_U16] = "u16",
    [SWS_PIXEL_U32] = "u32",
    [SWS_PIXEL_F32] = "f32",
};

static void impl_func_name(AVBPrint *bp, const SwsUOpWithBlockSize *uopbs)
{
    const SwsUOpParams *par = &uopbs->uop.par;
    av_bprintf(bp, "ff_sws_%s", op_type_names[uopbs->uop.uop]);
    switch (uopbs->uop.uop) {
    case SWS_UOP_MOVE:
        av_bprintf(bp, "_%012" PRIx64, move_to_mask(&par->move));
        break;
    case SWS_UOP_UNPACK:
    case SWS_UOP_PACK:
        av_bprintf(bp, "_%04x", pack_to_mask(&par->pack));
        break;
    case SWS_UOP_LSHIFT:
    case SWS_UOP_RSHIFT:
        av_bprintf(bp, "_%u", par->shift.amount);
        break;
    case SWS_UOP_CLEAR:
        av_bprintf(bp, "_%04x", clear_to_mask(&par->clear));
        break;
    case SWS_UOP_LINEAR:
    case SWS_UOP_LINEAR_FMA:
        av_bprintf(bp, "_%010" PRIx64, linear_to_mask(&par->lin));
        break;
    case SWS_UOP_DITHER:
        av_bprintf(bp, "_%04x_%u", dither_to_mask(&par->dither, uopbs->uop.mask), par->dither.size_log2);
        break;
    }
    uint16_t mask16 = 0;
    for (int i = 0; i < 4; i++)
        mask16 |= !!(uopbs->uop.mask & SWS_COMP(i)) << (i << 2);
    av_bprintf(bp, "_%u_%s_%04x_neon", uopbs->block_size, pixel_type_names[uopbs->uop.type], mask16);
}

static const char op_types[SWS_UOP_TYPE_NB][32] = {
    [SWS_UOP_READ_BIT      ] = "SWS_UOP_READ_BIT",
    [SWS_UOP_READ_NIBBLE   ] = "SWS_UOP_READ_NIBBLE",
    [SWS_UOP_READ_PACKED   ] = "SWS_UOP_READ_PACKED",
    [SWS_UOP_READ_PLANAR   ] = "SWS_UOP_READ_PLANAR",
    [SWS_UOP_WRITE_BIT     ] = "SWS_UOP_WRITE_BIT",
    [SWS_UOP_WRITE_NIBBLE  ] = "SWS_UOP_WRITE_NIBBLE",
    [SWS_UOP_WRITE_PACKED  ] = "SWS_UOP_WRITE_PACKED",
    [SWS_UOP_WRITE_PLANAR  ] = "SWS_UOP_WRITE_PLANAR",
    [SWS_UOP_SWAP_BYTES    ] = "SWS_UOP_SWAP_BYTES",
    [SWS_UOP_MOVE          ] = "SWS_UOP_MOVE",
    [SWS_UOP_UNPACK        ] = "SWS_UOP_UNPACK",
    [SWS_UOP_PACK          ] = "SWS_UOP_PACK",
    [SWS_UOP_LSHIFT        ] = "SWS_UOP_LSHIFT",
    [SWS_UOP_RSHIFT        ] = "SWS_UOP_RSHIFT",
    [SWS_UOP_CLEAR         ] = "SWS_UOP_CLEAR",
    [SWS_UOP_TO_U8         ] = "SWS_UOP_TO_U8",
    [SWS_UOP_TO_U16        ] = "SWS_UOP_TO_U16",
    [SWS_UOP_TO_U32        ] = "SWS_UOP_TO_U32",
    [SWS_UOP_TO_F32        ] = "SWS_UOP_TO_F32",
    [SWS_UOP_EXPAND_PAIR   ] = "SWS_UOP_EXPAND_PAIR",
    [SWS_UOP_EXPAND_QUAD   ] = "SWS_UOP_EXPAND_QUAD",
    [SWS_UOP_MIN           ] = "SWS_UOP_MIN",
    [SWS_UOP_MAX           ] = "SWS_UOP_MAX",
    [SWS_UOP_SCALE         ] = "SWS_UOP_SCALE",
    [SWS_UOP_LINEAR        ] = "SWS_UOP_LINEAR",
    [SWS_UOP_LINEAR_FMA    ] = "SWS_UOP_LINEAR_FMA",
    [SWS_UOP_DITHER        ] = "SWS_UOP_DITHER",
};

static const char pixel_types[SWS_PIXEL_TYPE_NB][32] = {
    [SWS_PIXEL_U8 ] = "SWS_PIXEL_U8",
    [SWS_PIXEL_U16] = "SWS_PIXEL_U16",
    [SWS_PIXEL_U32] = "SWS_PIXEL_U32",
    [SWS_PIXEL_F32] = "SWS_PIXEL_F32",
};

/*********************************************************************/
/* Serialize SwsUOp for one uop. */
static int print_uop(void *opaque, void *elem)
{
    SwsUOpWithBlockSize *uopbs = (SwsUOpWithBlockSize *) elem;
    FILE *fp = (FILE *) opaque;

    AVBPrint bp;
    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
#if 0
    char buf[SWS_UOP_NAME_MAX];
    ff_sws_uop_name(&uopbs->uop, buf);
    fprintf(fp, "%s_%d\n", buf, uopbs->block_size);
#else
    const SwsUOpParams *par = &uopbs->uop.par;
    av_bprintf(&bp, "ENTRY(");
    impl_func_name(&bp, uopbs);
    av_bprintf(&bp, ", {");
    av_bprintf(&bp, " .uop = %s", op_types[uopbs->uop.uop]);
    switch (uopbs->uop.uop) {
    case SWS_UOP_MOVE:
        av_bprintf(&bp, ", .par.move = { .num_moves = %d, .dst = {%d, %d, %d, %d, %d, %d}, .src = {%d, %d, %d, %d, %d, %d} }",
                   par->move.num_moves,
                   par->move.dst[0], par->move.dst[1], par->move.dst[2],
                   par->move.dst[3], par->move.dst[4], par->move.dst[5],
                   par->move.src[0], par->move.src[1], par->move.src[2],
                   par->move.src[3], par->move.src[4], par->move.src[5]);
        break;
    case SWS_UOP_UNPACK:
    case SWS_UOP_PACK:
        av_bprintf(&bp, ", .par.pack = { .pattern = {%d, %d, %d, %d} }",
                   par->pack.pattern[0], par->pack.pattern[1],
                   par->pack.pattern[2], par->pack.pattern[3]);
        break;
    case SWS_UOP_LSHIFT:
    case SWS_UOP_RSHIFT:
        av_bprintf(&bp, ", .par.shift.amount = %u", par->shift.amount);
        break;
    case SWS_UOP_CLEAR:
        av_bprintf(&bp, ", .par.clear = { .one = 0x%0x, .zero = 0x%0x }", par->clear.one, par->clear.zero);
        break;
    case SWS_UOP_LINEAR:
    case SWS_UOP_LINEAR_FMA:
        av_bprintf(&bp, ", .par.lin = { .one = 0x%x, .zero = 0x%x }", par->lin.one, par->lin.zero);
        break;
    case SWS_UOP_DITHER:
        av_bprintf(&bp, ", .par.dither = { .y_offset = {%u, %u, %u, %u}, .size_log2 = %u }",
                   (par->dither.y_offset[0] == 0xf) ? 0 : par->dither.y_offset[0],
                   (par->dither.y_offset[1] == 0xf) ? 0 : par->dither.y_offset[1],
                   (par->dither.y_offset[2] == 0xf) ? 0 : par->dither.y_offset[2],
                   (par->dither.y_offset[3] == 0xf) ? 0 : par->dither.y_offset[3],
                   par->dither.size_log2);
        break;
    }
    av_bprintf(&bp, ", .block_size = %u, .type = %s, .mask = 0x%x })", uopbs->block_size, pixel_types[uopbs->uop.type], uopbs->uop.mask);
#endif

    fprintf(fp, "%s\n", bp.str);
    av_bprint_finalize(&bp, NULL);

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
    printf(" * To regenerate, run: make fate-sws-ops-entries-aarch64 GEN=1\n");
    printf(" */\n");
    printf("\n");

    av_tree_enumerate(roots.uop, stdout, NULL, print_uop);

fail:
    av_tree_destroy(roots.op);
    av_tree_destroy(roots.uop);
    sws_free_context(&ctx);
    return ret;
}
