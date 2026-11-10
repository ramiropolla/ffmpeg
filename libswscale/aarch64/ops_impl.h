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
#include <stdbool.h>
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
    bool inorder; /* variant tuned for in-order cores (AV_CPU_FLAG_INORDER) */
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
 * Bit offset of component `i` in the pack pattern of SWS_UOP_PACK/UNPACK.
 */
static inline int pack_offset(const SwsAArch64OpImplParams *params, int i)
{
    int offset = 0;
    for (int j = i + 1; j < 4; j++)
        offset += params->par.pack.pattern[j];
    return offset;
}

/**
 * Whether component `i` of SWS_UOP_UNPACK needs to be masked after shifting.
 * The topmost field doesn't, if it extends up to the most significant bit.
 */
static inline bool unpack_needs_mask(const SwsAArch64OpImplParams *params, int i)
{
    const int bits = 8 * ff_sws_pixel_type_size(params->type);
    return pack_offset(params, i) + params->par.pack.pattern[i] != bits;
}

/**
 * Whether the terms of SWS_UOP_LINEAR are emitted interleaved across rows
 * and vector banks (round-robin by term index), instead of row by row. This
 * is done for all types except for the f32 fmul+fadd path.
 */
static inline bool linear_interleaved(const SwsAArch64OpImplParams *params)
{
    return params->type != SWS_PIXEL_F32 || params->uop == SWS_UOP_LINEAR_FMA;
}

/**
 * Inputs of an interleaved SWS_UOP_LINEAR that must be saved, since they are
 * read after their register has been overwritten. The first term of every
 * row (which writes the output register) is emitted before all other terms.
 */
static inline SwsCompMask linear_interleaved_save_mask(const SwsAArch64OpImplParams *params)
{
    SwsCompMask save_mask = 0;
    bool overwritten[4] = { false, false, false, false };
    for (int t = 0; t < 5; t++) {
        LOOP_MASK(params, i) {
            int n = 0;
            for (int j = 0; j < 5; j++) {
                int src_j = (j == 0) ? 4 : (j - 1);
                if (params->par.lin.zero & SWS_MASK(i, src_j))
                    continue;
                if (n++ != t)
                    continue;
                if (src_j != 4 && overwritten[src_j])
                    save_mask |= SWS_COMP(src_j);
            }
            if (t == 0)
                overwritten[i] = true;
        }
    }
    return save_mask;
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
 * Whether a conversion widens u8 to 32-bit elements with tbl, using one
 * index vector per 4 pixels stored in impl->priv.
 */
static inline bool convert_u8_tbl(const SwsAArch64OpImplParams *params)
{
    return (params->uop == SWS_UOP_TO_U32 || params->uop == SWS_UOP_TO_F32) &&
           params->type == SWS_PIXEL_U8;
}

/**
 * Components of SWS_UOP_CLEAR/MIN/MAX whose constant is stored in impl->priv,
 * as one pre-broadcast vector per component (in component order).
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
 * Whether the kernel for these parameters has a separate variant for
 * in-order cores (AV_CPU_FLAG_INORDER). Other kernels only have one
 * version, which is used on all cores.
 */
static inline bool has_inorder_variant(const SwsAArch64OpImplParams *params)
{
    switch (params->uop) {
    case SWS_UOP_TO_U16:
        /* The high halves are zero-extended with uxtl2 instead of zip2. */
        return params->type == SWS_PIXEL_U8 && params->block_size == 16;
    case SWS_UOP_TO_U32:
    case SWS_UOP_TO_F32:
        return params->type == SWS_PIXEL_U16;
    default:
        return false;
    }
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
    case SWS_UOP_MIN:
    case SWS_UOP_MAX:
        return 16 * const_vec_count(params); /* pre-broadcast constants */
    case SWS_UOP_SCALE:
        return 16; /* pre-broadcast scalar */
    case SWS_UOP_LINEAR:
    case SWS_UOP_LINEAR_FMA:
        /* packed coefficients, then pre-broadcast offsets */
        return 16 * (linear_num_vregs(params) + linear_num_offsets(params));
    case SWS_UOP_DITHER:
        return 8; /* pointer */
    case SWS_UOP_TO_U32:
    case SWS_UOP_TO_F32:
        /* tbl indices, one vector per 4 pixels */
        return convert_u8_tbl(params) ? 4 * params->block_size : 0;
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

#endif /* SWSCALE_AARCH64_OPS_IMPL_H */
