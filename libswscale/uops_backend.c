/**
 * Copyright (C) 2026 Niklas Haas
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

#include "libavutil/avassert.h"

#include "uops_entries.h"
#include "uops_entries_c.h"
#include "uops_tmpl.h"

/**
 * We want to disable FP contraction because this is a reference backend that
 * establishes a bit-exact reference result.
 */
#ifdef __clang__
#pragma STDC FP_CONTRACT OFF
#elif defined(__GNUC__)
#pragma GCC optimize ("fp-contract=off")
#elif defined(_MSC_VER)
#pragma fp_contract (off)
#endif

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("finite-math-only")
#endif

/* Integer types */
#define IS_FLOAT 0
#  define BIT_DEPTH 8
#    include "uops_tmpl.c"
#  undef BIT_DEPTH
#  define BIT_DEPTH 16
#    include "uops_tmpl.c"
#  undef BIT_DEPTH
#  define BIT_DEPTH 32
#    include "uops_tmpl.c"
#  undef BIT_DEPTH
#undef IS_FLOAT

/* Floating point types */
#define IS_FLOAT 1
#  define BIT_DEPTH 32
#    include "uops_tmpl.c"
#  undef BIT_DEPTH
#undef IS_FLOAT

#define C_PIXEL_U8  uint8_t
#define C_PIXEL_U16 uint16_t
#define C_PIXEL_U32 uint32_t
#define C_PIXEL_F32 float

