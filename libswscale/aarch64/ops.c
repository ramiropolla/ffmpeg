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

#include "../ops_chain.h"

#include "libswscale/ffasm/aarch64.h"

#include "libavutil/avstring.h"

#define LOOP_ARRAY(idx, arr)          \
    for (int idx = 0; idx < 4; idx++) \
        if (arr[idx])
#define LOOP_IN(idx)  LOOP_ARRAY(idx, !op->comps.unused)

#define Q(N) ((AVRational) { N, 1 })
#define Q0   Q(0)
#define Q1   Q(1)

/*********************************************************************/
/* This structure holds implementation details for the AArch64 NEON
 * functions. It is a stripped down and simplified version of SwsOp.
 */
typedef struct SwsOpAArch64Impl {
    uint8_t op;             // same as SwsOp.op
    uint8_t use_vh;         // whether to use the high vector bank
    uint8_t type_int;       // work type is integer
    uint16_t work_mask;     // work
    uint16_t t;             // arrangement specifier
    union {
        uint8_t rw;
        uint8_t shift;
        uint16_t swizzle;
        uint16_t pattern;
        struct {
            uint8_t to_size;
            uint8_t to_int;
            uint8_t expand;
        } convert;
        uint64_t linear_mask;
    };
} SwsOpAArch64Impl;

/*********************************************************************/
/* Convert SwsOp to a simplified structure used to generate NEON implementations. */
static SwsOpAArch64Impl sws_op_aarch64_impl(const SwsOpList *ops, int block_size, int n)
{
    const SwsOp *op = &ops->ops[n];
    const SwsOp *next = n + 1 < ops->num_ops ? &ops->ops[n + 1] : op;

    /* The element size and count are used to select the arrangement
     * specifier.
     */
    const int el_size = ff_sws_pixel_type_size(op->type);
    const int el_count = FFMIN(el_size * block_size, 16) / el_size;

    SwsOpAArch64Impl ret = {
        .op = op->op,
        .use_vh = ((op->type == SWS_PIXEL_U16) && block_size == 16)
               || ((op->type == SWS_PIXEL_U32) && block_size == 8)
               || ((op->type == SWS_PIXEL_F32) && block_size == 8),
        .type_int = ff_sws_pixel_type_is_int(op->type),
        .t = el_count << 8 | el_size,
    };

    /* Most SwsOp work on fields described by next->comps.unused.
     * The few that don't will override this field later.
     */
    for (int i = 0; i < 4; i++) {
        if (!next->comps.unused[i])
            ret.work_mask |= (1 << (i << 2));
    }

    switch (op->op) {
    case SWS_OP_READ:
    case SWS_OP_WRITE:
        if (op->rw.frac)
            ret.rw = op->rw.frac;
        else if (op->rw.packed)
            ret.rw = 0x10;
        break;
    case SWS_OP_SWIZZLE:
        ret.work_mask = (op->swizzle.in[0] == 0 ? 0xf : op->swizzle.in[0])
                      | (op->swizzle.in[1] == 1 ? 0xf : op->swizzle.in[1]) << 4
                      | (op->swizzle.in[2] == 2 ? 0xf : op->swizzle.in[2]) << 8
                      | (op->swizzle.in[3] == 3 ? 0xf : op->swizzle.in[3]) << 12;
        break;
    case SWS_OP_UNPACK:
    case SWS_OP_PACK:
        ret.pattern = op->pack.pattern[0]
                    | op->pack.pattern[1] << 4
                    | op->pack.pattern[2] << 8
                    | op->pack.pattern[3] << 12;
        break;
    case SWS_OP_LSHIFT:
    case SWS_OP_RSHIFT:
        ret.shift = op->c.u;
        break;
    case SWS_OP_CLEAR:
        ret.work_mask = !!op->c.q4[0].den
                      | !!op->c.q4[1].den << 4
                      | !!op->c.q4[2].den << 8
                      | !!op->c.q4[3].den << 12;
        break;
    case SWS_OP_CONVERT:
        ret.convert.to_size = ff_sws_pixel_type_size(op->convert.to);
        ret.convert.to_int  = ff_sws_pixel_type_is_int(op->convert.to);
        ret.convert.expand  = op->convert.expand;
        break;
    case SWS_OP_LINEAR: {
        /* Check which vectors are used after this operation */
        bool used[4] = { false, false, false, false };
        LOOP_IN(i) used[i] = true;
        LOOP_ARRAY(i, used) {
            bool is_identity = true;
            for (int j = 0; j < 5; j++) {
                if (i == j) {
                    if (op->lin.m[i][j].num != 1 || op->lin.m[i][j].den != 1) {
                        is_identity = false;
                        break;
                    }
                } else {
                    if (op->lin.m[i][j].num != 0) {
                        is_identity = false;
                        break;
                    }
                }
            }
            if (is_identity)
                used[i] = false;
        }

        for (int i = 0; i < 4; i++) {
            if (!used[i])
                continue;
            for (int j = 0; j < 5; j++) {
                if (!av_cmp_q(op->lin.m[i][j], Q1))
                    ret.linear_mask |= 1ULL << (2 * ((5 * i) + j));
                else if (av_cmp_q(op->lin.m[i][j], Q0))
                    ret.linear_mask |= 3ULL << (2 * ((5 * i) + j));
            }
        }
        break;
    }
    }

    return ret;
}

