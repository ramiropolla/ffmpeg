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
 * This file is used both by sws_ops_aarch64 to generate ops_entries.c and
 * by the standalone build-time tool that generates the static assembly
 * functions (aarch64/ops_asmgen). Therefore, it must not depend on internal
 * FFmpeg libraries.
 */

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

#include "libavutil/attributes.h"

/**
 * NOTE: ops_asmgen contains header redefinitions to provide av_assert0
 * while not depending on internal FFmpeg libraries.
 */
#include "libavutil/avassert.h"

#include "ops_impl.h"

/*********************************************************************/
static const char pixel_types[SWS_PIXEL_TYPE_NB][32] = {
    [SWS_PIXEL_U8 ] = "SWS_PIXEL_U8",
    [SWS_PIXEL_U16] = "SWS_PIXEL_U16",
    [SWS_PIXEL_U32] = "SWS_PIXEL_U32",
    [SWS_PIXEL_F32] = "SWS_PIXEL_F32",
};

static const char *aarch64_pixel_type(SwsPixelType fmt)
{
    if (fmt >= SWS_PIXEL_TYPE_NB) {
        av_assert0(!"Invalid pixel type!");
        return NULL;
    }
    return pixel_types[fmt];
}

static const char pixel_type_names[SWS_PIXEL_TYPE_NB][4] = {
    [SWS_PIXEL_U8 ] = "u8",
    [SWS_PIXEL_U16] = "u16",
    [SWS_PIXEL_U32] = "u32",
    [SWS_PIXEL_F32] = "f32",
};

static const char *aarch64_pixel_type_name(SwsPixelType fmt)
{
    if (fmt >= SWS_PIXEL_TYPE_NB) {
        av_assert0(!"Invalid pixel type!");
        return NULL;
    }
    return pixel_type_names[fmt];
}

/*********************************************************************/
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

static const char *aarch64_op_type(SwsUOpType op)
{
    if (op == SWS_UOP_INVALID || op >= SWS_UOP_TYPE_NB) {
        av_assert0(!"Invalid op type!");
        return NULL;
    }
    return op_types[op];
}

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

static const char *aarch64_op_type_name(SwsUOpType op)
{
    if (op == SWS_UOP_INVALID || op >= SWS_UOP_TYPE_NB) {
        av_assert0(!"Invalid op type!");
        return NULL;
    }
    return op_type_names[op];
}

/*********************************************************************/
/*
 * Helper string concatenation function that does not depend on the
 * FFmpeg libraries, so it may be used standalone.
 */
av_printf_format(3, 4)
static void buf_appendf(char **pbuf, size_t *prem, const char *fmt, ...)
{
    char *buf = *pbuf;
    size_t rem = *prem;
    if (!rem)
        return;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, rem, fmt, ap);
    va_end(ap);

    if (n > 0) {
        if (n < rem) {
            buf += n;
            rem -= n;
        } else {
            buf += rem - 1;
            rem = 0;
        }
        *pbuf = buf;
        *prem = rem;
    }
}

/*********************************************************************/
/**
 * The following structure is used to describe one field from
 * SwsAArch64OpImplParams. This will be used to serialize the parameter
 * structure, generate function names, and compare two sets of
 * parameters.
 */

typedef struct ParamField {
    const char *name;
    size_t offset;
    size_t size;
    void (*print_str)(char **pbuf, size_t *prem, void *p);
    void (*print_val)(char **pbuf, size_t *prem, void *p);
    int (*cmp_val)(void *pa, void *pb);
} ParamField;

#define PARAM_FIELD(name) #name, offsetof(SwsAArch64OpImplParams, name), sizeof(((SwsAArch64OpImplParams *) 0)->name)

static void print_uop_name(char **pbuf, size_t *prem, void *p)
{
    SwsUOpType op = *(SwsUOpType *) p;
    buf_appendf(pbuf, prem, "_%s", aarch64_op_type_name(op));
}

static void print_uop_val(char **pbuf, size_t *prem, void *p)
{
    SwsUOpType op = *(SwsUOpType *) p;
    buf_appendf(pbuf, prem, "%s", aarch64_op_type(op));
}

