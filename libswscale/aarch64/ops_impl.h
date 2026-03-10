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

#ifndef AARCH64_OPS_IMPL_H
#define AARCH64_OPS_IMPL_H

#include <stddef.h>
#include <stdint.h>

/*********************************************************************/
/* Similar to SwsPixelType */

typedef enum SwsAArch64PixelType {
    AARCH64_PIXEL_U8,
    AARCH64_PIXEL_U16,
    AARCH64_PIXEL_U32,
    AARCH64_PIXEL_F32,
} SwsAArch64PixelType;

const char *sws_aarch64_pixel_type(SwsAArch64PixelType fmt);
const char *sws_aarch64_pixel_type_name(SwsAArch64PixelType fmt);
size_t sws_aarch64_pixel_size(SwsAArch64PixelType fmt);

/*********************************************************************/
/* Similar to SwsOpType */

typedef enum SwsAArch64OpType {
    AARCH64_SWS_OP_NONE = 0,
    AARCH64_SWS_OP_PROCESS,
    AARCH64_SWS_OP_PROCESS_RETURN,
    AARCH64_SWS_OP_READ,
    AARCH64_SWS_OP_WRITE,
    AARCH64_SWS_OP_SWAP_BYTES,
    AARCH64_SWS_OP_SWIZZLE,
    AARCH64_SWS_OP_UNPACK,
    AARCH64_SWS_OP_PACK,
    AARCH64_SWS_OP_LSHIFT,
    AARCH64_SWS_OP_RSHIFT,
    AARCH64_SWS_OP_CLEAR,
    AARCH64_SWS_OP_CONVERT,
    AARCH64_SWS_OP_EXPAND,
    AARCH64_SWS_OP_MIN,
    AARCH64_SWS_OP_MAX,
    AARCH64_SWS_OP_SCALE,
    AARCH64_SWS_OP_LINEAR,
    AARCH64_SWS_OP_DITHER,
} SwsAArch64OpType;

const char *sws_aarch64_op_type(SwsAArch64OpType op);
const char *sws_aarch64_op_type_name(SwsAArch64OpType op);

/*********************************************************************/
/* Additional parameters for AARCH64_SWS_OP_READ/AARCH64_SWS_OP_WRITE */

typedef enum SwsAArch64RWOpType {
    AARCH64_SWS_OP_RW_BIT,
    AARCH64_SWS_OP_RW_NIBBLE,
    AARCH64_SWS_OP_RW_PACKED,
    AARCH64_SWS_OP_RW_PLANAR,
} SwsAArch64RWOpType;

const char *sws_aarch64_op_rw_type(SwsAArch64RWOpType rw);
const char *sws_aarch64_op_rw_type_name(SwsAArch64RWOpType rw);

/*********************************************************************/
typedef uint16_t SwsAArch64OpMask;
typedef uint64_t SwsAArch64LinearOpMask;

typedef struct SwsAArch64OpImplParams {
    SwsAArch64OpType    op;
    SwsAArch64OpMask    mask;
    SwsAArch64PixelType type;
    unsigned int block_size;
    union {
        SwsAArch64RWOpType     rw;
        unsigned int           shift;
        SwsAArch64OpMask       swizzle;
        SwsAArch64OpMask       pack;
        SwsAArch64PixelType    to_type;
        SwsAArch64LinearOpMask linear;
    };
} SwsAArch64OpImplParams;

void sws_aarch64_op_impl_serialize(char *buf, size_t size, const SwsAArch64OpImplParams *params);
void sws_aarch64_op_impl_func_name(char *buf, size_t size, const SwsAArch64OpImplParams *params);
void sws_aarch64_op_impl_cond_str(char *buf, size_t size, const SwsAArch64OpImplParams *params, const char *p_str);
int sws_aarch64_op_impl_cmp(const void *a, const void *b);

#if 0
#define LOOP_MASK(idx, mask)            \
    for (int idx = 0; idx < 4; idx++)   \
        if ((mask) & (1 << (idx << 2)))
#endif

static uint16_t nswap16(uint16_t v)
{
    return ((v & 0x000f) << 12) |
           ((v & 0x00f0) <<  4) |
           ((v & 0x0f00) >>  4) |
           ((v & 0xf000) >> 12);
}

#endif /* AARCH64_OPS_IMPL_H */
