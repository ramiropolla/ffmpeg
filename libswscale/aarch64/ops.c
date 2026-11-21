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

#include "config.h"

#if defined(__linux__) && defined(__GLIBC__)
#include <features.h>
#if __GLIBC_PREREQ(2, 35)
#include <sys/rseq.h>
#define HAVE_RSEQ_ABI 1
#endif
#endif
#ifndef HAVE_RSEQ_ABI
#define HAVE_RSEQ_ABI 0
#endif

#include "../ops_chain.h"

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/tree.h"
#include "libavutil/thread.h"
#include "libavutil/aarch64/cpu.h"

#include "ops_impl_conv.c"

/**
 * Check that there is no mismatch for the SwsOpExec/SwsOpImpl offset
 * values used by ops_static.
 * NOTE: The check is performed here since this file only ever targets
 *       aarch64, differently from ops_static which may be built on any
 *       host.
 */
static_assert(offsetof_exec_in       == offsetof(SwsOpExec, in),       "SwsOpExec layout mismatch");
static_assert(offsetof_exec_out      == offsetof(SwsOpExec, out),      "SwsOpExec layout mismatch");
static_assert(offsetof_exec_in_bump  == offsetof(SwsOpExec, in_bump),  "SwsOpExec layout mismatch");
static_assert(offsetof_exec_out_bump == offsetof(SwsOpExec, out_bump), "SwsOpExec layout mismatch");
static_assert(offsetof_impl_func     == offsetof(SwsOpImpl, func),     "SwsOpImpl layout mismatch");
static_assert(offsetof_impl_priv     == offsetof(SwsOpImpl, priv),     "SwsOpImpl layout mismatch");
static_assert(alignof_impl           == alignof(SwsOpImpl),            "SwsOpImpl layout mismatch");
static_assert(sizeof_dyn_header % alignof_impl == 0,                     "dyn header alignment mismatch");

/*********************************************************************/
/* Forward-declare exported functions. */
#define ENTRY(fname, ...) extern void fname(void);
#include "ops_entries.c"
#undef ENTRY

static const struct {
    void (*func)(void);
    SwsAArch64OpImplParams params;
} ops_entries[] = {
#define ENTRY(fname, ...) { .func = fname, .params = __VA_ARGS__ },
#include "ops_entries.c"
#undef ENTRY
};

/* Look up the exported function pointer for the given parameters. */
static SwsFuncPtr aarch64_lookup(const SwsAArch64OpImplParams *p)
{
    for (int i = 0; i < FF_ARRAY_ELEMS(ops_entries); i++)
        if (!memcmp(p, &ops_entries[i].params, sizeof(SwsAArch64OpImplParams)))
            return ops_entries[i].func;
    return NULL;
}

/*********************************************************************/
static int aarch64_setup_linear(const SwsAArch64OpImplParams *p,
                                const SwsUOp *uop, SwsImplResult *res)
{
    /**
     * Compute number of full vector registers needed to pack all non-zero
     * coefficients.
     */
    const int num_vregs = linear_num_vregs(p);
    av_assert0(num_vregs <= 4);
    union {
        uint8_t  u8[4 * 16];
        uint16_t u16[4 * 8];
        uint32_t u32[4 * 4];
        float    f32[4 * 4];
    } coeffs = { 0 };

    /**
     * Copy non-zero coefficients, packed in sequential order, offset first.
     * The same order must be followed in asmgen_op_linear().
     */
    int i_coeff = 0;
    LOOP_MASK(p, i) {
        for (int j = 0; j < 5; j++) {
            const int jj = (j == 0) ? 4 : (j - 1);
            if (!(p->par.lin.zero & SWS_MASK(i, jj))) {
                const SwsPixel px = uop->data.mat4x5[i][jj];
                switch (p->type) {
                case SWS_PIXEL_U8:  coeffs.u8[i_coeff++]  = px.u8;  break;
                case SWS_PIXEL_U16: coeffs.u16[i_coeff++] = px.u16; break;
                case SWS_PIXEL_U32: coeffs.u32[i_coeff++] = px.u32; break;
                case SWS_PIXEL_F32: coeffs.f32[i_coeff++] = px.f32; break;
                }
            }
        }
    }

    ff_sws_op_priv_pack(res, &coeffs, num_vregs * 16);

    /* Pre-broadcast offsets, loaded directly into the output registers. */
    const int size = ff_sws_pixel_type_size(p->type);
    LOOP_MASK(p, i) {
        if (p->par.lin.zero & SWS_MASK_OFF(i))
            continue;
        uint8_t vec[16];
        for (int j = 0; j < sizeof(vec); j += size)
            memcpy(&vec[j], &uop->data.mat4x5[i][4], size);
        ff_sws_op_priv_pack(res, vec, sizeof(vec));
    }
    return 0;
}