static int cmp_uop(void *pa, void *pb)
{
    int64_t ia = (int64_t) *((SwsUOpType *) pa);
    int64_t ib = (int64_t) *((SwsUOpType *) pb);
    int64_t diff = ia - ib;
    if (diff)
        return diff < 0 ? -1 : 1;
    return 0;
}

static void print_pixel_name(char **pbuf, size_t *prem, void *p)
{
    SwsPixelType type = *(SwsPixelType *) p;
    buf_appendf(pbuf, prem, "_%s", aarch64_pixel_type_name(type));
}

static void print_pixel_val(char **pbuf, size_t *prem, void *p)
{
    SwsPixelType type = *(SwsPixelType *) p;
    buf_appendf(pbuf, prem, "%s", aarch64_pixel_type(type));
}

static int cmp_pixel(void *pa, void *pb)
{
    int64_t ia = (int64_t) *((SwsPixelType *) pa);
    int64_t ib = (int64_t) *((SwsPixelType *) pb);
    int64_t diff = ia - ib;
    if (diff)
        return diff < 0 ? -1 : 1;
    return 0;
}

static uint16_t clear_to_mask(SwsClearUOp *clear)
{
    uint16_t mask = 0;
    for (int i = 0; i < 4; i++) {
        if (SWS_COMP_TEST(clear->zero, i)) {
            /* no-op */
        } else if (SWS_COMP_TEST(clear->one, i)) {
            MASK_SET(mask, i, 1);
        } else {
            MASK_SET(mask, i, 0xf);
        }
    }
    return mask;
}

static void print_clear_name(char **pbuf, size_t *prem, void *p)
{
    SwsClearUOp *clear = (SwsClearUOp *) p;
    uint16_t mask = clear_to_mask(clear);
    buf_appendf(pbuf, prem, "_%04x", mask);
}

static void print_clear_val(char **pbuf, size_t *prem, void *p)
{
    SwsClearUOp *clear = (SwsClearUOp *) p;
    buf_appendf(pbuf, prem, "{ .one = 0x%x, .zero = 0x%x }", clear->one, clear->zero);
}

static int cmp_clear(void *pa, void *pb)
{
    int64_t ia = (int64_t) clear_to_mask((SwsClearUOp *) pa);
    int64_t ib = (int64_t) clear_to_mask((SwsClearUOp *) pb);
    int64_t diff = ia - ib;
    if (diff)
        return diff < 0 ? -1 : 1;
    return 0;
}

static void print_u8_name(char **pbuf, size_t *prem, void *p)
{
    uint8_t val = *(uint8_t *) p;
    buf_appendf(pbuf, prem, "_%u", val);
}

static void print_u8_val(char **pbuf, size_t *prem, void *p)
{
    uint8_t val = *(uint8_t *) p;
    buf_appendf(pbuf, prem, "%u", val);
}

static int cmp_u8(void *pa, void *pb)
{
    int64_t ia = (int64_t) *((uint8_t *) pa);
    int64_t ib = (int64_t) *((uint8_t *) pb);
    int64_t diff = ia - ib;
    if (diff)
        return diff < 0 ? -1 : 1;
    return 0;
}

static void print_u16_name(char **pbuf, size_t *prem, void *p)
{
    uint16_t val = *(uint16_t *) p;
    buf_appendf(pbuf, prem, "_%04x", val);
}

static void print_u16_val(char **pbuf, size_t *prem, void *p)
{
    uint16_t val = *(uint16_t *) p;
    buf_appendf(pbuf, prem, "0x%04x", val);
}

static int cmp_u16(void *pa, void *pb)
{
    int64_t ia = (int64_t) *((uint16_t *) pa);
    int64_t ib = (int64_t) *((uint16_t *) pb);
    int64_t diff = ia - ib;
    if (diff)
        return diff < 0 ? -1 : 1;
    return 0;
}

static uint64_t move_to_mask(SwsMoveUOp *move)
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

static void print_move_name(char **pbuf, size_t *prem, void *p)
{
    SwsMoveUOp *move = (SwsMoveUOp *) p;
    uint64_t mask = move_to_mask(move);
    buf_appendf(pbuf, prem, "_%012" PRIx64, mask);
}

