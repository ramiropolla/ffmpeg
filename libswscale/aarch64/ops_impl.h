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

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "libswscale/uops.h"

/* Each nibble in the mask corresponds to one component. */
#define NIBBLE_GET(mask, idx) (((mask) >> ((idx) << 2)) & 0xf)
#define NIBBLE_SET(mask, idx, val) do { (mask) |= (((val) & 0xf) << ((idx) << 2)); } while (0)

static inline uint16_t nibble_mask(SwsCompMask mask)
{
    uint16_t ret = 0;
    for (int i = 0; i < 4; i++)
        NIBBLE_SET(ret, i, !!(mask & SWS_COMP(i)));
    return ret;
}

/**
 * SwsAArch64OpImplParams describes the parameters for an SwsUOpType
 * operation. It consists of simplified parameters from the SwsUOp structure,
 * with the purpose of being straight-forward to implement and execute.
 */
typedef struct SwsAArch64OpImplParams {
    SwsUOpType          uop;
    SwsCompMask         mask;
    SwsPixelType        type;
    uint8_t block_size;
    SwsUOpParams par;
} SwsAArch64OpImplParams;

/* SwsCompMask-related helpers. */
#define LOOP(mask, idx)                 \
    for (int idx = 0; idx < 4; idx++)   \
        if (mask & SWS_COMP(idx))
#define LOOP_BWD(mask, idx)             \
    for (int idx = 3; idx >= 0; idx--)  \
        if (mask & SWS_COMP(idx))

#define LOOP_MASK(p, idx) LOOP(p->mask, idx)
#define LOOP_MASK_BWD(p, idx) LOOP_BWD(p->mask, idx)

/**
 * Number of coefficients in each linear coefficient vector register.
 * Integer multiplication by element does not exist for 8-bit elements, so
 * u8 coefficients are stored pre-broadcast, one per vector.
 */
static inline int linear_vreg_nelems(SwsPixelType type)
{
    return type == SWS_PIXEL_U8 ? 1 : 16 / ff_sws_pixel_type_size(type);
}

/**
 * Mask of the linear coefficients that are used as multiplication operands,
 * and therefore packed into the coefficient vectors. This excludes offsets
 * (stored pre-broadcast), the first coefficient of a row without an offset
 * if it is one (a move), and coefficients equal to one in the fmul+fadd
 * path (an addition).
 */
static inline uint32_t linear_coeff_mask(const SwsAArch64OpImplParams *params)
{
    const SwsLinearUOp *lin = &params->par.lin;
    const bool split_f32 = params->type == SWS_PIXEL_F32 &&
                           params->uop  == SWS_UOP_LINEAR;
    uint32_t mask = 0;
    LOOP_MASK(params, i) {
        bool first = !!(lin->zero & SWS_MASK_OFF(i));
        for (int j = 0; j < 4; j++) {
            if (lin->zero & SWS_MASK(i, j))
                continue;
            const bool is_one = !!(lin->one & SWS_MASK(i, j));
            if (!(is_one && (first || split_f32)))
                mask |= SWS_MASK(i, j);
            first = false;
        }
    }
    return mask;
}

/* Compute number of vector registers needed to store the packed coefficients. */
static inline int linear_num_vregs(const SwsAArch64OpImplParams *params)
{
    const uint32_t mask = linear_coeff_mask(params);
    int count = 0;
    for (int i = 0; i < 20; i++)
        count += !!(mask & (1u << i));
    const int nelems = linear_vreg_nelems(params->type);
    return (count + nelems - 1) / nelems;
}

/**
 * Number of rows with a non-zero offset. These offsets are additionally stored
 * pre-broadcast after the coefficient vectors, one vector per row.
 */
static inline int linear_num_offsets(const SwsAArch64OpImplParams *params)
{
    int count = 0;
    LOOP_MASK(params, i) {
        if (!(params->par.lin.zero & SWS_MASK_OFF(i)))
            count++;
    }
    return count;
}

/**
 * Components of SWS_UOP_CLEAR/ADD/MIN/MAX whose constant is stored in
 * impl->priv, as one pre-broadcast vector per component (in component order).
 */
static inline SwsCompMask const_vec_mask(const SwsAArch64OpImplParams *params)
{
    SwsCompMask mask = params->mask;
    if (params->uop == SWS_UOP_CLEAR)
        mask &= ~(params->par.clear.zero | params->par.clear.one);
    return mask;
}

static inline int const_vec_count(const SwsAArch64OpImplParams *params)
{
    int count = 0;
    LOOP(const_vec_mask(params), i)
        count++;
    return count;
}

/**
 * Number of bytes of private data consumed by each kernel; must match what
 * aarch64_setup() packs.
 */
static inline int aarch64_priv_size(const SwsAArch64OpImplParams *params)
{
    switch (params->uop) {
    case SWS_UOP_READ_BIT:
    case SWS_UOP_WRITE_BIT:
        return 16; /* one full vector register */
    case SWS_UOP_CLEAR:
    case SWS_UOP_ADD:
    case SWS_UOP_MIN:
    case SWS_UOP_MAX:
        return 16 * const_vec_count(params); /* pre-broadcast constants */
    case SWS_UOP_SCALE:
        /* pre-broadcast for u8, scalar otherwise */
        return params->type == SWS_PIXEL_U8 ? 16 : ff_sws_pixel_type_size(params->type);
    case SWS_UOP_LINEAR:
    case SWS_UOP_LINEAR_FMA:
        /* packed coefficients, then pre-broadcast offsets */
        return 16 * (linear_num_vregs(params) + linear_num_offsets(params));
    case SWS_UOP_DITHER:
        return 8; /* pointer */
    default:
        return 0;
    }
}

/**
 * These values will be used by ops_asmgen to access fields inside of
 * SwsOpExec and SwsOpImpl. The sizes are checked in aarch64/ops.c when
 * compiling for AArch64 to make sure there is no mismatch.
 */
#define offsetof_exec_in         0
#define offsetof_exec_out       32
#define offsetof_exec_in_bump  128
#define offsetof_exec_out_bump 160
#define offsetof_impl_func       0
#define offsetof_impl_priv       8
#define alignof_impl             8

/**
 * Offset of the kernel data from the start of its SwsOpImpl. Every
 * SwsOpImpl takes a multiple of 16 bytes (see aarch64_impl_size()) and the
 * chain is 16-byte aligned, so data that holds vectors is placed at offset
 * 16 to keep it 16-byte aligned. This allows loading two vectors with a
 * single ldp, and keeps vectors from crossing cache lines.
 */
static inline int aarch64_priv_offset(const SwsAArch64OpImplParams *params)
{
    switch (params->uop) {
    case SWS_UOP_SCALE:
        return params->type == SWS_PIXEL_U8 ? 16 : offsetof_impl_priv;
    case SWS_UOP_DITHER:
        return offsetof_impl_priv; /* pointer */
    default:
        return aarch64_priv_size(params) ? 16 : offsetof_impl_priv;
    }
}

/* Number of bytes taken by the SwsOpImpl of each kernel. */
static inline int aarch64_impl_size(const SwsAArch64OpImplParams *params)
{
    const int size = aarch64_priv_offset(params) + aarch64_priv_size(params);
    return (size + 15) & ~15;
}

#endif /* SWSCALE_AARCH64_OPS_IMPL_H */
