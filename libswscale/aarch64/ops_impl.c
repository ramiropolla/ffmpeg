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

// NOTE this file should not depend on ffmpeg. It will be built with hostcc and run at build time.

#include <assert.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>

#include "ops_impl.h"

/*********************************************************************/
static const char pixel_types[AARCH64_SWS_OP_TYPE_NB][32] = {
    [AARCH64_PIXEL_U8 ] = "AARCH64_PIXEL_U8",
    [AARCH64_PIXEL_U16] = "AARCH64_PIXEL_U16",
    [AARCH64_PIXEL_U32] = "AARCH64_PIXEL_U32",
    [AARCH64_PIXEL_F32] = "AARCH64_PIXEL_F32",
};

const char *sws_aarch64_pixel_type(SwsAArch64PixelType fmt)
{
    if (fmt >= AARCH64_PIXEL_TYPE_NB) {
        assert(!"Invalid pixel type!");
        return NULL;
    }
    return pixel_types[fmt];
}

static const char pixel_type_names[AARCH64_SWS_OP_TYPE_NB][4] = {
    [AARCH64_PIXEL_U8 ] = "u8",
    [AARCH64_PIXEL_U16] = "u16",
    [AARCH64_PIXEL_U32] = "u32",
    [AARCH64_PIXEL_F32] = "f32",
};

const char *sws_aarch64_pixel_type_name(SwsAArch64PixelType fmt)
{
    if (fmt >= AARCH64_PIXEL_TYPE_NB) {
        assert(!"Invalid pixel type!");
        return NULL;
    }
    return pixel_type_names[fmt];
}

size_t sws_aarch64_pixel_size(SwsAArch64PixelType fmt)
{
    switch (fmt) {
    case AARCH64_PIXEL_U8:  return 1;
    case AARCH64_PIXEL_U16: return 2;
    case AARCH64_PIXEL_U32: return 4;
    case AARCH64_PIXEL_F32: return 4;
    default:
        assert(!"Invalid pixel type!");
        break;
    }
    return 0;
}

/*********************************************************************/
static const char op_types[AARCH64_SWS_OP_TYPE_NB][32] = {
    [AARCH64_SWS_OP_NONE          ] = "AARCH64_SWS_OP_NONE",
    [AARCH64_SWS_OP_PROCESS       ] = "AARCH64_SWS_OP_PROCESS",
    [AARCH64_SWS_OP_PROCESS_RETURN] = "AARCH64_SWS_OP_PROCESS_RETURN",
    [AARCH64_SWS_OP_READ_BIT      ] = "AARCH64_SWS_OP_READ_BIT",
    [AARCH64_SWS_OP_READ_NIBBLE   ] = "AARCH64_SWS_OP_READ_NIBBLE",
    [AARCH64_SWS_OP_READ_PACKED   ] = "AARCH64_SWS_OP_READ_PACKED",
    [AARCH64_SWS_OP_READ_PLANAR   ] = "AARCH64_SWS_OP_READ_PLANAR",
    [AARCH64_SWS_OP_WRITE_BIT     ] = "AARCH64_SWS_OP_WRITE_BIT",
    [AARCH64_SWS_OP_WRITE_NIBBLE  ] = "AARCH64_SWS_OP_WRITE_NIBBLE",
    [AARCH64_SWS_OP_WRITE_PACKED  ] = "AARCH64_SWS_OP_WRITE_PACKED",
    [AARCH64_SWS_OP_WRITE_PLANAR  ] = "AARCH64_SWS_OP_WRITE_PLANAR",
    [AARCH64_SWS_OP_SWAP_BYTES    ] = "AARCH64_SWS_OP_SWAP_BYTES",
    [AARCH64_SWS_OP_SWIZZLE       ] = "AARCH64_SWS_OP_SWIZZLE",
    [AARCH64_SWS_OP_UNPACK        ] = "AARCH64_SWS_OP_UNPACK",
    [AARCH64_SWS_OP_PACK          ] = "AARCH64_SWS_OP_PACK",
    [AARCH64_SWS_OP_LSHIFT        ] = "AARCH64_SWS_OP_LSHIFT",
    [AARCH64_SWS_OP_RSHIFT        ] = "AARCH64_SWS_OP_RSHIFT",
    [AARCH64_SWS_OP_CLEAR         ] = "AARCH64_SWS_OP_CLEAR",
    [AARCH64_SWS_OP_CONVERT       ] = "AARCH64_SWS_OP_CONVERT",
    [AARCH64_SWS_OP_EXPAND        ] = "AARCH64_SWS_OP_EXPAND",
    [AARCH64_SWS_OP_MIN           ] = "AARCH64_SWS_OP_MIN",
    [AARCH64_SWS_OP_MAX           ] = "AARCH64_SWS_OP_MAX",
    [AARCH64_SWS_OP_SCALE         ] = "AARCH64_SWS_OP_SCALE",
    [AARCH64_SWS_OP_LINEAR        ] = "AARCH64_SWS_OP_LINEAR",
    [AARCH64_SWS_OP_DITHER        ] = "AARCH64_SWS_OP_DITHER",
};