static void print_move_val(char **pbuf, size_t *prem, void *p)
{
    SwsMoveUOp *move = (SwsMoveUOp *) p;
    buf_appendf(pbuf, prem,
                "{ .num_moves = %d", move->num_moves);
    buf_appendf(pbuf, prem,
                ", .dst = {%d, %d, %d, %d, %d, %d}",
                move->dst[0], move->dst[1], move->dst[2],
                move->dst[3], move->dst[4], move->dst[5]);
    buf_appendf(pbuf, prem,
                ", .src = {%d, %d, %d, %d, %d, %d} }",
                move->src[0], move->src[1], move->src[2],
                move->src[3], move->src[4], move->src[5]);
}

static int cmp_move(void *pa, void *pb)
{
    int64_t ia = (int64_t) move_to_mask((SwsMoveUOp *) pa);
    int64_t ib = (int64_t) move_to_mask((SwsMoveUOp *) pb);
    int64_t diff = ia - ib;
    if (diff)
        return diff < 0 ? -1 : 1;
    return 0;
}

static uint16_t pack_to_mask(SwsPackUOp *pack)
{
    uint16_t mask = 0;
    for (int i = 0; i < 4; i++)
        MASK_SET(mask, i, pack->pattern[i]);
    return mask;
}

static void print_pack_name(char **pbuf, size_t *prem, void *p)
{
    SwsPackUOp *pack = (SwsPackUOp *) p;
    uint16_t mask = pack_to_mask(pack);
    buf_appendf(pbuf, prem, "_%04x", mask);
}

static void print_pack_val(char **pbuf, size_t *prem, void *p)
{
    SwsPackUOp *pack = (SwsPackUOp *) p;
    buf_appendf(pbuf, prem, "{ .pattern = {%d, %d, %d, %d} }",
                pack->pattern[0], pack->pattern[1],
                pack->pattern[2], pack->pattern[3]);
}

static int cmp_pack(void *pa, void *pb)
{
    int64_t ia = (int64_t) pack_to_mask((SwsPackUOp *) pa);
    int64_t ib = (int64_t) pack_to_mask((SwsPackUOp *) pb);
    int64_t diff = ia - ib;
    if (diff)
        return diff < 0 ? -1 : 1;
    return 0;
}

static void print_u40_name(char **pbuf, size_t *prem, void *p)
{
    uint64_t val = *(uint64_t *) p;
    buf_appendf(pbuf, prem, "_%010" PRIx64, val);
}

static void print_u40_val(char **pbuf, size_t *prem, void *p)
{
    uint64_t val = *(uint64_t *) p;
    buf_appendf(pbuf, prem, "0x%010" PRIx64 "ULL", val);
}

static int cmp_u40(void *pa, void *pb)
{
    int64_t ia = (int64_t) *((uint64_t *) pa);
    int64_t ib = (int64_t) *((uint64_t *) pb);
    int64_t diff = ia - ib;
    if (diff)
        return diff < 0 ? -1 : 1;
    return 0;
}

/*********************************************************************/
static const ParamField field_uop              = { PARAM_FIELD(uop),              print_uop_name,   print_uop_val,   cmp_uop };
static const ParamField field_mask             = { PARAM_FIELD(mask),             print_u16_name,   print_u16_val,   cmp_u16 };
static const ParamField field_type             = { PARAM_FIELD(type),             print_pixel_name, print_pixel_val, cmp_pixel };
static const ParamField field_block_size       = { PARAM_FIELD(block_size),       print_u8_name,    print_u8_val,    cmp_u8 };
static const ParamField field_shift            = { PARAM_FIELD(shift.amount),     print_u8_name,    print_u8_val,    cmp_u8 };
static const ParamField field_clear            = { PARAM_FIELD(clear),            print_clear_name, print_clear_val, cmp_clear };
static const ParamField field_move             = { PARAM_FIELD(move),             print_move_name,  print_move_val,  cmp_move };
static const ParamField field_pack             = { PARAM_FIELD(pack),             print_pack_name,  print_pack_val,  cmp_pack };
static const ParamField field_linear_mask      = { PARAM_FIELD(linear.mask),      print_u40_name,   print_u40_val,   cmp_u40 };
static const ParamField field_linear_fmla      = { PARAM_FIELD(linear.fmla),      print_u8_name,    print_u8_val,    cmp_u8 };
static const ParamField field_dither_y_offset  = { PARAM_FIELD(dither.y_offset),  print_u16_name,   print_u16_val,   cmp_u16 };
static const ParamField field_dither_size_log2 = { PARAM_FIELD(dither.size_log2), print_u8_name,    print_u8_val,    cmp_u8 };

