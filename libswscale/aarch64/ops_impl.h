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
    uint8_t in_planar;  /* each input element is in its own plane */
    uint8_t out_planar; /* each output element is in its own plane */
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
    uint8_t shuffle_planes[4]; /* input planes used by each output plane, for
                                * shuffles with several input planes */
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
    int in_bytes;   /* input bytes per iteration (and plane) */
    int out_bytes;  /* output bytes per iteration (and plane) */
    int in_planes;  /* number of input planes */
    int out_planes; /* number of output planes */
    int num_in;     /* number of input vector registers (per plane) */
    int num_out;    /* number of output vector registers (per plane) */
    int lo[8];      /* first input vector register used by each output (per plane) */
    int hi[8];      /* last input vector register used by each output (per plane) */
    int num_idx;    /* number of distinct tbl index vectors */
    int idx[8];     /* tbl index vector used by each output (q * num_out + j) */
    int clr_tbl;    /* arrangement of the table slots (see above) */
    int num_slots;  /* number of table slots (inputs and clear value copies) */
    int slot_in[16];/* input vector register (p * num_in + r) of each slot, or -1 */
    int in_slot[8]; /* table slot of each input vector register */
    int tlo[8];     /* first table slot of each output */
    int tnum[8];    /* number of table slots of each output */
    int clr_pos[8]; /* position of the clear value register in the table of each
                     * output, or -1 */
    int need_tmp;   /* a temporary register is needed for partial stores */
} SwsAArch64ShuffleLayout;

/**
 * Table slot arrangements. 0 to 2 are for a single input plane (see above),
 * the others for several input planes, with the input vector registers
 * either in plane order (p0r0 p0r1 ... p1r0 ...) or interleaved by register
 * (p0r0 p1r0 ... p0r1 p1r1 ...):
 *  3: plane order
 *  4: interleaved
 *  5: interleaved, with a clear value copy after the registers of each r
 *  6: plane order, with a clear value copy after each register
 *  7: plane order, with a clear value copy after every other register
 */
#define SHUFFLE_ARRANGEMENTS 8

static inline int shuffle_slots(SwsAArch64ShuffleLayout *l, int clr_tbl)
{
    const int P = l->in_planes, n = l->num_in;
    int ns = 0;
#define IN(p, r) do { l->slot_in[ns++] = (p) * n + (r); } while (0)
#define CLR()    do { l->slot_in[ns++] = -1; } while (0)
    if (P * n > 8)
        return -1; /* at most 2 * 8 slots */
    switch (clr_tbl) {
    case 0: for (int r = 0; r < n; r++) IN(0, r); break;
    case 1: for (int r = 0; r < n; r++) { IN(0, r); CLR(); } break;
    case 2:
        for (int r = 0; r < n; r++) {
            IN(0, r);
            if (!(r & 1))
                CLR();
        }
        break;
    case 3: for (int p = 0; p < P; p++) for (int r = 0; r < n; r++) IN(p, r); break;
    case 4: for (int r = 0; r < n; r++) for (int p = 0; p < P; p++) IN(p, r); break;
    case 5:
        for (int r = 0; r < n; r++) {
            for (int p = 0; p < P; p++)
                IN(p, r);
            CLR();
        }
        break;
    case 6: for (int p = 0; p < P; p++) for (int r = 0; r < n; r++) { IN(p, r); CLR(); } break;
    case 7:
        for (int p = 0, i = 0; p < P; p++) {
            for (int r = 0; r < n; r++, i++) {
                IN(p, r);
                if (!(i & 1))
                    CLR();
            }
        }
        break;
    default:
        return -1;
    }
#undef IN
#undef CLR
    l->num_slots = ns;
    for (int i = 0; i < ns; i++)
        if (l->slot_in[i] >= 0)
            l->in_slot[l->slot_in[i]] = i;
    return 0;
}

/**
 * Determine the tables and the shared tbl index vectors for the
 * arrangement clr_tbl. plane_map[q] is the mask of input planes used by
 * output plane q. Returns -1 if it doesn't fit.
 */
