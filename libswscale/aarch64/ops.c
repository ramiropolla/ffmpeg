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

static int aarch64_gen_sig(char **priv, const SwsOpList *ops, int block_size, int n)
{
    const SwsOp *op = &ops->ops[n];
    const SwsOp *next = n + 1 < ops->num_ops ? &ops->ops[n + 1] : op;

    bool use_vh = ((op->type == SWS_PIXEL_U16) && block_size == 16)
               || ((op->type == SWS_PIXEL_U32) && block_size == 8)
               || ((op->type == SWS_PIXEL_F32) && block_size == 8);
    int el_size = ff_sws_pixel_type_size(op->type);
    int el_count = FFMIN(el_size * block_size, 16) / el_size;
    uint16_t t = el_count << 8 | el_size;

    char name[256];
    char *p = name;
    size_t rem = sizeof(name);

    buf_appendf(&p, &rem, "ff_sws");

    uint16_t work_mask = 0;
    for (int i = 0; i < 4; i++) {
        if (!next->comps.unused[i])
            work_mask |= (1 << ((3 - i) * 4));
    }

    uint64_t func_mask = 0;

    switch (op->op) {
    case SWS_OP_READ:
    case SWS_OP_WRITE:
        buf_appendf(&p, &rem, op->op == SWS_OP_READ ? "_read" : "_write");
        if (op->rw.frac) {
            buf_appendf(&p, &rem, "_frac%d", op->rw.frac);
            func_mask = op->rw.frac;
        } else if (op->rw.packed) {
            buf_appendf(&p, &rem, "_packed");
            func_mask = 0x10;
        } else {
            buf_appendf(&p, &rem, "_planar");
            func_mask = 0x20;
        }
        buf_appendf(&p, &rem, "_%04x", work_mask);
        buf_appendf(&p, &rem, "_%04x", t);
        func_mask |= ((uint64_t) work_mask <<  8)
                   | ((uint64_t) t         << 24);
        break;
    case SWS_OP_SWAP_BYTES:
        buf_appendf(&p, &rem, "_bswap");
        buf_appendf(&p, &rem, "_%04x", work_mask);
        buf_appendf(&p, &rem, "_%04x", t);
        func_mask |= ((uint64_t) work_mask <<  8)
                   | ((uint64_t) t         << 24);
        break;
    case SWS_OP_SWIZZLE:
        buf_appendf(&p, &rem, "_swizzle");
        buf_appendf(&p, &rem, "_%c%c%c%c",
                    op->swizzle.in[0] == 0 ? 'f' : op->swizzle.in[0] + '0',
                    op->swizzle.in[1] == 1 ? 'f' : op->swizzle.in[1] + '0',
                    op->swizzle.in[2] == 2 ? 'f' : op->swizzle.in[2] + '0',
                    op->swizzle.in[3] == 3 ? 'f' : op->swizzle.in[3] + '0');
        break;
    case SWS_OP_UNPACK:
    case SWS_OP_PACK:
        buf_appendf(&p, &rem, op->op == SWS_OP_UNPACK ? "_unpack" : "_pack");
        buf_appendf(&p, &rem, "_%04x", work_mask);
        buf_appendf(&p, &rem, "_%x%x%x%x",
                    op->pack.pattern[0],
                    op->pack.pattern[1],
                    op->pack.pattern[2],
                    op->pack.pattern[3]);
        break;
    case SWS_OP_LSHIFT:
    case SWS_OP_RSHIFT:
        buf_appendf(&p, &rem, op->op == SWS_OP_LSHIFT ? "_lshift" : "_rshift");
        buf_appendf(&p, &rem, "_%04x", work_mask);
        buf_appendf(&p, &rem, "_%02x", op->c.u);
        break;
    case SWS_OP_CLEAR:
        buf_appendf(&p, &rem, "_clear");
        buf_appendf(&p, &rem, "_%d%d%d%d",
                    !!op->c.q4[0].den,
                    !!op->c.q4[1].den,
                    !!op->c.q4[2].den,
                    !!op->c.q4[3].den);
        break;
    case SWS_OP_CONVERT:
        buf_appendf(&p, &rem, "_convert");
        buf_appendf(&p, &rem, "_%04x", work_mask);
        buf_appendf(&p, &rem, "_%04x", t);
        buf_appendf(&p, &rem, "_%d", ff_sws_pixel_type_size(op->convert.to));
        buf_appendf(&p, &rem, "_%d", ff_sws_pixel_type_is_int(op->type));
        buf_appendf(&p, &rem, "_%d", ff_sws_pixel_type_is_int(op->convert.to));
        buf_appendf(&p, &rem, "_%d", op->convert.expand);
        break;
    case SWS_OP_MIN:
    case SWS_OP_MAX:
        buf_appendf(&p, &rem, op->op == SWS_OP_MIN ? "_min" : "_max");
        buf_appendf(&p, &rem, "_%04x", work_mask); // TODO check
        buf_appendf(&p, &rem, "_%d", ff_sws_pixel_type_is_int(op->type));
        break;
    case SWS_OP_SCALE:
        buf_appendf(&p, &rem, "_scale");
        buf_appendf(&p, &rem, "_%04x", work_mask);
        buf_appendf(&p, &rem, "_%04x", t);
        buf_appendf(&p, &rem, "_%d", ff_sws_pixel_type_is_int(op->type));
        break;
    case SWS_OP_LINEAR: {
        buf_appendf(&p, &rem, "_linear");
        uint64_t linear_mask = 0;

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
                    linear_mask |= 1ULL << (2 * ((5 * i) + j));
                else if (av_cmp_q(op->lin.m[i][j], Q0))
                    linear_mask |= 3ULL << (2 * ((5 * i) + j));
            }
        }
        buf_appendf(&p, &rem, "_%010"PRIx64"", linear_mask);
        break;
    }
    case SWS_OP_DITHER:
        buf_appendf(&p, &rem, "_dither");
        buf_appendf(&p, &rem, "_%04x", work_mask);
        buf_appendf(&p, &rem, "_%04x", t);
        buf_appendf(&p, &rem, "_%d", ff_sws_pixel_type_is_int(op->type));
        break;
    }
    buf_appendf(&p, &rem, "_%d", (use_vh ? 2 : 1));
    buf_appendf(&p, &rem, "_neon");
    buf_appendc(&p, &rem, '\0');
    av_assert0(rem);

    func_mask |= (uint64_t) op->op  << 56;
    priv[n] = av_asprintf("%s 0x%016" PRIx64, name, func_mask);
    if (!priv[n])
        return AVERROR(ENOMEM);

    return 0;
}

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
        ret = aarch64_gen_sig(priv, ops, block_size, i);
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