/*********************************************************************/
/* Swap nibbles in uint16_t so that the printed value is in the correct order. */
static uint16_t nswap16(uint16_t v)
{
    return ((v & 0x000f) << 12) |
           ((v & 0x00f0) <<  4) |
           ((v & 0x0f00) >>  4) |
           ((v & 0xf000) >> 12);
}

/* Generate a 64-bit mask used to identify a NEON function. */
static uint64_t gen_func_mask(const SwsOpAArch64Impl *impl)
{
    uint64_t func_mask = 0;

    switch (impl->op) {
    case SWS_OP_READ:
    case SWS_OP_WRITE:
        func_mask |= (uint64_t) nswap16(impl->work_mask) << 36;
        func_mask |= (uint64_t) impl->t                  << 20;
        func_mask |= (uint64_t) impl->rw                 << 12;
        break;
    case SWS_OP_SWAP_BYTES:
        func_mask |= (uint64_t) nswap16(impl->work_mask) << 36;
        func_mask |= (uint64_t) impl->t                  << 20;
        break;
    case SWS_OP_SWIZZLE:
        func_mask |= (uint64_t) nswap16(impl->work_mask) << 36;
        break;
    case SWS_OP_UNPACK:
    case SWS_OP_PACK:
        func_mask |= (uint64_t) nswap16(impl->work_mask) << 36;
        func_mask |= (uint64_t) nswap16(impl->pattern)   << 20;
        break;
    case SWS_OP_LSHIFT:
    case SWS_OP_RSHIFT:
        func_mask |= (uint64_t) nswap16(impl->work_mask) << 36;
        func_mask |= (uint64_t) impl->shift              << 32;
        break;
    case SWS_OP_CLEAR:
        func_mask |= (uint64_t) nswap16(impl->work_mask) << 36;
        break;
    case SWS_OP_CONVERT:
        func_mask |= (uint64_t) nswap16(impl->work_mask) << 36;
        func_mask |= (uint64_t) impl->t                  << 20;
        func_mask |= (uint64_t) impl->type_int           << 16;
        func_mask |= (uint64_t) impl->convert.to_size    << 12;
        func_mask |= (uint64_t) impl->convert.to_int     <<  8;
        func_mask |= (uint64_t) impl->convert.expand     <<  4;
        break;
    case SWS_OP_MIN:
    case SWS_OP_MAX:
        func_mask |= (uint64_t) nswap16(impl->work_mask) << 36;
        func_mask |= (uint64_t) impl->type_int           << 20;
        break;
    case SWS_OP_SCALE:
        func_mask |= (uint64_t) nswap16(impl->work_mask) << 36;
        func_mask |= (uint64_t) impl->t                  << 20;
        func_mask |= (uint64_t) impl->type_int           << 16;
        break;
    case SWS_OP_LINEAR:
        func_mask |= impl->linear_mask;
        break;
    case SWS_OP_DITHER:
        func_mask |= (uint64_t) nswap16(impl->work_mask) << 36;
        func_mask |= (uint64_t) impl->t                  << 20;
        break;
    }
    func_mask |= (uint64_t) impl->use_vh << 52;
    func_mask |= (uint64_t) impl->op << 56;

    return func_mask;
}

