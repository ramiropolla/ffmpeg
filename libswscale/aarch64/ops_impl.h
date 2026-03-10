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

#ifndef SWSCALE_AARCH64_OPS_IMPL_H
#define SWSCALE_AARCH64_OPS_IMPL_H

#include <stddef.h>
#include <stdint.h>

/* Similar to SwsPixelType */
typedef enum SwsAArch64PixelType {
    AARCH64_PIXEL_U8,
    AARCH64_PIXEL_U16,
    AARCH64_PIXEL_U32,
    AARCH64_PIXEL_F32,
    AARCH64_PIXEL_TYPE_NB,
} SwsAArch64PixelType;

const char *sws_aarch64_pixel_type(SwsAArch64PixelType fmt);
const char *sws_aarch64_pixel_type_name(SwsAArch64PixelType fmt);
size_t sws_aarch64_pixel_size(SwsAArch64PixelType fmt);

/* Similar to SwsOpType */
typedef enum SwsAArch64OpType {
    AARCH64_SWS_OP_NONE = 0,
    AARCH64_SWS_OP_PROCESS,
    AARCH64_SWS_OP_PROCESS_RETURN,
    AARCH64_SWS_OP_READ_BIT,
    AARCH64_SWS_OP_READ_NIBBLE,
    AARCH64_SWS_OP_READ_PACKED,
    AARCH64_SWS_OP_READ_PLANAR,
    AARCH64_SWS_OP_WRITE_BIT,
    AARCH64_SWS_OP_WRITE_NIBBLE,
    AARCH64_SWS_OP_WRITE_PACKED,
    AARCH64_SWS_OP_WRITE_PLANAR,
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
    AARCH64_SWS_OP_TYPE_NB,
} SwsAArch64OpType;

const char *sws_aarch64_op_type(SwsAArch64OpType op);
const char *sws_aarch64_op_type_name(SwsAArch64OpType op);

/* Each nibble in the mask corresponds to one component. */
typedef uint16_t SwsAArch64OpMask;

/**
 * Affine coefficient mask for linear op. Packs a 4x5 matrix in execution
 * order, where the offset is the first element, with 2 bits per element:
 *   00: m[i][j] == 0
 *   01: m[i][j] == 1
 *   11: m[i][j] is any other coefficient
 */
typedef uint64_t SwsAArch64LinearOpMask;

typedef struct SwsAArch64DitherOp {
    uint16_t y_offset;
    uint8_t size_log2;
} SwsAArch64DitherOp;

/**
 * SwsAArch64OpImplParams describes the parameters for an SwsAArch64OpType
 * operation. It consists of simplified parameters from the SwsOp structure,
 * with the purpose of being straight-forward to implement and execute.
 */
typedef struct SwsAArch64OpImplParams {
    SwsAArch64OpType    op;
    SwsAArch64OpMask    mask;
    SwsAArch64PixelType type;
    uint8_t block_size;
    union {
        uint8_t                shift;
        SwsAArch64OpMask       swizzle;
        SwsAArch64OpMask       pack;
        SwsAArch64PixelType    to_type;
        SwsAArch64LinearOpMask linear;
        SwsAArch64DitherOp     dither;
    };
} SwsAArch64OpImplParams;

/**
 * Serialize SwsAArch64OpImplParams so it can be used to generate
 * ops_entries.c, which will in turn be used by asmgen to generate
 * the functions.
 */
void sws_aarch64_op_impl_serialize(char *buf, size_t size, const SwsAArch64OpImplParams *params);

/* Generate the function name for the given parameters. */
void sws_aarch64_op_impl_func_name(char *buf, size_t size, const SwsAArch64OpImplParams *params);

/**
 * Generate code fragment to uniquely detect the function described by
 * the given parameters, opening and/or closing conditions depending on
 * the parameters of the previous function.
 */
void sws_aarch64_op_impl_lookup_str(char *buf, size_t size, const SwsAArch64OpImplParams *params,
                                    const SwsAArch64OpImplParams *prev, const char *p_str);

/**
 * Compare two SwsAArch64OpImplParams structures. Establishes the
 * canonical ordering used by all SwsAArch64OpImplParams-related lists.
 */
int sws_aarch64_op_impl_cmp(const void *a, const void *b);

/* SwsAArch64OpMask-related helpers. */

#define MASK_GET(mask, idx) (((mask) >> ((idx) << 2)) & 0xf)
#define MASK_SET(mask, idx, val) do { (mask) |= (((val) & 0xf) << ((idx) << 2)); } while (0)

#define LOOP(mask, idx)                 \
    for (int idx = 0; idx < 4; idx++)   \
        if (MASK_GET(mask, idx))
#define LOOP_BWD(mask, idx)             \
    for (int idx = 3; idx >= 0; idx--)  \
        if (MASK_GET(mask, idx))

#define LOOP_MASK(p, idx) LOOP(p->mask, idx)
#define LOOP_MASK_BWD(p, idx) LOOP_BWD(p->mask, idx)

#define LINEAR_MASK_GET(mask, idx, jdx) (((mask) >> (2 * ((5 * (idx) + (jdx))))) & 3)
#define LINEAR_MASK_SET(mask, idx, jdx, val) do { (mask) |= (((val) & 3) << (2 * ((5 * (idx) + (jdx))))); } while (0)
#define LINEAR_MASK_0 0
#define LINEAR_MASK_1 1
#define LINEAR_MASK_X 3

#define LOOP_LINEAR_MASK(p, idx, jdx)       \
    LOOP_MASK(p, idx)                       \
        for (int jdx = 0; jdx < 5; jdx++)   \
            if (LINEAR_MASK_GET(p->linear, idx, jdx))

/* Compute number of vector registers needed to store all coefficients. */
static inline int linear_num_vregs(const SwsAArch64OpImplParams *params)
{
    int count = 0;
    LOOP_LINEAR_MASK(params, i, j)
        count++;
    int num_vregs = (count + 3) >> 2;
    assert(num_vregs <= 4);
    return num_vregs;
}

/**
 * The column index order for SwsLinearOp.mask follows the affine transform
 * order, where the offset is the last element. SwsAArch64LinearOpMask, on
 * the other hand, follows execution order, where the offset is the first
 * element.
 */
static inline int linear_index_from_sws_op(int idx)
{
    const int reorder_col[5] = { 1, 2, 3, 4, 0 };
    return reorder_col[idx];
}

static inline int linear_index_is_offset(int idx)
{
    return (idx == 0);
}

static inline int linear_index_to_vx(int idx)
{
    /* The offset shouldn't map to any vx, but to please UBSan we map
     * it to 0. */
    if (linear_index_is_offset(idx))
        return 0;
    return (idx - 1);
}

#endif /* SWSCALE_AARCH64_OPS_IMPL_H */