static inline int shuffle_tables(int read_size, int write_size, int clear, int clr_tbl,
                                 const uint8_t *plane_map, SwsAArch64ShuffleLayout *l)
{
    l->clr_tbl = clr_tbl;
    if (shuffle_slots(l, clr_tbl) < 0)
        return -1;

    int max_slot = 0;
    int keys[8][24];
    l->num_idx = 0;
    for (int q = 0; q < l->out_planes; q++) {
        const int map = l->in_planes > 1 ? plane_map[q] : 1;
        for (int j = 0; j < l->num_out; j++) {
            const int k = q * l->num_out + j;
            int t0 = 16, t1 = -1;
            for (int p = 0; p < l->in_planes; p++) {
                if (!(map >> p & 1))
                    continue;
                for (int r = l->lo[j]; r <= l->hi[j]; r++) {
                    const int t = l->in_slot[p * l->num_in + r];
                    t0 = t < t0 ? t : t0;
                    t1 = t > t1 ? t : t1;
                }
            }
            int c = -1;
            if (clear) {
                if ((clr_tbl == 1 || clr_tbl == 2) && t1 >= 0) {
                    /* the copy right after the first input, or right before it */
                    c = t0 + 1 < l->num_slots && l->slot_in[t0 + 1] < 0 ? t0 + 1 : t0 - 1;
                } else {
                    /* the copy that gives the smallest table */
                    int best = 99;
                    for (int t = 0; t < l->num_slots; t++) {
                        if (l->slot_in[t] >= 0)
                            continue;
                        const int lo = t1 < 0 || t < t0 ? t : t0;
                        const int hi = t1 < 0 || t > t1 ? t : t1;
                        if (hi - lo < best) {
                            best = hi - lo;
                            c = t;
                        }
                    }
                }
                if (c < 0)
                    return -1;
                if (t1 < 0)
                    t0 = t1 = c;
                t0 = c < t0 ? c : t0;
                t1 = c > t1 ? c : t1;
            }
            if (t1 < 0)
                t0 = t1 = 0; /* the output does not use any input */
            if (t1 - t0 >= 4)
                return -1; /* tbl takes at most 4 table registers */
            l->tlo[k]     = t0;
            l->tnum[k]    = t1 - t0 + 1;
            l->clr_pos[k] = c >= 0 ? c - t0 : -1;
            if (t1 > max_slot)
                max_slot = t1;

            /**
             * Outputs share the tbl indices if they belong to the same output
             * plane, start at the same offset within their group, have the
             * same length, reference the same input offset relative to their
             * first input vector register, and have the same table positions
             * for the inputs and the clear value.
             */
            const int out_start = 16 * j;
            int key[24], nk = 0;
            for (int i = 0; i < 24; i++)
                key[i] = -2;
            key[nk++] = q;
            key[nk++] = out_start % write_size;
            key[nk++] = l->out_bytes - out_start < 16 ? l->out_bytes - out_start : 16;
            key[nk++] = (out_start / write_size) * read_size - 16 * l->lo[j];
            key[nk++] = l->clr_pos[k];
            for (int p = 0; p < l->in_planes; p++) {
                if (!(map >> p & 1))
                    continue;
                for (int r = l->lo[j]; r <= l->hi[j] && nk < 24; r++)
                    key[nk++] = l->in_slot[p * l->num_in + r] - t0;
            }
            int i;
            for (i = 0; i < l->num_idx; i++)
                if (!memcmp(keys[i], key, sizeof(key)))
                    break;
            if (i == l->num_idx) {
                memcpy(keys[i], key, sizeof(key));
                l->num_idx++;
            }
            l->idx[k] = i;
        }
    }

    /**
     * Without a clear value, the input vector registers are placed in v0-v7,
     * and with one, the table slots are placed in v16-v31. The tbl index
     * vectors and the output vector registers (and a temporary register for
     * partial stores, if needed) go in the remaining registers of v0-v7 and
     * v16-v31 (see asmgen_shuffle()).
     */
    /* Drop the clear value copies after the last slot that is used. */
    for (int i = 0; i < l->in_planes * l->num_in; i++)
        if (l->in_slot[i] > max_slot)
            max_slot = l->in_slot[i];
    l->num_slots = max_slot + 1;

    l->need_tmp = l->out_planes > 1 && (l->out_bytes & 15);
    const int total = l->num_slots + l->num_idx + l->out_planes * l->num_out + l->need_tmp;
    if (clear ? l->num_slots > 16 : l->num_slots > 8)
        return -1;
    if (total > 24)
        return -1;
    return 0;
}

static inline int shuffle_layout(int read_size, int write_size, int in_planes,
                                 int out_planes, const uint8_t *plane_map, int bytes,
                                 int clear, SwsAArch64ShuffleLayout *l)
{
    in_planes  = in_planes  ? in_planes  : 1;
    out_planes = out_planes ? out_planes : 1;
    const int in_total  = in_planes  * read_size;
    const int out_total = out_planes * write_size;
    const int max_size  = in_total > out_total ? in_total : out_total;
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
     * this for a single plane, since max_size is at most 16.
     *
     * All of this is per plane, with at most 8 input and 8 output vector
     * registers in total.
     */
    int fallback = 0;
    l->groups = 0;
    for (int g = bytes / max_size; g > 0; g--) {
        const int tail = (g * read_size) & 15;
        if (tail & (tail - 1))
            continue;
        if (in_planes  * ((g * read_size  + 15) >> 4) > 8 ||
            out_planes * ((g * write_size + 15) >> 4) > 8)
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
    l->in_planes  = in_planes;
    l->out_planes = out_planes;
    l->in_bytes   = l->groups * read_size;
    l->out_bytes  = l->groups * write_size;
    l->num_in     = (l->in_bytes  + 15) >> 4;
    l->num_out    = (l->out_bytes + 15) >> 4;
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

    static const int single[2][3] = { { 0, -1, -1 }, { 1, 2, -1 } };
    static const int multi[2][4]  = { { 3, 4, -1, -1 }, { 5, 6, 7, -1 } };
    const int *arr = in_planes == 1 ? single[!!clear] : multi[!!clear];
    for (int i = 0; arr[i] >= 0; i++)
        if (!shuffle_tables(read_size, write_size, clear, arr[i], plane_map, l))
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

/**
 * Number of ldN/stN pairs per iteration of a structured load/store shuffle.
 * With a single input element and a planar output, stores dominate, and
 * storing 32 bytes to each output plane per iteration drops Cortex-A55 out
 * of write streaming mode (every output line is read before being written),
 * so only 64 bytes are processed per iteration.
 */
static inline int shuffle_ldst_count(const SwsAArch64ShuffleLdSt *ldst)
{
    const int n = ldst->num_in > ldst->num_out ? ldst->num_in : ldst->num_out;
    const int bytes = ldst->num_in == 1 && ldst->out_planar ? 64 : SHUFFLE_MAX_BYTES;
    return bytes / (16 * n) ? bytes / (16 * n) : 1;
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