const char *sws_aarch64_op_type(SwsAArch64OpType op)
{
    if (op == AARCH64_SWS_OP_NONE || op >= AARCH64_SWS_OP_TYPE_NB) {
        assert(!"Invalid op type!");
        return NULL;
    }
    return op_types[op];
}

static const char op_type_names[AARCH64_SWS_OP_TYPE_NB][16] = {
    [AARCH64_SWS_OP_NONE          ] = "none",
    [AARCH64_SWS_OP_PROCESS       ] = "process",
    [AARCH64_SWS_OP_PROCESS_RETURN] = "process_return",
    [AARCH64_SWS_OP_READ_BIT      ] = "read_bit",
    [AARCH64_SWS_OP_READ_NIBBLE   ] = "read_nibble",
    [AARCH64_SWS_OP_READ_PACKED   ] = "read_packed",
    [AARCH64_SWS_OP_READ_PLANAR   ] = "read_planar",
    [AARCH64_SWS_OP_WRITE_BIT     ] = "write_bit",
    [AARCH64_SWS_OP_WRITE_NIBBLE  ] = "write_nibble",
    [AARCH64_SWS_OP_WRITE_PACKED  ] = "write_packed",
    [AARCH64_SWS_OP_WRITE_PLANAR  ] = "write_planar",
    [AARCH64_SWS_OP_SWAP_BYTES    ] = "swap_bytes",
    [AARCH64_SWS_OP_SWIZZLE       ] = "swizzle",
    [AARCH64_SWS_OP_UNPACK        ] = "unpack",
    [AARCH64_SWS_OP_PACK          ] = "pack",
    [AARCH64_SWS_OP_LSHIFT        ] = "lshift",
    [AARCH64_SWS_OP_RSHIFT        ] = "rshift",
    [AARCH64_SWS_OP_CLEAR         ] = "clear",
    [AARCH64_SWS_OP_CONVERT       ] = "convert",
    [AARCH64_SWS_OP_EXPAND        ] = "expand",
    [AARCH64_SWS_OP_MIN           ] = "min",
    [AARCH64_SWS_OP_MAX           ] = "max",
    [AARCH64_SWS_OP_SCALE         ] = "scale",
    [AARCH64_SWS_OP_LINEAR        ] = "linear",
    [AARCH64_SWS_OP_DITHER        ] = "dither",
};

const char *sws_aarch64_op_type_name(SwsAArch64OpType op)
{
    if (op == AARCH64_SWS_OP_NONE || op >= AARCH64_SWS_OP_TYPE_NB) {
        assert(!"Invalid op type!");
        return NULL;
    }
    return op_type_names[op];
}

/*********************************************************************/
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

void sws_aarch64_op_impl_func_name(char *buf, size_t size, const SwsAArch64OpImplParams *params)
{
    buf_appendf(&buf, &size, "ff_sws_%s", sws_aarch64_op_type_name(params->op));

    switch (params->op) {
    case AARCH64_SWS_OP_PROCESS:
    case AARCH64_SWS_OP_PROCESS_RETURN:
        buf_appendf(&buf, &size, "_%04x_neon", params->mask);
        return;
    case AARCH64_SWS_OP_SWIZZLE:
        buf_appendf(&buf, &size, "_%04x", params->swizzle);
        break;
    case AARCH64_SWS_OP_UNPACK:
    case AARCH64_SWS_OP_PACK:
        buf_appendf(&buf, &size, "_%04x", params->pack);
        break;
    case AARCH64_SWS_OP_LSHIFT:
    case AARCH64_SWS_OP_RSHIFT:
        buf_appendf(&buf, &size, "_%02x", params->shift);
        break;
    case AARCH64_SWS_OP_EXPAND:
    case AARCH64_SWS_OP_CONVERT:
        buf_appendf(&buf, &size, "_%s", sws_aarch64_pixel_type_name(params->to_type));
        break;
        break;
    case AARCH64_SWS_OP_LINEAR:
        buf_appendf(&buf, &size, "_%010"PRIx64"", params->linear);
        break;
    case AARCH64_SWS_OP_DITHER:
        buf_appendf(&buf, &size, "_%04x_%u", params->dither.y_offset, params->dither.size_log2);
        break;
    default:
        break;
    }

    buf_appendf(&buf, &size, "_%04x_%u_%s_neon",
                params->mask,
                params->block_size,
                sws_aarch64_pixel_type_name(params->type));
    assert(size);
}

