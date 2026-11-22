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
 * Layout of the SWS_UOP_RW_SHUFFLE fast path. The shuffle pattern from
 * the uop is repeated for as many groups as fit in SHUFFLE_MAX_BYTES of
 * input and output, which are then loaded into (at most) 8 input vector
 * registers and stored from (at most) 8 output vector registers. Each
 * output vector register is computed with a single tbl instruction over
 * the range of (at most 4) input vector registers it references.
 *
 * Since each output byte only references input bytes from its own group,
 * this range can be determined from the read and write sizes alone,
 * without looking at the shuffle mask.
 *
 * At most SHUFFLE_MAX_BYTES (64 or 128) bytes are processed per iteration
 * by default. The `bytes` parameter overrides this for the lane mode (see
 * shuffle_lane_bytes()).
 */
#ifndef SHUFFLE_MAX_BYTES
#define SHUFFLE_MAX_BYTES 64
#endif

typedef struct SwsAArch64ShuffleLayout {
    int groups;     /* number of shuffle groups per iteration */
    int in_bytes;   /* input bytes per iteration */
    int out_bytes;  /* output bytes per iteration */
    int num_in;     /* number of input vector registers */
    int num_out;    /* number of output vector registers */
    int lo[8];      /* first input vector register used by each output */
    int hi[8];      /* last input vector register used by each output */
} SwsAArch64ShuffleLayout;

static inline int shuffle_layout(int read_size, int write_size, int bytes,
                                 SwsAArch64ShuffleLayout *l)
{
    const int max_size = read_size > write_size ? read_size : write_size;
    if (!bytes)
        bytes = SHUFFLE_MAX_BYTES;
    if (!read_size || !write_size || max_size > bytes)
        return -1;
    /**
     * Prefer a number of groups for which the output is a whole number of
     * vector registers. Otherwise, the last output vector register is
     * stored partially (see asmgen_shuffle()). The stores must never overlap
     * between iterations, since that defeats the write streaming of whole
     * cache lines and doubles the memory traffic for large images.
     */
    l->groups = bytes / max_size;
    for (int g = l->groups; g > 0; g--) {
        if ((g * write_size) % 16 == 0) {
            l->groups = g;
            break;
        }
    }
    l->in_bytes  = l->groups * read_size;
    l->out_bytes = l->groups * write_size;
    l->num_in    = (l->in_bytes  + 15) >> 4;
    l->num_out   = (l->out_bytes + 15) >> 4;
    for (int k = 0; k < l->num_out; k++) {
        const int out_start = 16 * k;
        const int out_end   = 16 * (k + 1) < l->out_bytes ? 16 * (k + 1) : l->out_bytes;
        const int g0 = out_start / write_size;
        const int g1 = (out_end - 1) / write_size;
        l->lo[k] = (g0 * read_size) >> 4;
        l->hi[k] = ((g1 + 1) * read_size - 1) >> 4;
        if (l->hi[k] - l->lo[k] >= 4)
            return -1; /* tbl takes at most 4 table registers */
    }
    return 0;
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
    case SWS_UOP_CLEAR:
    case SWS_UOP_ADD:
    case SWS_UOP_MIN:
    case SWS_UOP_MAX:
        return 16; /* one full vector register */
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
