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
    /* SWS_UOP_CLEAR: 1 + index of an earlier loaded component with the
     * same value, or 0 if the value has its own slot. */
    uint8_t clear_dup[4];
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

/* Number of elements in each linear coefficient vector register. */
static inline int linear_vreg_nelems(SwsPixelType type)
{
    return 16 / ff_sws_pixel_type_size(type);
}

/* Compute number of vector registers needed to store all coefficients. */
static inline int linear_num_vregs(const SwsAArch64OpImplParams *params)
{
    int count = 0;
    LOOP_MASK(params, i) {
        for (int j = 0; j < 5; j++)
            if (!(params->par.lin.zero & SWS_MASK(i, j)))
                count++;
    }
    const int nelems = linear_vreg_nelems(params->type);
    return (count + nelems - 1) / nelems;
}

/**
 * Clear kernel implementation:
 *  1: all clear values in one vector, broadcast to each lane with dup
 *  2: one pre-broadcast vector per value, loaded into vl and into vh with
 *     ldp/ldr
 *  3: as 2, with the vl and vh loads paired into the same ldp sequence
 *  4: as 2 for vl, vh copied from vl with mov
 *  5: as 2, but a single value is loaded into vl and into vh with ld1r
 *  6: as 5, vh copied from vl with mov
 */
#ifndef AARCH64_CLEAR_VARIANT
#define AARCH64_CLEAR_VARIANT 2
#endif

/**
 * Deduplication of clear values (variants 2 and 4 only):
 *  0: one slot per loaded component
 *  1: one slot per distinct value, duplicates loaded again from the slot
 *  2: one slot per distinct value, duplicates copied with mov
 */
#ifndef AARCH64_CLEAR_DEDUP
#define AARCH64_CLEAR_DEDUP 0
#endif
#define AARCH64_CLEAR_USE_DEDUP \
    (AARCH64_CLEAR_DEDUP && (AARCH64_CLEAR_VARIANT == 2 || AARCH64_CLEAR_VARIANT == 4))

static inline int clear_vec_size(const SwsAArch64OpImplParams *params)
{
    const int total_size = params->block_size * ff_sws_pixel_type_size(params->type);
    return total_size < 16 ? total_size : 16;
}

static inline int clear_use_vh(const SwsAArch64OpImplParams *params)
{
    return params->block_size * ff_sws_pixel_type_size(params->type) > 16;
}

/* Components whose clear value is loaded from the kernel data. */
static inline SwsCompMask clear_load_mask(const SwsAArch64OpImplParams *params)
{
    return params->mask & ~(params->par.clear.zero | params->par.clear.one);
}

/* Number of pre-broadcast vectors in the clear kernel data. */
static inline int clear_num_slots(const SwsAArch64OpImplParams *params)
{
    int count = 0;
    LOOP(clear_load_mask(params), i) {
        if (!params->clear_dup[i])
            count++;
    }
    if (AARCH64_CLEAR_VARIANT == 3 && clear_use_vh(params))
        count *= 2;
    return count;
}

static inline int clear_priv_size(const SwsAArch64OpImplParams *params)
{
    if (AARCH64_CLEAR_VARIANT == 1)
        return 16;
    return clear_num_slots(params) * clear_vec_size(params);
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
    case SWS_UOP_ADD:
    case SWS_UOP_MIN:
    case SWS_UOP_MAX:
        return 16; /* one full vector register */
    case SWS_UOP_CLEAR:
        return clear_priv_size(params);
    case SWS_UOP_SCALE:
        return ff_sws_pixel_type_size(params->type);
    case SWS_UOP_LINEAR:
    case SWS_UOP_LINEAR_FMA:
        return 16 * linear_num_vregs(params); /* packed coefficients */
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
 * Align the data inside SwsOpImpl.impl to 16 bytes, to allow aligned
 * load of vector data using ldr/ldp.
 */
#define AARCH64_IMPL_DATA_ALIGN     16
#define AARCH64_IMPL_DATA_OFFSET    FFALIGN(offsetof_impl_priv, AARCH64_IMPL_DATA_ALIGN)

#endif /* SWSCALE_AARCH64_OPS_IMPL_H */