void sws_aarch64_op_impl_serialize(char *buf, size_t size, const SwsAArch64OpImplParams *params)
{
    buf_appendf(&buf, &size, "{ .op = %s, .mask = 0x%04x",
                sws_aarch64_op_type(params->op),
                params->mask);

    switch (params->op) {
    case AARCH64_SWS_OP_PROCESS:
    case AARCH64_SWS_OP_PROCESS_RETURN:
        goto end;
    default:
        break;
    }

    buf_appendf(&buf, &size, ", .block_size = %u, .type = %s",
                params->block_size,
                sws_aarch64_pixel_type(params->type));

    switch (params->op) {
    case AARCH64_SWS_OP_SWIZZLE:
        buf_appendf(&buf, &size, ", .swizzle = 0x%04x", params->swizzle);
        break;
    case AARCH64_SWS_OP_UNPACK:
    case AARCH64_SWS_OP_PACK:
        buf_appendf(&buf, &size, ", .pack = 0x%04x", params->pack);
        break;
    case AARCH64_SWS_OP_LSHIFT:
    case AARCH64_SWS_OP_RSHIFT:
        buf_appendf(&buf, &size, ", .shift = %u", params->shift);
        break;
    case AARCH64_SWS_OP_EXPAND:
    case AARCH64_SWS_OP_CONVERT:
        buf_appendf(&buf, &size, ", .to_type = %s", sws_aarch64_pixel_type(params->to_type));
        break;
    case AARCH64_SWS_OP_LINEAR:
        buf_appendf(&buf, &size, ", .linear = 0x%010"PRIx64"ULL", params->linear);
        break;
    case AARCH64_SWS_OP_DITHER:
        buf_appendf(&buf, &size, ", .dither = { .y_offset = 0x%04x, .size_log2 = %u }", params->dither.y_offset, params->dither.size_log2);
        break;
    default:
        break;
    }

end:
    buf_appendf(&buf, &size, " }");
    assert(size);
}

void sws_aarch64_op_impl_cond_str(char *buf, size_t size, const SwsAArch64OpImplParams *params, const char *p_str)
{
    buf_appendf(&buf, &size, "%sop == %s && %smask == 0x%04x && %sblock_size == %u && %stype == %s",
                p_str, sws_aarch64_op_type(params->op),
                p_str, params->mask,
                p_str, params->block_size,
                p_str, sws_aarch64_pixel_type(params->type));

    switch (params->op) {
    case AARCH64_SWS_OP_SWIZZLE:
        buf_appendf(&buf, &size, " && %sswizzle == 0x%04x", p_str, params->swizzle);
        break;
    case AARCH64_SWS_OP_UNPACK:
    case AARCH64_SWS_OP_PACK:
        buf_appendf(&buf, &size, " && %spack == 0x%04x", p_str, params->pack);
        break;
    case AARCH64_SWS_OP_LSHIFT:
    case AARCH64_SWS_OP_RSHIFT:
        buf_appendf(&buf, &size, " && %sshift == %u", p_str, params->shift);
        break;
    case AARCH64_SWS_OP_EXPAND:
    case AARCH64_SWS_OP_CONVERT:
        buf_appendf(&buf, &size, " && %sto_type == %s", p_str, sws_aarch64_pixel_type(params->to_type));
        break;
    case AARCH64_SWS_OP_LINEAR:
        buf_appendf(&buf, &size, " && %slinear == 0x%010"PRIx64"ULL", p_str, params->linear);
        break;
    case AARCH64_SWS_OP_DITHER:
        buf_appendf(&buf, &size, " && %sdither.y_offset == 0x%04x && %sdither.size_log2 == %u", p_str, params->dither.y_offset, p_str, params->dither.size_log2);
        break;
    default:
        break;
    }
    assert(size);
}

#define COMPARE_VAL(a, b, name)         \
    do {                                \
        int64_t ia = (int64_t) a->name; \
        int64_t ib = (int64_t) b->name; \
        int64_t diff = ia - ib;         \
        if (diff)                       \
            return diff < 0 ? -1 : 1;   \
    } while (0)

int sws_aarch64_op_impl_cmp(const void *a, const void *b)
{
    const SwsAArch64OpImplParams *pa = (const SwsAArch64OpImplParams *) a;
    const SwsAArch64OpImplParams *pb = (const SwsAArch64OpImplParams *) b;
    COMPARE_VAL(pa, pb, op);
    COMPARE_VAL(pa, pb, mask);
    COMPARE_VAL(pa, pb, type);
    COMPARE_VAL(pa, pb, block_size);
    switch (pa->op) {
    case AARCH64_SWS_OP_SWIZZLE:
        COMPARE_VAL(pa, pb, swizzle);
        break;
    case AARCH64_SWS_OP_UNPACK:
    case AARCH64_SWS_OP_PACK:
        COMPARE_VAL(pa, pb, pack);
        break;
    case AARCH64_SWS_OP_LSHIFT:
    case AARCH64_SWS_OP_RSHIFT:
        COMPARE_VAL(pa, pb, shift);
        break;
    case AARCH64_SWS_OP_EXPAND:
    case AARCH64_SWS_OP_CONVERT:
        COMPARE_VAL(pa, pb, to_type);
        break;
    case AARCH64_SWS_OP_LINEAR:
        COMPARE_VAL(pa, pb, linear);
        break;
    case AARCH64_SWS_OP_DITHER:
        COMPARE_VAL(pa, pb, dither.y_offset);
        COMPARE_VAL(pa, pb, dither.size_log2);
        break;
    default:
        break;
    }
    return 0;
}