/* Fields needed to uniquely identify each SwsUOpType. */
#define MAX_LEVELS 8
static const ParamField *op_fields[SWS_UOP_TYPE_NB][MAX_LEVELS] = {
    [SWS_UOP_READ_BIT      ] = { &field_uop,                                                  &field_block_size, &field_type, &field_mask },
    [SWS_UOP_READ_NIBBLE   ] = { &field_uop,                                                  &field_block_size, &field_type, &field_mask },
    [SWS_UOP_READ_PACKED   ] = { &field_uop,                                                  &field_block_size, &field_type, &field_mask },
    [SWS_UOP_READ_PLANAR   ] = { &field_uop,                                                  &field_block_size, &field_type, &field_mask },
    [SWS_UOP_WRITE_BIT     ] = { &field_uop,                                                  &field_block_size, &field_type, &field_mask },
    [SWS_UOP_WRITE_NIBBLE  ] = { &field_uop,                                                  &field_block_size, &field_type, &field_mask },
    [SWS_UOP_WRITE_PACKED  ] = { &field_uop,                                                  &field_block_size, &field_type, &field_mask },
    [SWS_UOP_WRITE_PLANAR  ] = { &field_uop,                                                  &field_block_size, &field_type, &field_mask },
    [SWS_UOP_SWAP_BYTES    ] = { &field_uop,                                                  &field_block_size, &field_type, &field_mask },
    [SWS_UOP_MOVE          ] = { &field_uop, &field_move,                                     &field_block_size, &field_type, &field_mask },
    [SWS_UOP_UNPACK        ] = { &field_uop, &field_pack,                                     &field_block_size, &field_type, &field_mask },
    [SWS_UOP_PACK          ] = { &field_uop, &field_pack,                                     &field_block_size, &field_type, &field_mask },
    [SWS_UOP_LSHIFT        ] = { &field_uop, &field_shift,                                    &field_block_size, &field_type, &field_mask },
    [SWS_UOP_RSHIFT        ] = { &field_uop, &field_shift,                                    &field_block_size, &field_type, &field_mask },
    [SWS_UOP_CLEAR         ] = { &field_uop, &field_clear,                                    &field_block_size, &field_type, &field_mask },
    [SWS_UOP_TO_U8         ] = { &field_uop,                                                  &field_block_size, &field_type, &field_mask },
    [SWS_UOP_TO_U16        ] = { &field_uop,                                                  &field_block_size, &field_type, &field_mask },
    [SWS_UOP_TO_U32        ] = { &field_uop,                                                  &field_block_size, &field_type, &field_mask },
    [SWS_UOP_TO_F32        ] = { &field_uop,                                                  &field_block_size, &field_type, &field_mask },
    [SWS_UOP_EXPAND_PAIR   ] = { &field_uop,                                                  &field_block_size, &field_type, &field_mask },
    [SWS_UOP_EXPAND_QUAD   ] = { &field_uop,                                                  &field_block_size, &field_type, &field_mask },
    [SWS_UOP_MIN           ] = { &field_uop,                                                  &field_block_size, &field_type, &field_mask },
    [SWS_UOP_MAX           ] = { &field_uop,                                                  &field_block_size, &field_type, &field_mask },
    [SWS_UOP_SCALE         ] = { &field_uop,                                                  &field_block_size, &field_type, &field_mask },
    [SWS_UOP_LINEAR        ] = { &field_uop, &field_linear_mask,     &field_linear_fmla,      &field_block_size, &field_type, &field_mask },
    [SWS_UOP_LINEAR_FMA    ] = { &field_uop, &field_linear_mask,     &field_linear_fmla,      &field_block_size, &field_type, &field_mask },
    [SWS_UOP_DITHER        ] = { &field_uop, &field_dither_y_offset, &field_dither_size_log2, &field_block_size, &field_type, &field_mask },
};