/*********************************************************************/
static int aarch64_setup_dither(const SwsUOp *uop, SwsImplResult *res)
{
    ff_sws_op_priv_pack_refptr(res, av_refstruct_ref(uop->data.ptr));
    return 0;
}

/*********************************************************************/
static int aarch64_setup(const SwsUOp *uop, const SwsAArch64OpImplParams *p,
                         SwsImplResult *out)
{
    switch (uop->uop) {
    case SWS_UOP_READ_BIT: {
        /* Negative shift values to perform right shift using ushl. */
        static const int8_t shift[16] = {
            -7, -6, -5, -4, -3, -2, -1, 0,
            -7, -6, -5, -4, -3, -2, -1, 0,
        };
        SWS_OP_PRIV_PACK(out, shift);
        break;
    }
    case SWS_UOP_WRITE_BIT: {
        /* Shift values for ushl. */
        static const uint8_t shift[16] = {
            7, 6, 5, 4, 3, 2, 1, 0,
            7, 6, 5, 4, 3, 2, 1, 0,
        };
        SWS_OP_PRIV_PACK(out, shift);
        break;
    }
    case SWS_UOP_CLEAR:
    case SWS_UOP_ADD:
    case SWS_UOP_MIN:
    case SWS_UOP_MAX: {
        /**
         * Store each constant pre-broadcast to a full vector register, so
         * that the kernel can load it directly instead of loading and
         * broadcasting it on every block.
         */
        const int size = ff_sws_pixel_type_size(uop->type);
        LOOP(const_vec_mask(p), i) {
            uint8_t vec[16];
            for (int j = 0; j < sizeof(vec); j += size)
                memcpy(&vec[j], &uop->data.vec4[i], size);
            ff_sws_op_priv_pack(out, vec, sizeof(vec));
        }
        return 0;
    }
    case SWS_UOP_SCALE: {
        /* Store the scalar pre-broadcast to a full vector register. */
        const int size = ff_sws_pixel_type_size(uop->type);
        uint8_t vec[16];
        for (int j = 0; j < sizeof(vec); j += size)
            memcpy(&vec[j], &uop->data.scalar, size);
        ff_sws_op_priv_pack(out, vec, sizeof(vec));
        return 0;
    }
    case SWS_UOP_LINEAR:
    case SWS_UOP_LINEAR_FMA:
        return aarch64_setup_linear(p, uop, out);
    case SWS_UOP_TO_U32:
    case SWS_UOP_TO_F32:
        if (convert_u8_tbl(p)) {
            /* Indices placing each byte into the low byte of a 32-bit
             * element, 4 bytes per vector; out-of-range indices produce
             * zeros. */
            uint8_t idx[64];
            memset(idx, 0xff, sizeof(idx));
            for (int i = 0; i < p->block_size; i++)
                idx[i * 4] = i;
            ff_sws_op_priv_pack(out, idx, 4 * p->block_size);
        }
        return 0;
    case SWS_UOP_DITHER:
        return aarch64_setup_dither(uop, out);
    }
    return 0;
}

