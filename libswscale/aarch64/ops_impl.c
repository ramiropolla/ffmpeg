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
const char *sws_aarch64_pixel_type(SwsAArch64PixelType fmt)
{
    switch (fmt) {
    case AARCH64_PIXEL_U8:  return "AARCH64_PIXEL_U8";
    case AARCH64_PIXEL_U16: return "AARCH64_PIXEL_U16";
    case AARCH64_PIXEL_U32: return "AARCH64_PIXEL_U32";
    case AARCH64_PIXEL_F32: return "AARCH64_PIXEL_F32";
    }
    assert(!"Invalid pixel type!");
    return NULL;
}

const char *sws_aarch64_pixel_type_name(SwsAArch64PixelType fmt)
{
    switch (fmt) {
    case AARCH64_PIXEL_U8:  return "u8";
    case AARCH64_PIXEL_U16: return "u16";
    case AARCH64_PIXEL_U32: return "u32";
    case AARCH64_PIXEL_F32: return "f32";
    }
    assert(!"Invalid pixel type!");
    return NULL;
}

size_t sws_aarch64_pixel_size(SwsAArch64PixelType fmt)
{
    switch (fmt) {
    case AARCH64_PIXEL_U8:  return 1;
    case AARCH64_PIXEL_U16: return 2;
    case AARCH64_PIXEL_U32: return 4;
    case AARCH64_PIXEL_F32: return 4;
    }
    assert(!"Invalid pixel type!");
    return 0;
}

/*********************************************************************/
const char *sws_aarch64_op_type(SwsAArch64OpType op)
{
    switch (op) {
    case AARCH64_SWS_OP_NONE:           return "AARCH64_SWS_OP_NONE";
    case AARCH64_SWS_OP_PROCESS:        return "AARCH64_SWS_OP_PROCESS";
    case AARCH64_SWS_OP_PROCESS_RETURN: return "AARCH64_SWS_OP_PROCESS_RETURN";
    case AARCH64_SWS_OP_READ_BIT:       return "AARCH64_SWS_OP_READ_BIT";
    case AARCH64_SWS_OP_READ_NIBBLE:    return "AARCH64_SWS_OP_READ_NIBBLE";
    case AARCH64_SWS_OP_READ_PACKED:    return "AARCH64_SWS_OP_READ_PACKED";
    case AARCH64_SWS_OP_READ_PLANAR:    return "AARCH64_SWS_OP_READ_PLANAR";
    case AARCH64_SWS_OP_WRITE_BIT:      return "AARCH64_SWS_OP_WRITE_BIT";
    case AARCH64_SWS_OP_WRITE_NIBBLE:   return "AARCH64_SWS_OP_WRITE_NIBBLE";
    case AARCH64_SWS_OP_WRITE_PACKED:   return "AARCH64_SWS_OP_WRITE_PACKED";
    case AARCH64_SWS_OP_WRITE_PLANAR:   return "AARCH64_SWS_OP_WRITE_PLANAR";
    case AARCH64_SWS_OP_SWAP_BYTES:     return "AARCH64_SWS_OP_SWAP_BYTES";
    case AARCH64_SWS_OP_SWIZZLE:        return "AARCH64_SWS_OP_SWIZZLE";
    case AARCH64_SWS_OP_UNPACK:         return "AARCH64_SWS_OP_UNPACK";
    case AARCH64_SWS_OP_PACK:           return "AARCH64_SWS_OP_PACK";
    case AARCH64_SWS_OP_LSHIFT:         return "AARCH64_SWS_OP_LSHIFT";
    case AARCH64_SWS_OP_RSHIFT:         return "AARCH64_SWS_OP_RSHIFT";
    case AARCH64_SWS_OP_CLEAR:          return "AARCH64_SWS_OP_CLEAR";
    case AARCH64_SWS_OP_CONVERT:        return "AARCH64_SWS_OP_CONVERT";
    case AARCH64_SWS_OP_EXPAND:         return "AARCH64_SWS_OP_EXPAND";
    case AARCH64_SWS_OP_MIN:            return "AARCH64_SWS_OP_MIN";
    case AARCH64_SWS_OP_MAX:            return "AARCH64_SWS_OP_MAX";
    case AARCH64_SWS_OP_SCALE:          return "AARCH64_SWS_OP_SCALE";
    case AARCH64_SWS_OP_LINEAR:         return "AARCH64_SWS_OP_LINEAR";
    case AARCH64_SWS_OP_DITHER:         return "AARCH64_SWS_OP_DITHER";
    }
    assert(!"Invalid op type!");
    return NULL;
}