/*********************************************************************/
static av_printf_format(3, 4) void buf_appendf(char **pbuf, size_t *prem, const char *fmt, ...)
{
    char *buf = *pbuf;
    size_t rem = *prem;
    if (!rem)
        return;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, rem, fmt, ap);
    va_end(ap);

    if (n > 0) {
        size_t written = (size_t) n;
        if (written < rem) {
            buf += written;
            rem -= written;
        } else {
            buf += rem - 1;
            rem = 0;
        }
        *pbuf = buf;
        *prem = rem;
    }
}

/* NOTE does not write terminating '\0' */
static void buf_appendc(char **pbuf, size_t *prem, char c)
{
    char *buf = *pbuf;
    size_t rem = *prem;
    if (!rem)
        return;

    *buf++ = c;

    *pbuf = buf;
    *prem = rem - 1;
}

/* Generate a function name */
static char *gen_func_name(const SwsOpAArch64Impl *impl)
{
    char name[256];
    char *p = name;
    size_t rem = sizeof(name);

    buf_appendf(&p, &rem, "ff_sws");

    switch (impl->op) {
    case SWS_OP_READ:
    case SWS_OP_WRITE:
        buf_appendf(&p, &rem, impl->op == SWS_OP_READ ? "_read" : "_write");
        if (!impl->rw) {
            buf_appendf(&p, &rem, "_planar");
        } else if (impl->rw == 0x10) {
            buf_appendf(&p, &rem, "_packed");
        } else {
            buf_appendf(&p, &rem, "_frac%d", impl->rw);
        }
        buf_appendf(&p, &rem, "_%04x", nswap16(impl->work_mask));
        buf_appendf(&p, &rem, "_%04x", impl->t);
        break;
    case SWS_OP_SWAP_BYTES:
        buf_appendf(&p, &rem, "_bswap");
        buf_appendf(&p, &rem, "_%04x", nswap16(impl->work_mask));
        buf_appendf(&p, &rem, "_%04x", impl->t);
        break;
    case SWS_OP_SWIZZLE:
        buf_appendf(&p, &rem, "_swizzle");
        buf_appendf(&p, &rem, "_%04x", nswap16(impl->work_mask));
        break;
    case SWS_OP_UNPACK:
    case SWS_OP_PACK:
        buf_appendf(&p, &rem, impl->op == SWS_OP_UNPACK ? "_unpack" : "_pack");
        buf_appendf(&p, &rem, "_%04x", nswap16(impl->work_mask));
        buf_appendf(&p, &rem, "_%04x", nswap16(impl->pattern));
        break;
    case SWS_OP_LSHIFT:
    case SWS_OP_RSHIFT:
        buf_appendf(&p, &rem, impl->op == SWS_OP_LSHIFT ? "_lshift" : "_rshift");
        buf_appendf(&p, &rem, "_%04x", nswap16(impl->work_mask));
        buf_appendf(&p, &rem, "_%02x", impl->shift);
        break;
    case SWS_OP_CLEAR:
        buf_appendf(&p, &rem, "_clear");
        buf_appendf(&p, &rem, "_%04x", nswap16(impl->work_mask));
        break;
    case SWS_OP_CONVERT:
        buf_appendf(&p, &rem, "_convert");
        buf_appendf(&p, &rem, "_%04x", nswap16(impl->work_mask));
        buf_appendf(&p, &rem, "_%04x", impl->t);
        buf_appendf(&p, &rem, "_%d", impl->type_int);
        buf_appendf(&p, &rem, "_%d", impl->convert.to_size);
        buf_appendf(&p, &rem, "_%d", impl->convert.to_int);
        buf_appendf(&p, &rem, "_%d", impl->convert.expand);
        break;
    case SWS_OP_MIN:
    case SWS_OP_MAX:
        buf_appendf(&p, &rem, impl->op == SWS_OP_MIN ? "_min" : "_max");
        buf_appendf(&p, &rem, "_%04x", nswap16(impl->work_mask)); // TODO check
        buf_appendf(&p, &rem, "_%d", impl->type_int);
        break;
    case SWS_OP_SCALE:
        buf_appendf(&p, &rem, "_scale");
        buf_appendf(&p, &rem, "_%04x", nswap16(impl->work_mask));
        buf_appendf(&p, &rem, "_%04x", impl->t);
        buf_appendf(&p, &rem, "_%d", impl->type_int);
        break;
    case SWS_OP_LINEAR:
        buf_appendf(&p, &rem, "_linear");
        buf_appendf(&p, &rem, "_%010"PRIx64"", impl->linear_mask);
        break;
    case SWS_OP_DITHER:
        buf_appendf(&p, &rem, "_dither");
        buf_appendf(&p, &rem, "_%04x", nswap16(impl->work_mask));
        buf_appendf(&p, &rem, "_%04x", impl->t);
        break;
    }
    buf_appendf(&p, &rem, "_%d", (impl->use_vh ? 2 : 1));
    buf_appendf(&p, &rem, "_neon");
    buf_appendc(&p, &rem, '\0');
    av_assert0(rem);

    return av_strdup(name);
}

