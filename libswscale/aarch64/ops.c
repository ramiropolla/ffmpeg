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

#include "libavutil/avstring.h"

typedef struct FFAsmAArch64Context {
};

enum FFAsmAArch64Insn {
    FFASM_AARCH64_NONE = 0,
    FFASM_AARCH64_LDR,
};

#define ffasm_aarch64_ldr(ctx, Rt) ffasm_add_insn(ctx, FFASM_AARCH64_LDR, Rt, b, c, 0)

typedef struct AArch64Context {
    int block_size;
} AArch64Context;

#define LOOP_ARRAY(idx, arr)          \
    for (int idx = 0; idx < 4; idx++) \
        if (arr[idx])
#define LOOP_OUT(idx) LOOP_ARRAY(idx, !next->comps.unused)
#define LOOP_IN(idx)  LOOP_ARRAY(idx, !op->comps.unused)
#define LOOP_OUT_VH(idx) LOOP_ARRAY(idx, use_vh && !next->comps.unused)
#define LOOP_IN_VH(idx)  LOOP_ARRAY(idx, use_vh && !op->comps.unused)

#if 0
/* Vector element type for given SwsOp */
typedef struct VectorElementType {
    VectorElementType(const SwsOp *op, int block_size)
    {
        e = (op->type == SWS_PIXEL_U8)  ? 1
          : (op->type == SWS_PIXEL_U16) ? 2
          :                               4;
        int full_size = e * block_size;
        size = FFMIN(full_size, 16);
    }

    a64::Vec type(const a64::Vec &vreg) const
    {
        switch ((e << 8) | size) {
        case 0x0110: return vreg.b16();
        case 0x0108: return vreg.b8();
        case 0x0210: return vreg.h8();
        case 0x0410: return vreg.s4();
        }
        printf("ERORROORORRORORO %d %d\n", e, size);
        return vreg.b16();
    }

    a64::Vec type_gpr(const a64::Vec &vreg) const
    {
        switch ((e << 8) | size) {
        case 0x0110: return vreg.q();
        case 0x0108: return vreg.d();
        case 0x0210: return vreg.q();
        case 0x0410: return vreg.q();
        }
        printf("ERORROORORRORORO gpr %d %d\n", e, size);
        return vreg.q();
    }

    uint8_t e;
    uint8_t size;
} VectorElementType;
#else
typedef struct VectorElementType {
    uint8_t e; /* element size */
    uint8_t v; /* vector size */
} VectorElementType;
static void vet_init(VectorElementType *vet, const SwsOp *op, int block_size)
{
    vet->e = ff_sws_pixel_type_size(op->type);
    vet->v = FFMIN(vet->e * block_size, 16);
}
#endif

const char *used_mask(const bool unused[4], char buf[5]);
const char *used_mask(const bool unused[4], char buf[5])
{
    for (int i = 0; i < 4; i++)
        buf[i] = unused[i] ? '0' : '1';
    return buf;
}
#define USED_MASK(op) used_mask(next->comps.unused, (char[8]){0})

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

static int aarch64_sig_gen(AArch64Context *actx, const SwsOpList *ops, int n)
{
    int block_size = actx->block_size;

    const SwsOp *op = &ops->ops[n];
    const SwsOp *next = n + 1 < ops->num_ops ? &ops->ops[n + 1] : op;

    bool use_vh = ((op->type == SWS_PIXEL_U16) && block_size == 16)
               || ((op->type == SWS_PIXEL_U32) && block_size == 8)
               || ((op->type == SWS_PIXEL_F32) && block_size == 8);

    VectorElementType vet;
    vet_init(&vet, op, block_size);
    // int num_bytes = vet.v << (use_vh ? 1 : 0); // TODO check if we can use num_bytes instead of (use_vh ? 0x100 : 0) | vet.v

    char sig[128];
    char *p = sig;
    size_t rem = sizeof(sig);

    const char *op_type_name = ff_sws_op_type_name(op->op);
    size_t op_type_name_len = strlen(op_type_name);
    for (size_t i = 0; i < op_type_name_len; i++)
        buf_appendc(&p, &rem, av_tolower(op_type_name[i]));

    buf_appendf(&p, &rem, "_%c", ff_sws_pixel_type_is_int(op->type) ? 'i' : 'f');
    buf_appendf(&p, &rem, "_%s", USED_MASK(next));
    buf_appendf(&p, &rem, "_%04x", ((use_vh ? 2 : 1) << 12) | (vet.v << 4) | vet.e);

    switch (op->op) {
    case SWS_OP_READ:
    case SWS_OP_WRITE:
        if (op->rw.frac) {
            buf_appendf(&p, &rem, "_frac_%d", op->rw.frac);
        } else if (op->rw.packed) {
            buf_appendf(&p, &rem, "_packed");
        } else {
            buf_appendf(&p, &rem, "_planar");
        }
        break;
    case SWS_OP_SWAP_BYTES:
        break;
    case SWS_OP_SWIZZLE:
        break;
    case SWS_OP_UNPACK:
    case SWS_OP_PACK:
        buf_appendf(&p, &rem, "_%08x",
                    MKBETAG(op->pack.pattern[0],
                            op->pack.pattern[1],
                            op->pack.pattern[2],
                            op->pack.pattern[3]));
        break;
    case SWS_OP_LSHIFT:
    case SWS_OP_RSHIFT:
        buf_appendf(&p, &rem, "_%02x", op->c.u);
        break;
    case SWS_OP_CLEAR:
        break;
    case SWS_OP_CONVERT:
        buf_appendf(&p, &rem, "_%c_%02x_%d",
                    ff_sws_pixel_type_is_int(op->convert.to) ? 'i' : 'f',
                    ff_sws_pixel_type_size(op->convert.to),
                    op->convert.expand);
        break;
    case SWS_OP_MIN:
        break;
    case SWS_OP_MAX:
        break;
    case SWS_OP_SCALE:
        break;
    case SWS_OP_LINEAR:
        {
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

            buf_appendc(&p, &rem, '_');
            for (int i = 0; i < 4; i++) {
                if (!used[i]) {
                    buf_appendf(&p, &rem, "00000");
                    continue;
                }
                for (int j = 0; j < 5; j++) {
                    if (!av_cmp_q(op->lin.m[i][j], Q0)) {
                        buf_appendc(&p, &rem, '0');
                    } else if (!av_cmp_q(op->lin.m[i][j], Q1)) {
                        buf_appendc(&p, &rem, '1');
                    } else {
                        buf_appendc(&p, &rem, 'x');
                    }
                }
            }
        }
        break;
    case SWS_OP_DITHER:
        break;
    }
    buf_appendc(&p, &rem, '\0');
    if (rem == 0) {
        fprintf(stderr, "BUFFER TOO SMALL %zu\n", sizeof(sig));
    } else {
        printf("%s\n", sig);
    }

    return 0;
}

