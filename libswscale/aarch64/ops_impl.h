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
 * Parameters for the structured load/store variant of SWS_UOP_RW_SHUFFLE
 * (see shuffle_ldst() in ops_impl_conv.c). Each pixel consists of elements
 * of the size of SwsAArch64OpImplParams.type, which are deinterleaved with
 * ldN, and interleaved back with stN.
 */
typedef struct SwsAArch64ShuffleLdSt {
    uint8_t num_in;     /* elements per input pixel (0 if unused) */
    uint8_t num_out;    /* elements per output pixel */
    int8_t  src[4];     /* input element of each output element, or -1 */
} SwsAArch64ShuffleLdSt;

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
    SwsAArch64ShuffleLdSt ldst;
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

/* Multiplication instructions have by-element variants for some types. */
static inline bool mul_by_element(SwsPixelType type)
{
    switch (type) {
    case SWS_PIXEL_U16:
    case SWS_PIXEL_U32:
    case SWS_PIXEL_F32:
        return true;
    default:
        return false;
    }
}

/**
 * Layout of the SWS_UOP_RW_SHUFFLE functions. The shuffle pattern from the
 * uop is repeated for as many groups as fit in SHUFFLE_MAX_BYTES of input
 * and output, which are then loaded into (at most) 8 input vector
 * registers and stored from (at most) 8 output vector registers. Each
 * output vector register is computed with a single tbl instruction over
 * the range of (at most 4) input vector registers it references.
 *
 * Since each output byte only references input bytes from its own group,
 * this range can be determined from the read and write sizes alone,
 * without looking at the shuffle mask.
 *
 * For the same reason, output vector registers with the same number of
 * bytes, which start at the same offset within their group and reference
 * the same input offsets relative to their first input vector register,
 * have the same tbl indices, and share a single index vector.
 *
 * The `bytes` parameter of shuffle_layout() overrides SHUFFLE_MAX_BYTES
 * for the lane mode (see shuffle_lane_bytes()), if it is not 0.
 *
 * Without a clear value, the cleared bytes get an out of range index, so
 * that tbl sets them to zero, and the table slots are the input vector
 * registers (clr_tbl 0). A non-zero clear value is taken by the tbl from a
 * vector register that holds the clear value in every byte, and which is
 * part of the table of each output vector register. Since the table
 * registers must be consecutive, copies of that register are placed
 * between the input vector registers, in one of two arrangements of the
 * table slots:
 *  clr_tbl 1: in0 c in1 c in2 c in3 c ...
 *  clr_tbl 2: in0 c in1 in2 c in3 in4 c ...
 * The second one needs fewer copies, but the table position of the inputs
 * depends on the parity of the first input of each output, which may need
 * more distinct tbl index vectors. The first arrangement that fits in the
 * vector registers is used. If neither fits, the shuffle is not supported.
 */
#define SHUFFLE_MAX_BYTES 128

typedef struct SwsAArch64ShuffleLayout {
    int groups;     /* number of shuffle groups per iteration */
    int in_bytes;   /* input bytes per iteration */
    int out_bytes;  /* output bytes per iteration */
    int num_in;     /* number of input vector registers */
    int num_out;    /* number of output vector registers */
    int lo[8];      /* first input vector register used by each output */
    int hi[8];      /* last input vector register used by each output */
    int num_idx;    /* number of distinct tbl index vectors */
    int idx[8];     /* tbl index vector used by each output */
    int clr_tbl;    /* arrangement of the table slots (see above) */
    int num_slots;  /* number of table slots (inputs and clear value copies) */
    int tlo[8];     /* first table slot of each output */
    int tnum[8];    /* number of table slots of each output */
} SwsAArch64ShuffleLayout;

/* Table slot of input vector register i. */
static inline int shuffle_slot(int clr_tbl, int i)
{
    switch (clr_tbl) {
    case 1:  return 2 * i;
    case 2:  return 3 * (i >> 1) + 2 * (i & 1);
    default: return i;
    }
}

/* Whether table slot n holds a copy of the clear value register. */
static inline int shuffle_slot_is_clr(int clr_tbl, int n)
{
    return clr_tbl == 1 ? n & 1 : clr_tbl == 2 ? n % 3 == 1 : 0;
}

/* Table slot of the clear value register used by output k (clr_tbl != 0). */
static inline int shuffle_clr_slot(const SwsAArch64ShuffleLayout *l, int k)
{
    const int n = shuffle_slot(l->clr_tbl, l->lo[k]);
    return l->clr_tbl == 2 && (l->lo[k] & 1) ? n - 1 : n + 1;
}

/**
 * Determine the tables and the shared tbl index vectors for the
 * arrangement clr_tbl. Returns -1 if it doesn't fit.
 */