/*********************************************************************/
static int aarch64_gen_sig(char **priv, const SwsOpList *ops, int n, int block_size)
{
    SwsOpAArch64Impl impl = sws_op_aarch64_impl(ops, block_size, n);
    uint64_t func_mask = gen_func_mask(&impl);
    char *func_name = gen_func_name(&impl);

    if (1) {
        /* generate c switch exact get_func */
        priv[n] = av_asprintf("    case 0x%016" PRIx64 "ULL: return %s;", func_mask, func_name);
    } else {
        /* generate c declaration */
        priv[n] = av_asprintf("void %s(void);", func_name);
    }
    if (!priv[n])
        return AVERROR(ENOMEM);

    return 0;
}

/*********************************************************************/
static void sig_free_cb(void *ptr)
{
    if (!ptr)
        return;

    char **priv = ptr;
    while (*priv)
        av_free(*priv++);
    av_free(ptr);
}

static int aarch64_gen_sig_compile(SwsContext *ctx, SwsOpList *ops, SwsCompiledOp *out,
                                   int flags)
{
    int ret;

    /* Use at most two full vregs during the widest precision section */
    int block_size = (ff_sws_op_list_max_size(ops) == 4) ? 8 : 16;

    char **priv = av_mallocz((ops->num_ops + 2) * sizeof(char *));
    if (!priv)
        return AVERROR(ENOMEM);

    *out = (SwsCompiledOp) {
        .priv = priv,
        .slice_align = 1,
        .free = sig_free_cb,
    };

    const SwsOp *read  = ff_sws_op_list_input(ops);
    const SwsOp *write = ff_sws_op_list_output(ops);
    const int read_planes  = read ? (read->rw.packed ? 1 : read->rw.elems) : 0;
    const int write_planes = write->rw.packed ? 1 : write->rw.elems;

    for (int i = 0; i < ops->num_ops; i++) {
        ret = aarch64_gen_sig(priv, ops, i, block_size);
        if (ret < 0)
            goto end;
    }

    priv[ops->num_ops] = av_asprintf("ff_sws_process_%u_%u", read_planes, write_planes);
    if (ret < 0)
        goto end;

    return 0;

end:
    sig_free_cb(priv);
    return ret;
}

const SwsOpBackend backend_aarch64 = {
    .name       = "aarch64_gen_sig",
    .compile    = aarch64_gen_sig_compile,
    .hw_format  = AV_PIX_FMT_NONE,
};
