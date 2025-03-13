/**
 * Copyright (C) 2025 Niklas Haas
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

#include "libavutil/cpu.h"

#include "ops_internal.h"
#include "ops_backend.h"

#ifndef BACKEND_NAME
#  define BACKEND_NAME c
#endif

#ifndef CPU_FLAGS
#  define CPU_FLAGS 0
#endif

static const OpImpl bitfn(op_table_int,    u8)[];
static const OpImpl bitfn(op_table_int,   u16)[];
static const OpImpl bitfn(op_table_int,   u32)[];
static const OpImpl bitfn(op_table_float, f32)[];

#define BIT_DEPTH 8
# include "ops_tmpl_int.c"
#undef BIT_DEPTH

#define BIT_DEPTH 16
# include "ops_tmpl_int.c"
#undef BIT_DEPTH

#define BIT_DEPTH 32
# include "ops_tmpl_int.c"
# include "ops_tmpl_float.c"
#undef BIT_DEPTH

static int compile(SwsOpList *ops, SwsCompiledOp *out_compiled)
{
    static const SwsOp dummy = { .comps.unused = { true, true, true, true }};
    const SwsOp *next = ops->num_ops > 1 ? &ops->ops[1] : &dummy;
    const OpImpl *table;
    const OpImpl *best = NULL;
    const void *priv = NULL;
    SwsOp op = ops->ops[0];
    int best_score = 0;

    const int cpu_flags = av_get_cpu_flags();
    if (CPU_FLAGS & ~cpu_flags)
        return AVERROR(ENOTSUP);

    /* These operations can be replaced by integer operations with
     * no change in behavior, so convert the type before matching */
    switch (op.op) {
    case SWS_OP_READ:
    case SWS_OP_WRITE:
    case SWS_OP_SWAP_BYTES:
    case SWS_OP_SWIZZLE:
        switch (op.type) {
        case SWS_PIXEL_F32: op.type = SWS_PIXEL_U32; break;
        }
    }

    switch (op.type) {
    case SWS_PIXEL_U8:  table = bitfn(op_table_int,    u8); break;
    case SWS_PIXEL_U16: table = bitfn(op_table_int,   u16); break;
    case SWS_PIXEL_U32: table = bitfn(op_table_int,   u32); break;
    case SWS_PIXEL_F32: table = bitfn(op_table_float, f32); break;
    default: return AVERROR(EINVAL);
    }

    for (; table[0].op.op != SWS_OP_INVALID; table++) {
        int score = ff_sws_op_match(&op, &table[0].op, next->comps);
        if (score > best_score) {
            best_score = score;
            best = &table[0];
        }
    }

    if (!best)
        return AVERROR(ENOTSUP);

    if (best->setup) {
        int ret = best->setup(&op, &priv);
        if (ret < 0)
            return ret;
    }

    *out_compiled = (SwsCompiledOp) {
        .chunk_size = SWS_CHUNK_SIZE,
        .alignment  = SWS_ALIGNMENT,
        .func       = best->func,
        .func_n     = best->func_n,
        .priv       = priv,
        .free_priv  = best->free,
    };

    ops->ops++;
    ops->num_ops--;
    return 0;
}

SwsOpBackend AV_JOIN(backend_, BACKEND_NAME) = {
    .name    = AV_STRINGIFY(BACKEND_NAME),
    .compile = compile,
};
