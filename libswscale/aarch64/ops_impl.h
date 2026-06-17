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

/* TODO make available in uops.h */
#define SWS_MASK(I, J)  (1 << (5 * (I) + (J)))

/**
 * SwsAArch64OpImplParams describes the parameters for an SwsUOpType
 * operation. It consists of simplified parameters from the SwsOp structure,
 * with the purpose of being straight-forward to implement and execute.
 */
typedef struct SwsAArch64OpImplParams {
    SwsUOpType          uop;
    SwsCompMask         mask;
    SwsPixelType        type;
    uint8_t block_size;
    SwsUOpParams par;
} SwsAArch64OpImplParams;

#define LOOP(mask, idx)                 \
    for (int idx = 0; idx < 4; idx++)   \
        if (mask & SWS_COMP(idx))
#define LOOP_BWD(mask, idx)             \
    for (int idx = 3; idx >= 0; idx--)  \
        if (mask & SWS_COMP(idx))

#define LOOP_MASK(p, idx) LOOP(p->mask, idx)
#define LOOP_MASK_BWD(p, idx) LOOP_BWD(p->mask, idx)

/* Compute number of vector registers needed to store all coefficients. */
static inline int linear_num_vregs(const SwsAArch64OpImplParams *params)
{
    int count = 0;
    for (int i = 0; i < 4 * 5; i++)
        if (!(params->par.lin.zero & (1ULL << i)))
            count++;
    return (count + 3) / 4;
}

/**
 * These values will be used by ops_asmgen to access fields inside of
 * SwsOpExec and SwsOpImpl. The sizes are checked below when compiling
 * for AArch64 to make sure there is no mismatch.
 */
#define offsetof_exec_in         0
#define offsetof_exec_out       32
#define offsetof_exec_in_bump  128
#define offsetof_exec_out_bump 160
#define offsetof_impl_cont       0
#define offsetof_impl_priv      16
#define sizeof_impl             32

#if ARCH_AARCH64 && HAVE_NEON
static_assert(offsetof_exec_in       == offsetof(SwsOpExec, in),       "SwsOpExec layout mismatch");
static_assert(offsetof_exec_out      == offsetof(SwsOpExec, out),      "SwsOpExec layout mismatch");
static_assert(offsetof_exec_in_bump  == offsetof(SwsOpExec, in_bump),  "SwsOpExec layout mismatch");
static_assert(offsetof_exec_out_bump == offsetof(SwsOpExec, out_bump), "SwsOpExec layout mismatch");
static_assert(offsetof_impl_cont     == offsetof(SwsOpImpl, cont),     "SwsOpImpl layout mismatch");
static_assert(offsetof_impl_priv     == offsetof(SwsOpImpl, priv),     "SwsOpImpl layout mismatch");
#endif

#endif /* SWSCALE_AARCH64_OPS_IMPL_H */