/* Plane pointer arguments of read and write kernels */
#define C_ARGS_PLAIN(TYPE)
#define C_ARGS_READ(TYPE)                                                       \
    , (const C_PIXEL_##TYPE *) iter->in[0], (const C_PIXEL_##TYPE *) iter->in[1], \
      (const C_PIXEL_##TYPE *) iter->in[2], (const C_PIXEL_##TYPE *) iter->in[3]
#define C_ARGS_WRITE(TYPE)                                                      \
    , (C_PIXEL_##TYPE *) iter->out[0], (C_PIXEL_##TYPE *) iter->out[1],         \
      (C_PIXEL_##TYPE *) iter->out[2], (C_PIXEL_##TYPE *) iter->out[3]

#define C_SETUP_NONE(TYPE)      NULL
#define C_SETUP_SCALAR(TYPE)    ff_sws_setup_scalar
#define C_SETUP_VEC4(TYPE)      ff_sws_setup_vec4
#define C_SETUP_FILTER_V(TYPE)  bitfn(setup_filter_v, TYPE)
#define C_SETUP_FILTER_H(TYPE)  bitfn(setup_filter_h, TYPE)
#define C_SETUP_DITHER(TYPE)    bitfn(setup_dither, TYPE)
#define C_SETUP_LINEAR(TYPE)    bitfn(setup_linear, TYPE)
#define C_SETUP_LUT3D(TYPE)     bitfn(setup_lut3d, TYPE)

/* Template function, plane arguments and setup function of each uop type */
#define C_READ_PLANAR       read_planar,    C_ARGS_READ,  C_SETUP_NONE
#define C_READ_PLANAR_FH    read_planar_fh, C_ARGS_READ,  C_SETUP_FILTER_H
#define C_READ_PLANAR_FV    read_planar_fv, C_ARGS_READ,  C_SETUP_FILTER_V
#define C_READ_PACKED       read_packed,    C_ARGS_READ,  C_SETUP_NONE
#define C_READ_NIBBLE       read_nibble,    C_ARGS_READ,  C_SETUP_NONE
#define C_READ_BIT          read_bit,       C_ARGS_READ,  C_SETUP_NONE
#define C_READ_PALETTE      read_palette,   C_ARGS_READ,  C_SETUP_NONE
#define C_WRITE_PLANAR      write_planar,   C_ARGS_WRITE, C_SETUP_NONE
#define C_WRITE_PACKED      write_packed,   C_ARGS_WRITE, C_SETUP_NONE
#define C_WRITE_NIBBLE      write_nibble,   C_ARGS_WRITE, C_SETUP_NONE
#define C_WRITE_BIT         write_bit,      C_ARGS_WRITE, C_SETUP_NONE
#define C_PERMUTE           permute,        C_ARGS_PLAIN, C_SETUP_NONE
#define C_COPY              copy,           C_ARGS_PLAIN, C_SETUP_NONE
#define C_SWAP_BYTES        swap_bytes,     C_ARGS_PLAIN, C_SETUP_NONE
#define C_EXPAND_BIT        expand_bit,     C_ARGS_PLAIN, C_SETUP_NONE
#define C_EXPAND_PAIR       expand_pair,    C_ARGS_PLAIN, C_SETUP_NONE
#define C_EXPAND_QUAD       expand_quad,    C_ARGS_PLAIN, C_SETUP_NONE
#define C_TO_U8             to_u8,          C_ARGS_PLAIN, C_SETUP_NONE
#define C_TO_U16            to_u16,         C_ARGS_PLAIN, C_SETUP_NONE
#define C_TO_U32            to_u32,         C_ARGS_PLAIN, C_SETUP_NONE
#define C_TO_F32            to_f32,         C_ARGS_PLAIN, C_SETUP_NONE
#define C_SCALE             scale,          C_ARGS_PLAIN, C_SETUP_SCALAR
#define C_ADD               add,            C_ARGS_PLAIN, C_SETUP_VEC4
#define C_MIN               min,            C_ARGS_PLAIN, C_SETUP_VEC4
#define C_MAX               max,            C_ARGS_PLAIN, C_SETUP_VEC4
#define C_UNPACK            unpack,         C_ARGS_PLAIN, C_SETUP_NONE
#define C_PACK              pack,           C_ARGS_PLAIN, C_SETUP_NONE
#define C_LSHIFT            lshift,         C_ARGS_PLAIN, C_SETUP_NONE
#define C_RSHIFT            rshift,         C_ARGS_PLAIN, C_SETUP_NONE
#define C_CLEAR             clear,          C_ARGS_PLAIN, C_SETUP_VEC4
#define C_LINEAR            linear,         C_ARGS_PLAIN, C_SETUP_LINEAR
#define C_DITHER            dither,         C_ARGS_PLAIN, C_SETUP_DITHER
#define C_LUT_3D            lut3d,          C_ARGS_PLAIN, C_SETUP_LUT3D

#define C_DECL_IMPL(FUNC, PLANE_ARGS, SETUP, NAME, TYPE, ARGS, INIT)            \
    av_flatten static void NAME##_c(SwsOpIter *restrict iter,                   \
                                    const SwsOpImpl *restrict impl,             \
                                    void *restrict x, void *restrict y,         \
                                    void *restrict z, void *restrict w)         \
    {                                                                           \
        bitfn(FUNC, TYPE)(iter, impl, x, y, z, w,                               \
                          SWS_UNPAREN ARGS PLANE_ARGS(TYPE));                   \
    }                                                                           \
                                                                                \
    static const SwsUOpEntry uop_##NAME = {                                     \
        SWS_UNPAREN INIT,                                                       \
        .func  = (SwsFuncPtr) NAME##_c,                                         \
        .setup = SETUP(TYPE),                                                   \
    };

/* Expands C_<UOP> into the first arguments of C_DECL_IMPL() */
#define C_DECL(...) C_DECL_IMPL(__VA_ARGS__)
#define C_ENTRY(DUMMY, NAME, TYPE, UOP, BLOCK_SIZE, ARGS, INIT)                 \
    C_DECL(C_##UOP, NAME, TYPE, ARGS, INIT)
#define C_REF(DUMMY, NAME, ...) &uop_##NAME,

SWS_ENTRIES_C(C_ENTRY, 0)

static const SwsUOpTable uop_table = {
    .entries = {
        SWS_ENTRIES_C(C_REF, 0)
        NULL
    },
};

static void process_c(const SwsOpExec *exec, const void *priv,
                      const int bx_start, const int y_start,
                      int bx_end, int y_end)
{
    const SwsOpChain *chain = priv;
    const SwsOpImpl *impl = (const SwsOpImpl *) chain->impl;
    block_t x, y, z, w; /* allocate enough space for any intermediate */

    SwsOpIter iter;
    iter.exec = exec;
    for (int i = 0; i < 4; i++) {
        iter.in[i]  = (uintptr_t) exec->in[i];
        iter.out[i] = (uintptr_t) exec->out[i];
    }

    void (*entry)(SwsOpIter *, const SwsOpImpl *, void *, void *, void *, void *);
    entry = (void (*)(SwsOpIter *, const SwsOpImpl *,
                      void *, void *, void *, void *)) impl->func;

    for (iter.y = y_start; iter.y < y_end; iter.y++) {
        for (int block = bx_start; block < bx_end; block++) {
            iter.x = block * SWS_BLOCK_SIZE;
            entry(&iter, impl, &x, &y, &z, &w);
        }

        const int y_bump = exec->in_bump_y ? exec->in_bump_y[iter.y] : 0;
        for (int i = 0; i < 4; i++) {
            iter.in[i]  += exec->in_bump[i] + y_bump * exec->in_stride[i];
            iter.out[i] += exec->out_bump[i];
        }
    }
}

static int compile_uops_c(SwsContext *ctx, const SwsUOpList *uops, SwsCompiledOp *out)
{
    int ret;

    SwsOpChain *chain = ff_sws_op_chain_alloc();
    if (!chain)
        return AVERROR(ENOMEM);

    av_assert0(uops->num_ops > 0);
    for (int i = 0; i < uops->num_ops; i++) {
        const SwsUOpTable *table = &uop_table;
        SwsUOp uop = uops->ops[i];
        uop.block_size = SWS_BLOCK_SIZE;
        ret = ff_sws_uop_lookup(ctx, &table, 1, &uop, chain);
        if (ret < 0)
            goto fail;
    }

    *out = (SwsCompiledOp) {
        .block_size  = SWS_BLOCK_SIZE,
        .slice_align = 1,
        .func        = process_c,
    };

    ff_sws_uop_list_print(ctx, AV_LOG_DEBUG, uops);
    ff_sws_op_chain_attach(chain, out);
    return 0;

fail:
    ff_sws_op_chain_free(chain);
    return ret;
}

static int compile_ops_c(SwsContext *ctx, const SwsOpList *ops, SwsCompiledOp *out)
{
    SwsUOpList *uops = ff_sws_uop_list_alloc();
    if (!uops)
        return AVERROR(ENOMEM);

    const SwsUOpTarget target = {
        .policy = &ff_sws_uop_policy_c,
        .flags  = ctx->flags,
    };
    int ret = ff_sws_ops_translate(ctx, ops, &target, uops);
    if (ret < 0)
        goto fail;

    ret = compile_uops_c(ctx, uops, out);

fail:
    ff_sws_uop_list_free(&uops);
    return ret;
}

const SwsOpBackend backend_c = {
    .name           = "c",
    .flags          = SWS_BACKEND_C,
    .compile        = compile_ops_c,
    .compile_uops   = compile_uops_c,
    .hw_format      = AV_PIX_FMT_NONE,
};
