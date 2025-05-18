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

#include "libavutil/avassert.h"
#include "libavutil/bswap.h"
#include "libavutil/intfloat.h"

#include "memops.h"

#include "ops_backend.h"

typedef struct MemcpyPriv {
    int num_planes;
    int index[4]; /* or -fmt_size to clear plane */
    int bswap;
    int lshift;
    uint32_t clear_value[4];
    MemOpsContext mctx;
} MemcpyPriv;

/* Memcpy backend for trivial cases */

#define av_q2f(q) ((q).den ? (float) (q).num / (q).den : 0)

static void process(const SwsOpExec *exec, const void *priv,
                    int x_start, int y_start, int x_end, int y_end)
{
    const MemcpyPriv *p = priv;
    const int lines = y_end - y_start;
    av_assert1(x_start == 0 && x_end == exec->width);

    for (int i = 0; i < p->num_planes; i++) {
        uint8_t *out = exec->out[i];
        const int idx = p->index[i];
        if (idx == -1) {
            memset(out, p->clear_value[i], exec->out_stride[i] * lines);
        } else if (idx == -2) {
            p->mctx.memset16((uint16_t *) out, p->clear_value[i], (exec->out_stride[i] * lines) >> 1);
        } else if (idx == -4) {
            p->mctx.memset32((uint32_t *) out, p->clear_value[i], (exec->out_stride[i] * lines) >> 2);
        } else if (exec->out_stride[i] == exec->in_stride[idx]) {
            if (p->lshift) {
                p->mctx.lshift16((uint16_t *) out, (uint16_t *) exec->in[idx], (exec->out_stride[i] * lines) >> 1, p->lshift);
            } else if (p->bswap == 2) {
                p->mctx.bswap16((uint16_t *) out, (uint16_t *) exec->in[idx], (exec->out_stride[i] * lines) >> 1);
            } else if (p->bswap == 4) {
                p->mctx.bswap32((uint32_t *) out, (uint32_t *) exec->in[idx], (exec->out_stride[i] * lines) >> 2);
            } else {
                memcpy(out, exec->in[idx], exec->out_stride[i] * lines);
            }
        } else {
            const int bytes = x_end * exec->block_size_out;
            const uint8_t *in = exec->in[idx];
            for (int y = y_start; y < y_end; y++) {
                if (p->lshift) {
                    p->mctx.lshift16((uint16_t *) out, (uint16_t *) in, bytes >> 1, p->lshift);
                } else if (p->bswap == 2) {
                    p->mctx.bswap16((uint16_t *) out, (uint16_t *) in, bytes >> 1);
                } else if (p->bswap == 4) {
                    p->mctx.bswap32((uint32_t *) out, (uint32_t *) in, bytes >> 2);
                } else {
                    memcpy(out, in, bytes);
                }
                out += exec->out_stride[i];
                in  += exec->in_stride[idx];
            }
        }
    }
}

static int compile(SwsContext *ctx, SwsOpList *ops, SwsCompiledOp *out)
{
    MemcpyPriv p = {0};

    for (int n = 0; n < ops->num_ops; n++) {
        const SwsOp *op = &ops->ops[n];
        switch (op->op) {
        case SWS_OP_READ:
            if ((op->rw.packed && op->rw.elems != 1) || op->rw.frac || op->rw.filter)
                return AVERROR(ENOTSUP);
            for (int i = 0; i < op->rw.elems; i++)
                p.index[i] = i;
            break;

        case SWS_OP_LSHIFT:
            if (p.bswap)
                return AVERROR(ENOTSUP);
            p.lshift = op->shift.amount;
            break;

        case SWS_OP_SWAP_BYTES: {
            if (p.lshift)
                return AVERROR(ENOTSUP);
            int fmt_size = ff_sws_pixel_type_size(op->type);
            if (p.bswap == 0) {
                p.bswap = fmt_size;
            } else if (p.bswap == fmt_size) {
                p.bswap = 0;
            } else {
                return AVERROR(ENOTSUP);
            }
            for (int i = 0; i < 4; i++) {
                switch (fmt_size) {
                case 2: p.clear_value[i] = av_bswap16(p.clear_value[i]); break;
                case 4: p.clear_value[i] = av_bswap32(p.clear_value[i]); break;
                }
            }
            break;
        }

        case SWS_OP_SWIZZLE: {
            const MemcpyPriv orig = p;
            for (int i = 0; i < 4; i++) {
                /* Explicitly exclude swizzle masks that contain duplicates,
                 * because these are wasteful to implement as a memcpy */
                for (int j = 0; j < i; j++) {
                    if (op->swizzle.in[i] == op->swizzle.in[j])
                        return AVERROR(ENOTSUP);
                }
                p.index[i] = orig.index[op->swizzle.in[i]];
            }
            break;
        }

        case SWS_OP_CLEAR:
            for (int i = 0; i < 4; i++) {
                if (!op->clear.value[i].den)
                    continue;

                int fmt_size = ff_sws_pixel_type_size(op->type);
                if (op->type == SWS_PIXEL_F32) {
                    float val = av_q2f(op->clear.value[i]);
                    p.clear_value[i] = av_float2int(val);
                } else {
                    if (op->clear.value[i].den != 1)
                        return AVERROR(ENOTSUP);
                    uint32_t val = op->clear.value[i].num;
                    uint32_t ref = val & 0xFF;
                    switch (fmt_size) {
                    case 2: ref *= 0x0101; break;
                    case 4: ref *= 0x01010101; break;
                    }
                    if (ref == val)
                        fmt_size = 1;
                    p.clear_value[i] = val;
                }
                p.index[i] = -fmt_size;
            }
            break;

        case SWS_OP_WRITE:
            if ((op->rw.packed && op->rw.elems != 1) || op->rw.frac || op->rw.filter)
                return AVERROR(ENOTSUP);
            p.num_planes = op->rw.elems;
            break;

        default:
            return AVERROR(ENOTSUP);
        }
    }

    ff_memops_init(&p.mctx);

    *out = (SwsCompiledOp) {
        .slice_align = 1,
        .block_size  = 1,
        .func = process,
        .priv = av_memdup(&p, sizeof(p)),
        .free = av_free,
    };
    return out->priv ? 0 : AVERROR(ENOMEM);
}

const SwsOpBackend backend_murder = {
    .name       = "memcpy",
    .compile    = compile,
    .hw_format  = AV_PIX_FMT_NONE,
};