const char *sws_aarch64_op_type_name(SwsAArch64OpType op)
{
    switch (op) {
    case AARCH64_SWS_OP_NONE:           return "none";
    case AARCH64_SWS_OP_PROCESS:        return "process";
    case AARCH64_SWS_OP_PROCESS_RETURN: return "process_return";
    case AARCH64_SWS_OP_READ_BIT:       return "read_bit";
    case AARCH64_SWS_OP_READ_NIBBLE:    return "read_nibble";
    case AARCH64_SWS_OP_READ_PACKED:    return "read_packed";
    case AARCH64_SWS_OP_READ_PLANAR:    return "read_planar";
    case AARCH64_SWS_OP_WRITE_BIT:      return "write_bit";
    case AARCH64_SWS_OP_WRITE_NIBBLE:   return "write_nibble";
    case AARCH64_SWS_OP_WRITE_PACKED:   return "write_packed";
    case AARCH64_SWS_OP_WRITE_PLANAR:   return "write_planar";
    case AARCH64_SWS_OP_SWAP_BYTES:     return "swap_bytes";
    case AARCH64_SWS_OP_SWIZZLE:        return "swizzle";
    case AARCH64_SWS_OP_UNPACK:         return "unpack";
    case AARCH64_SWS_OP_PACK:           return "pack";
    case AARCH64_SWS_OP_LSHIFT:         return "lshift";
    case AARCH64_SWS_OP_RSHIFT:         return "rshift";
    case AARCH64_SWS_OP_CLEAR:          return "clear";
    case AARCH64_SWS_OP_CONVERT:        return "convert";
    case AARCH64_SWS_OP_EXPAND:         return "expand";
    case AARCH64_SWS_OP_MIN:            return "min";
    case AARCH64_SWS_OP_MAX:            return "max";
    case AARCH64_SWS_OP_SCALE:          return "scale";
    case AARCH64_SWS_OP_LINEAR:         return "linear";
    case AARCH64_SWS_OP_DITHER:         return "dither";
    }
    assert(!"Invalid op type!");
    return NULL;
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
        buf_appendf(&buf, &size, "_%04x_neon", nswap16(params->mask));
        return;
    case AARCH64_SWS_OP_SWIZZLE:
        buf_appendf(&buf, &size, "_%04x", nswap16(params->swizzle));
        break;
    case AARCH64_SWS_OP_UNPACK:
    case AARCH64_SWS_OP_PACK:
        buf_appendf(&buf, &size, "_%04x", nswap16(params->pack));
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
    default:
        break;
    }

    buf_appendf(&buf, &size, "_%04x_%u_%s_neon",
                nswap16(params->mask),
                params->block_size,
                sws_aarch64_pixel_type_name(params->type));
}

void sws_aarch64_op_impl_serialize(char *buf, size_t size, const SwsAArch64OpImplParams *params)
{
    buf_appendf(&buf, &size, "{ .op = %s, .mask = 0x%04x",
                sws_aarch64_op_type(params->op),
                params->mask);

    switch (params->op) {
    case AARCH64_SWS_OP_PROCESS:
    case AARCH64_SWS_OP_PROCESS_RETURN:
        buf_appendf(&buf, &size, " }");
        return;
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
    default:
        break;
    }
    buf_appendf(&buf, &size, " }");
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
    default:
        break;
    }
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
    default:
        break;
    }
    return 0;
}