static int aarch64_compile(SwsContext *ctx, SwsOpList *ops, SwsCompiledOp *out,
                           int flags)
{
    AArch64Context actx;

    /* Use at most two full vregs during the widest precision section */
    actx.block_size = (ff_sws_op_list_max_size(ops) == 4) ? 8 : 16;

    // printf("num_ops %d\n", ops->num_ops);
    if (flags & SWS_OP_FLAG_SIG_GEN) {
        for (int n = 0; n < ops->num_ops; n++)
            aarch64_sig_gen(&actx, ops, n);
   }

    return AVERROR(ENOTSUP);

#if 0
    const int cpu_flags = av_get_cpu_flags();
    const int mmsize = get_mmsize(cpu_flags);
    if (mmsize < 0)
        return mmsize;

    const SwsOp *read  = ff_sws_op_list_input(ops);
    const SwsOp *write = ff_sws_op_list_output(ops);
    av_assert1(write);
    int ret;

    /* Special fast path for in-place packed shuffle */
    ret = solve_shuffle(ops, mmsize, out);
    if (ret != AVERROR(ENOTSUP))
        return ret;

    SwsOpChain *chain = ff_sws_op_chain_alloc();
    if (!chain)
        return AVERROR(ENOMEM);

    *out = (SwsCompiledOp) {
        .priv = chain,
        .slice_align = 1,
        .free = ff_sws_op_chain_free_cb,

        /* Use at most two full YMM regs during the widest precision section */
        .block_size = 2 * FFMIN(mmsize, 32) / ff_sws_op_list_max_size(ops),
    };

    /* 3-component reads/writes process one extra garbage word */
    if (read && read->rw.packed && read->rw.elems == 3)
        out->over_read = sizeof(uint32_t);
    if (write->rw.packed && write->rw.elems == 3)
        out->over_write = sizeof(uint32_t);


    /* Make on-stack copy of `ops` to iterate over */
    SwsOpList rest = *ops;
    do {
        int op_block_size = out->block_size;
        SwsOp *op = &rest.ops[0];

        if (op_is_type_invariant(op)) {
            if (op->op == SWS_OP_CLEAR)
                normalize_clear(op);
            op_block_size *= ff_sws_pixel_type_size(op->type);
            op->type = SWS_PIXEL_U8;
        }

        ret = ff_sws_op_compile_tables(tables, FF_ARRAY_ELEMS(tables), &rest,
                                       op_block_size, chain);
    } while (ret == AVERROR(EAGAIN));

    if (ret < 0) {
        ff_sws_op_chain_free(chain);
        if (rest.num_ops < ops->num_ops) {
            av_log(ctx, AV_LOG_TRACE, "Uncompiled remainder:\n");
            ff_sws_op_list_print(ctx, AV_LOG_TRACE, AV_LOG_TRACE, &rest);
        }
        return ret;
    }

#define ASSIGN_PROCESS_FUNC(NAME)                               \
    do {                                                        \
        SWS_DECL_FUNC(NAME);                                    \
        void NAME##_return(void);                               \
        ret = ff_sws_op_chain_append(chain, NAME##_return,      \
                                     NULL, &(SwsOpPriv) {0});   \
        out->func = NAME;                                       \
    } while (0)

    const int read_planes  = read ? (read->rw.packed ? 1 : read->rw.elems) : 0;
    const int write_planes = write->rw.packed ? 1 : write->rw.elems;
    switch (FFMAX(read_planes, write_planes)) {
    case 1: ASSIGN_PROCESS_FUNC(ff_sws_process1_x86); break;
    case 2: ASSIGN_PROCESS_FUNC(ff_sws_process2_x86); break;
    case 3: ASSIGN_PROCESS_FUNC(ff_sws_process3_x86); break;
    case 4: ASSIGN_PROCESS_FUNC(ff_sws_process4_x86); break;
    }

    if (ret < 0) {
        ff_sws_op_chain_free(chain);
        return ret;
    }

    out->cpu_flags = chain->cpu_flags;

    return 0;
#endif
}

const SwsOpBackend backend_aarch64 = {
    .name       = "aarch64",
    .compile    = aarch64_compile,
    /* TODO call compile(flag_sig_gen) or add sig_gen() function? */
    .hw_format  = AV_PIX_FMT_NONE,
};
