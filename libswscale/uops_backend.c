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

#include "uops_tmpl.h"

/**
 * We want to disable FP contraction because this is a reference backend that
 * establishes a bit-exact reference result.
 */
#ifdef __clang__
#pragma STDC FP_CONTRACT OFF
#elif AV_GCC_VERSION_AT_LEAST(4, 8)
#pragma GCC optimize ("fp-contract=off")
#elif defined(_MSC_VER)
#pragma fp_contract (off)
#endif

#if AV_GCC_VERSION_AT_LEAST(4, 4)
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

/* Expanded as new uop types are implemented in the C/template backend */
#define REF_ALL_UOPS(TYPE)                                  \
    SWS_FOR(READ_PLANAR,    TYPE, REF_ENTRY)                \
    SWS_FOR(READ_PLANAR_FV, TYPE, REF_ENTRY)                \
    SWS_FOR(READ_PLANAR_FH, TYPE, REF_ENTRY)                \
    SWS_FOR(READ_PACKED,    TYPE, REF_ENTRY)                \
    SWS_FOR(READ_NIBBLE,    TYPE, REF_ENTRY)                \
    SWS_FOR(READ_BIT,       TYPE, REF_ENTRY)                \
    SWS_FOR(WRITE_PLANAR,   TYPE, REF_ENTRY)                \
    SWS_FOR(WRITE_PACKED,   TYPE, REF_ENTRY)                \
    SWS_FOR(WRITE_NIBBLE,   TYPE, REF_ENTRY)                \
    SWS_FOR(WRITE_BIT,      TYPE, REF_ENTRY)                \
    SWS_FOR(SWAP_BYTES,     TYPE, REF_ENTRY)                \
    SWS_FOR(EXPAND_BIT,     TYPE, REF_ENTRY)                \
    SWS_FOR(EXPAND_PAIR,    TYPE, REF_ENTRY)                \
    SWS_FOR(EXPAND_QUAD,    TYPE, REF_ENTRY)                \
    SWS_FOR(PERMUTE,        TYPE, REF_ENTRY)                \
    SWS_FOR(COPY,           TYPE, REF_ENTRY)                \
    SWS_FOR(CONVERT,        TYPE, REF_ENTRY)                \
    SWS_FOR(SCALE,          TYPE, REF_ENTRY)                \
    SWS_FOR(ADD,            TYPE, REF_ENTRY)                \
    SWS_FOR(MIN,            TYPE, REF_ENTRY)                \
    SWS_FOR(MAX,            TYPE, REF_ENTRY)                \
    SWS_FOR(UNPACK,         TYPE, REF_ENTRY)                \
    SWS_FOR(PACK,           TYPE, REF_ENTRY)                \
    SWS_FOR(LSHIFT,         TYPE, REF_ENTRY)                \
    SWS_FOR(RSHIFT,         TYPE, REF_ENTRY)                \
    SWS_FOR(CLEAR,          TYPE, REF_ENTRY)                \
    SWS_FOR(LINEAR,         TYPE, REF_ENTRY)                \
    SWS_FOR(DITHER,         TYPE, REF_ENTRY)                \
    /* end of macro */

static const SwsOpTable op_table = {
    .block_size = SWS_BLOCK_SIZE,
    .uops = true,
    .entries = {
        REF_ALL_UOPS(U8)
        REF_ALL_UOPS(U16)
        REF_ALL_UOPS(U32)
        REF_ALL_UOPS(F32)
        NULL
    },
};

static void process(const SwsOpExec *exec, const void *priv,
                    const int bx_start, const int y_start,
                    int bx_end, int y_end)
{
    const SwsOpChain *chain = priv;
    const SwsOpImpl *impl = chain->impl;
    block_t x, y, z, w; /* allocate enough space for any intermediate */

    SwsOpIter iterdata;
    SwsOpIter *iter = &iterdata; /* for CONTINUE() macro to work */
    iter->exec = exec;
    for (int i = 0; i < 4; i++) {
        iter->in[i]  = (uintptr_t) exec->in[i];
        iter->out[i] = (uintptr_t) exec->out[i];
    }

    for (iter->y = y_start; iter->y < y_end; iter->y++) {
        for (int block = bx_start; block < bx_end; block++) {
            iter->x = block * SWS_BLOCK_SIZE;
            CONTINUE(&x, &y, &z, &w);
        }

        const int y_bump = exec->in_bump_y ? exec->in_bump_y[iter->y] : 0;
        for (int i = 0; i < 4; i++) {
            iter->in[i]  += exec->in_bump[i] + y_bump * exec->in_stride[i];
            iter->out[i] += exec->out_bump[i];
        }
    }
}

static int compile(SwsContext *ctx, SwsOpList *ops, SwsCompiledOp *out)
{
    int ret;

    SwsOpChain *chain = ff_sws_op_chain_alloc();
    if (!chain)
        return AVERROR(ENOMEM);

    SwsUOpList *uops = ff_sws_uop_list_alloc();
    if (!uops) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    ret = ff_sws_ops_translate(ops, uops);
    if (ret < 0)
        goto fail;

    av_assert0(uops->num_ops > 0);
    for (int i = 0; i < uops->num_ops; i++) {
        const SwsOpTable *table = &op_table;
        ret = ff_sws_uop_lookup(ctx, &table, 1, &uops->ops[i],
                                SWS_BLOCK_SIZE, chain);
        if (ret < 0)
            goto fail;
    }

    *out = (SwsCompiledOp) {
        .slice_align = 1,
        .block_size  = SWS_BLOCK_SIZE,
        .cpu_flags   = chain->cpu_flags,
        .over_read   = chain->over_read,
        .over_write  = chain->over_write,
        .priv        = chain,
        .free        = ff_sws_op_chain_free_cb,
        .func        = process,
    };

    av_log(ctx, AV_LOG_DEBUG, "Compiled micro-ops:\n");
    for (int i = 0; i < uops->num_ops; i++) {
        char name[SWS_UOP_NAME_MAX];
        ff_sws_uop_name(&uops->ops[i], name);
        av_log(ctx, AV_LOG_DEBUG, "    %s\n", name);
    }

    ff_sws_uop_list_free(&uops);
    return 0;

fail:
    ff_sws_uop_list_free(&uops);
    ff_sws_op_chain_free(chain);
    return ret;
}

const SwsOpBackend backend_uops = {
    .name       = "uops",
    .compile    = compile,
    .hw_format  = AV_PIX_FMT_NONE,
};