/*********************************************************************/
/**
 * Choosing between the kernels for in-order and out-of-order cores on each
 * call (see the dyn header in ops_impl.h). This needs the number of the
 * current CPU, which glibc keeps up to date in the rseq area of each thread
 * that it has registered with the kernel, and a system with both kinds of
 * cores.
 */
typedef struct AArch64DynInfo {
    bool     enabled;
    int64_t  rseq_cpu_id;
    uint64_t inorder_mask;
} AArch64DynInfo;

static AArch64DynInfo dyn_info;
static AVOnce dyn_once = AV_ONCE_INIT;

static av_cold void dyn_init(void)
{
#if HAVE_RSEQ_ABI
    /* __rseq_size is 0 if glibc didn't register the rseq area. */
    if (__rseq_size < offsetof(struct rseq, cpu_id) + sizeof(uint32_t))
        return;
    uint64_t inorder, ooo;
    if (avpriv_aarch64_get_core_kinds(&inorder, &ooo) < 0 || !inorder || !ooo)
        return;
    dyn_info.rseq_cpu_id  = __rseq_offset + offsetof(struct rseq, cpu_id);
    dyn_info.inorder_mask = inorder;
    dyn_info.enabled      = true;
#endif
}

/* Append the kernels for a list of uops to the chain. */
static int append_uops(SwsOpChain *chain, const SwsUOpList *uops,
                       int block_size, bool inorder, bool *used_inorder)
{
    for (int i = 0; i < uops->num_ops; i++) {
        SwsAArch64OpImplParams params = { 0 };
        convert_to_aarch64_impl(&uops->ops[i], block_size, inorder, &params);
        SwsFuncPtr func = aarch64_lookup(&params);
        if (!func)
            return AVERROR(ENOTSUP);
        if (params.inorder)
            *used_inorder = true;
        SwsImplResult res = { 0 };
        int ret = aarch64_setup(&uops->ops[i], &params, &res);
        if (ret < 0)
            return ret;
        av_assert1(res.priv_size == aarch64_priv_size(&params));
        ret = ff_sws_op_chain_append(chain, func, res.ref, res.priv, res.priv_size);
        av_refstruct_unref(&res.ref);
        if (ret < 0)
            return ret;
    }
    return 0;
}

/**
 * Build a chain with a dyn header, a sub-chain for out-of-order cores and
 * one for in-order cores. Returns 0 with *pchain set to NULL if no kernel
 * has an in-order variant, so that a plain chain can be used instead.
 */
static int build_dyn_chain(const SwsUOpList *uops, int block_size,
                           SwsOpChain **pchain)
{
    bool used_inorder = false;
    uint64_t chain_off[2];
    int ret;

    *pchain = NULL;
    SwsOpChain *chain = ff_sws_op_chain_alloc();
    if (!chain)
        return AVERROR(ENOMEM);

    chain->impl_size = sizeof_dyn_header;
    for (int inorder = 0; inorder < 2; inorder++) {
        chain_off[inorder] = chain->impl_size;
        ret = append_uops(chain, uops, block_size, inorder, &used_inorder);
        if (ret < 0)
            goto error;
    }
    if (!used_inorder) {
        ff_sws_op_chain_free(chain);
        return 0;
    }

    const int64_t rseq_cpu_id = dyn_info.rseq_cpu_id;
    const uint64_t inorder_mask = dyn_info.inorder_mask;
    memcpy(&chain->impl[offsetof_dyn_rseq_cpu_id],  &rseq_cpu_id,  sizeof(rseq_cpu_id));
    memcpy(&chain->impl[offsetof_dyn_inorder_mask], &inorder_mask, sizeof(inorder_mask));
    memcpy(&chain->impl[offsetof_dyn_chain_off],    chain_off,     sizeof(chain_off));
    chain->cpu_flags = AV_CPU_FLAG_NEON | AV_CPU_FLAG_INORDER;
    *pchain = chain;
    return 0;

error:
    ff_sws_op_chain_free(chain);
    return ret;
}