static inline int shuffle_tables(int read_size, int write_size, int clr_tbl,
                                 SwsAArch64ShuffleLayout *l)
{
    l->clr_tbl   = clr_tbl;
    l->num_slots = shuffle_slot(clr_tbl, l->num_in - 1) + 1;
    for (int k = 0; k < l->num_out; k++) {
        int t0 = shuffle_slot(clr_tbl, l->lo[k]);
        int t1 = shuffle_slot(clr_tbl, l->hi[k]);
        if (clr_tbl) {
            const int c = shuffle_clr_slot(l, k);
            t0 = c < t0 ? c : t0;
            t1 = c > t1 ? c : t1;
        }
        if (t1 - t0 >= 4)
            return -1; /* tbl takes at most 4 table registers */
        l->tlo[k]  = t0;
        l->tnum[k] = t1 - t0 + 1;
        if (t1 + 1 > l->num_slots)
            l->num_slots = t1 + 1;
    }

    int idx_key[8][4];
    l->num_idx = 0;
    for (int k = 0; k < l->num_out; k++) {
        const int out_start = 16 * k;
        const int len = l->out_bytes - out_start < 16 ? l->out_bytes - out_start : 16;
        const int offset = out_start % write_size;
        const int in_shift = (out_start / write_size) * read_size - 16 * l->lo[k];
        const int parity = clr_tbl == 2 ? l->lo[k] & 1 : 0;

        int i;
        for (i = 0; i < l->num_idx; i++) {
            if (idx_key[i][0] == offset && idx_key[i][1] == in_shift &&
                idx_key[i][2] == len && idx_key[i][3] == parity)
                break;
        }
        if (i == l->num_idx) {
            idx_key[i][0] = offset;
            idx_key[i][1] = in_shift;
            idx_key[i][2] = len;
            idx_key[i][3] = parity;
            l->num_idx++;
        }
        l->idx[k] = i;
    }

    /**
     * The table slots are placed in v16-v31, and the tbl index vectors and
     * output vector registers in the remaining registers of v16-v31 and
     * v0-v7 (see asmgen_shuffle()).
     */
    if (clr_tbl && (l->num_slots > 16 || l->num_slots + l->num_idx + l->num_out > 24))
        return -1;
    return 0;
}

static inline int shuffle_layout(int read_size, int write_size, int bytes,
                                 int clear, SwsAArch64ShuffleLayout *l)
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
     *
     * The input must leave a remainder of 0 bytes or a power of two after
     * the whole vector registers, so that the last input vector register is
     * loaded exactly with a single scalar load (see shuffle_load()), and
     * nothing is read past the end of the input. 8 groups always satisfy
     * this, since max_size is at most 16.
     */
    int fallback = 0;
    l->groups = 0;
    for (int g = bytes / max_size; g > 0; g--) {
        const int tail = (g * read_size) & 15;
        if (tail & (tail - 1))
            continue;
        if (!fallback)
            fallback = g;
        if ((g * write_size) % 16 == 0) {
            l->groups = g;
            break;
        }
    }
    if (!l->groups)
        l->groups = fallback;
    if (!l->groups)
        return -1;
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

    if (!clear)
        return shuffle_tables(read_size, write_size, 0, l);
    for (int clr_tbl = 1; clr_tbl <= 2; clr_tbl++)
        if (!shuffle_tables(read_size, write_size, clr_tbl, l))
            return 0;
    return -1;
}

/**
 * Register offset between the input and output elements of a structured
 * load/store shuffle: output element k is stored from the vector register
 * that input element k - offset was loaded into. shuffle_ldst() only
 * accepts shuffles with a single offset, and with cleared output elements
 * in registers that are never loaded into.
 */
static inline int shuffle_ldst_offset(const SwsAArch64ShuffleLdSt *ldst)
{
    for (int k = 0; k < ldst->num_out; k++)
        if (ldst->src[k] >= 0)
            return k - ldst->src[k];
    return 0;
}

/* Number of ldN/stN pairs per iteration of a structured load/store shuffle. */
static inline int shuffle_ldst_count(const SwsAArch64ShuffleLdSt *ldst)
{
    const int n = ldst->num_in > ldst->num_out ? ldst->num_in : ldst->num_out;
    return SHUFFLE_MAX_BYTES / (16 * n);
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
        return 16; /* one full vector register */
    case SWS_UOP_MUL:
        if (mul_by_element(params->type))
            return 16;
        av_fallthrough;
    case SWS_UOP_ADD:
    case SWS_UOP_MIN:
    case SWS_UOP_MAX:
        return 16 * ff_sws_vec4_count(&params->par.vec4, params->mask);
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
