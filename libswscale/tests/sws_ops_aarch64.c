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

static uint16_t dither_to_mask(const SwsDitherUOp *dither)
{
    uint16_t mask = 0;
    for (int i = 0; i < 4; i++)
        mask |= dither->y_offset[i] << (i << 2);
    return mask;
}

static int aarch64_op_impl_cmp(const void *a, const void *b)
{
    const SwsAArch64OpImplParams *pa = (const SwsAArch64OpImplParams *) a;
    const SwsAArch64OpImplParams *pb = (const SwsAArch64OpImplParams *) b;
    const SwsUOpParams *para = &pa->par;
    const SwsUOpParams *parb = &pb->par;

    if (pa->uop != pb->uop)
        return (int) pa->uop - pb->uop;

    switch (pa->uop) {
    case SWS_UOP_MOVE: {
        uint64_t ia = move_to_mask(&para->move);
        uint64_t ib = move_to_mask(&parb->move);
        if (ia != ib)
            return (int64_t) (ia - ib) < 0 ? -1 : 1;
        break;
    }
    case SWS_UOP_UNPACK:
    case SWS_UOP_PACK: {
        uint16_t ia = pack_to_mask(&para->pack);
        uint16_t ib = pack_to_mask(&parb->pack);
        if (ia != ib)
            return (int) ia - ib;
        break;
    }
    case SWS_UOP_LSHIFT:
    case SWS_UOP_RSHIFT:
        if (para->shift.amount != parb->shift.amount)
            return (int) para->shift.amount - parb->shift.amount;
        break;
    case SWS_UOP_CLEAR: {
        uint16_t ia = clear_to_mask(&para->clear);
        uint16_t ib = clear_to_mask(&parb->clear);
        if (ia != ib)
            return (int) ia - ib;
        break;
    }
    case SWS_UOP_LINEAR:
    case SWS_UOP_LINEAR_FMA: {
        uint64_t ia = linear_to_mask(&para->lin);
        uint64_t ib = linear_to_mask(&parb->lin);
        if (ia != ib)
            return (int64_t) (ia - ib) < 0 ? -1 : 1;
        break;
    }
    case SWS_UOP_DITHER: {
        uint16_t ia = dither_to_mask(&para->dither);
        uint16_t ib = dither_to_mask(&parb->dither);
        if (ia != ib)
            return (int) ia - ib;
        if (para->dither.size_log2 != parb->dither.size_log2)
            return (int) para->dither.size_log2 - parb->dither.size_log2;
        break;
    }
    }

    if (pa->block_size != pb->block_size)
        return (int) pa->block_size - pb->block_size;
    if (pa->type != pb->type)
        return (int) pa->type - pb->type;
    if (pa->mask != pb->mask)
        return (int) pa->mask - pb->mask;

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
#endif

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

static void impl_func_name(AVBPrint *bp, const SwsAArch64OpImplParams *params)
{
    const SwsUOpParams *par = &params->par;
    av_bprintf(bp, "ff_sws_%s", op_type_names[params->uop]);
    switch (params->uop) {
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
        av_bprintf(bp, "_%04x_%u", dither_to_mask(&par->dither), par->dither.size_log2);
        break;
    }
    uint16_t mask16 = 0;
    for (int i = 0; i < 4; i++)
        mask16 |= !!(params->mask & SWS_COMP(i)) << (i << 2);
    av_bprintf(bp, "_%u_%s_%04x_neon", params->block_size, pixel_type_names[params->type], mask16);
}

static void serialize_op(AVBPrint *bp, const SwsAArch64OpImplParams *params)
{
#if 0
    av_bprintf(bp, "ENTRY(");
    impl_func_name(&buf, &size, params);
    av_bprintf(bp, ", {");
    const ParamField **fields = op_fields[params->uop];
    for (int i = 0; fields[i]; i++) {
        const ParamField *field = fields[i];
        void *p = (void *) (((uintptr_t) params) + field->offset);
        if (i)
            av_bprintf(bp, ",");
        av_bprintf(bp, " .%s = ", field->name);
        field->print_val(&buf, &size, p);
    }
    av_bprintf(bp, " })");
#else
    // f32_linear_xyzw_xxx0x_xxx0x_xxx0x_000x0_8
    const SwsAArch64OpImplParams *p = params;
    switch (p->type) {
    case SWS_PIXEL_U8:  av_bprintf(bp, "u8_");  break;
    case SWS_PIXEL_U16: av_bprintf(bp, "u16_"); break;
    case SWS_PIXEL_U32: av_bprintf(bp, "u32_"); break;
    case SWS_PIXEL_F32: av_bprintf(bp, "f32_"); break;
    }
    switch (p->uop) {
    case SWS_UOP_READ_PLANAR:        av_bprintf(bp, "read_planar");        break;
    case SWS_UOP_READ_PLANAR_FH:     av_bprintf(bp, "read_planar_fh");     break;
    case SWS_UOP_READ_PLANAR_FV:     av_bprintf(bp, "read_planar_fv");     break;
    case SWS_UOP_READ_PLANAR_FV_FMA: av_bprintf(bp, "read_planar_fv_fma"); break;
    case SWS_UOP_READ_PACKED:        av_bprintf(bp, "read_packed");        break;
    case SWS_UOP_READ_NIBBLE:        av_bprintf(bp, "read_nibble");        break;
    case SWS_UOP_READ_BIT:           av_bprintf(bp, "read_bit");           break;
    case SWS_UOP_WRITE_PLANAR:       av_bprintf(bp, "write_planar");       break;
    case SWS_UOP_WRITE_PACKED:       av_bprintf(bp, "write_packed");       break;
    case SWS_UOP_WRITE_NIBBLE:       av_bprintf(bp, "write_nibble");       break;
    case SWS_UOP_WRITE_BIT:          av_bprintf(bp, "write_bit");          break;
    case SWS_UOP_PERMUTE:            av_bprintf(bp, "permute");            break;
    case SWS_UOP_COPY:               av_bprintf(bp, "copy");               break;
    case SWS_UOP_MOVE:               av_bprintf(bp, "move");               break;
    case SWS_UOP_SWAP_BYTES:         av_bprintf(bp, "swap_bytes");         break;
    case SWS_UOP_EXPAND_BIT:         av_bprintf(bp, "expand_bit");         break;
    case SWS_UOP_EXPAND_PAIR:        av_bprintf(bp, "expand_pair");        break;
    case SWS_UOP_EXPAND_QUAD:        av_bprintf(bp, "expand_quad");        break;
    case SWS_UOP_TO_U8:              av_bprintf(bp, "to_u8");              break;
    case SWS_UOP_TO_U16:             av_bprintf(bp, "to_u16");             break;
    case SWS_UOP_TO_U32:             av_bprintf(bp, "to_u32");             break;
    case SWS_UOP_TO_F32:             av_bprintf(bp, "to_f32");             break;
    case SWS_UOP_SCALE:              av_bprintf(bp, "scale");              break;
    case SWS_UOP_ADD:                av_bprintf(bp, "add");                break;
    case SWS_UOP_MIN:                av_bprintf(bp, "min");                break;
    case SWS_UOP_MAX:                av_bprintf(bp, "max");                break;
    case SWS_UOP_UNPACK:             av_bprintf(bp, "unpack");             break;
    case SWS_UOP_PACK:               av_bprintf(bp, "pack");               break;
    case SWS_UOP_LSHIFT:             av_bprintf(bp, "lshift");             break;
    case SWS_UOP_RSHIFT:             av_bprintf(bp, "rshift");             break;
    case SWS_UOP_CLEAR:              av_bprintf(bp, "clear");              break;
    case SWS_UOP_LINEAR:             av_bprintf(bp, "linear");             break;
    case SWS_UOP_LINEAR_FMA:         av_bprintf(bp, "linear_fma");         break;
    case SWS_UOP_DITHER:             av_bprintf(bp, "dither");             break;
    }
    if (p->mask && p->uop != SWS_UOP_MOVE) {
        av_bprintf(bp, "_");
        LOOP(p->mask, i) {
            av_bprintf(bp, "%c", "xyzw"[i]);
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
        {
#if 0
            SwsAArch64MoveOp move = p->move;
            char src[8] = { 0 };
            char dst[8] = { 0 };
            char *psrc = src;
            char *pdst = dst;
            char chars[16] = { 0 };
            chars[0] = 'x';
            chars[1] = 'y';
            chars[2] = 'z';
            chars[3] = 'w';
            chars[0xf] = 't';
            while (move) {
                *psrc++ = chars[move & 0xf];
                move >>= 4;
                *pdst++ = chars[move & 0xf];
                move >>= 4;
            }
            av_bprintf(bp, "_%s_%s", dst, src);
#else
        char src[8] = { 0 };
        char dst[8] = { 0 };
        char *psrc = src;
        char *pdst = dst;
        char chars[16] = { 0 };
        chars[0] = 't';
        chars[1] = 'x';
        chars[2] = 'y';
        chars[3] = 'z';
        chars[4] = 'w';
        for (int i = 0; i < p->par.move.num_moves; i++) {
            *psrc++ = chars[p->par.move.src[i] + 1];
            *pdst++ = chars[p->par.move.dst[i] + 1];
        }
        av_bprintf(bp, "_%s_%s", dst, src);
#endif
        }
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
        av_bprintf(bp, "_");
        for (int i = 0; i < 4 && p->par.pack.pattern[i]; i++)
            av_bprintf(bp, "%x", p->par.pack.pattern[i]);
        break;
    case SWS_UOP_LSHIFT:
    case SWS_UOP_RSHIFT:
        av_bprintf(bp, "_%u", p->par.shift.amount);
        break;
    case SWS_UOP_CLEAR:
        av_bprintf(bp, "_");
        LOOP(p->mask, i) {
            if (SWS_COMP_TEST(p->par.clear.zero, i)) {
                av_bprintf(bp, "0");
            } else if (SWS_COMP_TEST(p->par.clear.one, i)) {
                av_bprintf(bp, "1");
            } else {
                av_bprintf(bp, "x");
            }
        }
        break;
    case SWS_UOP_LINEAR:
    case SWS_UOP_LINEAR_FMA:
        LOOP(p->mask, i) {
            av_bprintf(bp, "_");
            for (int j = 0; j < 5; j++) {
                // int jj = (j == 4) ? 0 : j + 1;
                int jj = j;
                if (p->par.lin.zero & SWS_MASK(i, jj)) {
                    av_bprintf(bp, "0");
                } else if (p->par.lin.one & SWS_MASK(i, jj)) {
                    av_bprintf(bp, "1");
                } else {
                    av_bprintf(bp, "x");
                }
            }
        }
        break;
    case SWS_UOP_DITHER:
        LOOP(p->mask, i) {
            av_bprintf(bp, "_%x", p->par.dither.y_offset[i]);
        }
        av_bprintf(bp, "_16x16");
        break;
    }
    av_bprintf(bp, "_%d", p->block_size);
#endif
}

/* Serialize SwsAArch64OpImplParams for one function. */
static int print_op(void *opaque, void *elem)
{
    SwsAArch64OpImplParams *params = (SwsAArch64OpImplParams *) elem;
    FILE *fp = (FILE *) opaque;

    AVBPrint bp;
    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
    serialize_op(&bp, params);
    fprintf(fp, "%s\n", bp.str);
    av_bprint_finalize(&bp, NULL);

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
    printf(" * To regenerate, run: make fate-sws-ops-entries-aarch64 GEN=1\n");
    printf(" */\n");
    printf("\n");
#endif

#if UOPSIE
    printf("UOPS\n");
    av_tree_enumerate(roots.uop, stdout, NULL, print_uop);
#else
    av_tree_enumerate(roots.op, stdout, NULL, print_op);
#endif

fail:
    av_tree_destroy(roots.op);
    av_tree_destroy(roots.uop);
    sws_free_context(&ctx);
    return ret;
}