static int compile_uops_aarch64(SwsContext *ctx, const SwsUOpList *uops,
                                SwsCompiledOp *out)
{
    SwsOpChain *chain = NULL;
    bool is_dyn = false;
    int ret;

    const int cpu_flags = av_get_cpu_flags();
    if (!(cpu_flags & AV_CPU_FLAG_NEON))
        return AVERROR(ENOTSUP);

    ff_thread_once(&dyn_once, dyn_init);

    /**
     * On systems with both kinds of cores, use the block size for
     * out-of-order cores in both sub-chains, since the process function
     * loops over blocks of one size.
     */
    bool inorder = cpu_flags & AV_CPU_FLAG_INORDER;
    int block_size = aarch64_block_size(uops, inorder);
    if (dyn_info.enabled) {
        block_size = aarch64_block_size(uops, false);
        ret = build_dyn_chain(uops, block_size, &chain);
        /* If both sub-chains don't fit, use a plain one. */
        if (ret == AVERROR(EINVAL))
            ret = 0;
        if (ret < 0)
            return ret;
        is_dyn = !!chain;
        inorder = false;
    }

    if (!chain) {
        bool used_inorder = false;
        chain = ff_sws_op_chain_alloc();
        if (!chain)
            return AVERROR(ENOMEM);
        ret = append_uops(chain, uops, block_size, inorder, &used_inorder);
        if (ret < 0)
            goto error;
        chain->cpu_flags = AV_CPU_FLAG_NEON | (used_inorder ? AV_CPU_FLAG_INORDER : 0);
    }

    *out = (SwsCompiledOp) {
        .block_size  = block_size,
        .slice_align = 1,
    };

    /* Look up process function. */
    void ff_sws_process_0001_neon(void);
    void ff_sws_process_0011_neon(void);
    void ff_sws_process_0111_neon(void);
    void ff_sws_process_1111_neon(void);
    void ff_sws_process_0001_dyn_neon(void);
    void ff_sws_process_0011_dyn_neon(void);
    void ff_sws_process_0111_dyn_neon(void);
    void ff_sws_process_1111_dyn_neon(void);
    static void (*const process_funcs[2][4])(void) = {
        { ff_sws_process_0001_neon,     ff_sws_process_0011_neon,
          ff_sws_process_0111_neon,     ff_sws_process_1111_neon },
        { ff_sws_process_0001_dyn_neon, ff_sws_process_0011_dyn_neon,
          ff_sws_process_0111_dyn_neon, ff_sws_process_1111_dyn_neon },
    };

    const int nplanes = av_popcount(uops->planes_in | uops->planes_out);
    av_assert0(nplanes >= 1 && nplanes <= 4);
    out->func = (SwsOpFunc) process_funcs[is_dyn][nplanes - 1];
    ff_sws_op_chain_attach(chain, out);

    return 0;

error:
    ff_sws_op_chain_free(chain);
    return ret;
}

/*********************************************************************/
static int compile_ops_aarch64(SwsContext *ctx, const SwsOpList *ops,
                               SwsCompiledOp *out)
{
    SwsUOpList *uops = ff_sws_uop_list_alloc();
    if (!uops)
        return AVERROR(ENOMEM);

    SwsUOpFlags flags = SWS_UOP_FLAG_PUNNING | SWS_UOP_FLAG_ADD | SWS_UOP_FLAG_SAT_CONVERT;
    if (!(ctx->flags & SWS_BITEXACT))
        flags |= SWS_UOP_FLAG_FMA;
    int ret = ff_sws_ops_translate(ctx, ops, flags, uops);
    if (ret < 0)
        goto error;

    ret = compile_uops_aarch64(ctx, uops, out);

error:
    ff_sws_uop_list_free(&uops);
    return ret;
}

/*********************************************************************/
const SwsOpBackend backend_aarch64 = {
    .name         = "aarch64",
    .flags        = SWS_BACKEND_AARCH64,
    .compile      = compile_ops_aarch64,
    .compile_uops = compile_uops_aarch64,
    .hw_format    = AV_PIX_FMT_NONE,
};
