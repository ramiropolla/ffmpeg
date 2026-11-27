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

/* __GLIBC__ is only defined after including a libc header. */
#if defined(__linux__)
#include <features.h>
#if defined(__GLIBC__)
#if __GLIBC_PREREQ(2, 35)
#include <sys/rseq.h>
#define HAVE_RSEQ_ABI 1
#endif
#endif
#endif
#ifndef HAVE_RSEQ_ABI
#define HAVE_RSEQ_ABI 0
#endif

#include "../ops_chain.h"

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/thread.h"
#include "libavutil/tree.h"
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
    uint8_t coeffs[4 * 16] = { 0 };

    /**
     * Copy non-zero coefficients, packed in sequential order, offset first.
     * The same order must be followed in asmgen_op_linear().
     */
    int i_coeff = 0;
    const int size = ff_sws_pixel_type_size(p->type);
    LOOP_MASK(p, i) {
        for (int j = 0; j < 5; j++) {
            const int jj = (j == 0) ? 4 : (j - 1);
            if (!(p->par.lin.zero & SWS_MASK(i, jj))) {
                memcpy(&coeffs[i_coeff++ * size],
                       &uop->data.mat4x5[i][jj], size);
            }
        }
    }

    ff_sws_op_priv_pack(res, coeffs, num_vregs * 16);
    return 0;
}

/*********************************************************************/
static int aarch64_setup_vec4_by_element(const SwsAArch64OpImplParams *p,
                                         const SwsUOp *uop, SwsImplResult *res)
{
    const int count = ff_sws_vec4_count(&p->par.vec4, p->mask);
    const int size = ff_sws_pixel_type_size(p->type);
    uint8_t vals[16] = { 0 };

    /* Pack values into each lane. */
    for (int i = 0; i < count; i++)
        memcpy(&vals[i * size], &uop->data.vec4[i], size);

    ff_sws_op_priv_pack(res, vals, 16);
    return 0;
}

/*********************************************************************/
static int aarch64_setup_vec4(const SwsAArch64OpImplParams *p,
                              const SwsUOp *uop, SwsImplResult *res)
{
    const int count = ff_sws_vec4_count(&p->par.vec4, p->mask);
    const int size = ff_sws_pixel_type_size(p->type);
    const int el_count = 16 / size;
    uint8_t vals[4 * 16] = { 0 };

    /* Broadcast each value to a full vector register. */
    for (int i = 0; i < count; i++) {
        for (int j = 0; j < el_count; j++) {
            memcpy(&vals[(i * el_count + j) * size],
                   &uop->data.vec4[i], size);
        }
    }

    ff_sws_op_priv_pack(res, vals, count * 16);
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
        return ff_sws_setup_vec4(&(const SwsImplParams) { .uop = uop }, out);
    case SWS_UOP_MUL:
        if (mul_by_element(p->type))
            return aarch64_setup_vec4_by_element(p, uop, out);
        av_fallthrough;
    case SWS_UOP_ADD:
    case SWS_UOP_MIN:
    case SWS_UOP_MAX:
        return aarch64_setup_vec4(p, uop, out);
    case SWS_UOP_LINEAR:
    case SWS_UOP_LINEAR_FMA:
        return aarch64_setup_linear(p, uop, out);
    case SWS_UOP_DITHER:
        return aarch64_setup_dither(uop, out);
    }
    return 0;
}

/*********************************************************************/
/**
 * Selection between the tbl and the structured load/store variants of the
 * packed shuffle functions (see shuffle_ldst() in ops_impl_conv.c), which
 * are both always generated. The structured variant is faster on in-order
 * cores, and the tbl variant on out-of-order cores.
 *  0: always use the tbl variant
 *  1: use the structured variant where it applies
 *  2: choose by core kind; on systems with both kinds of cores, on each
 *     call (see shuffle_dyn())
 */
#ifndef SHUFFLE_LDST
#define SHUFFLE_LDST 2
#endif

/**
 * Choosing the variant on each call needs the number of the current CPU,
 * which glibc keeps up to date in the rseq area of each thread that it has
 * registered with the kernel, and a system with both kinds of cores.
 */
typedef struct AArch64DynInfo {
    bool     enabled;
    int64_t  rseq_cpu_id;   /* offset of rseq.cpu_id from the thread pointer */
    uint64_t inorder_mask;  /* in-order cores */
} AArch64DynInfo;

static AArch64DynInfo dyn_info;
static AVOnce dyn_once = AV_ONCE_INIT;

static av_cold void dyn_init(void)
{
#if HAVE_RSEQ_ABI
    /* __rseq_size is 0 if glibc didn't register the rseq area. */
    if (__rseq_size < offsetof(struct rseq, cpu_id) + sizeof(uint32_t))
        return;
    uint64_t inorder;
    uint64_t ooo;
    if (avpriv_aarch64_get_core_kinds(&inorder, &ooo) < 0 || !inorder || !ooo)
        return;
    dyn_info.rseq_cpu_id  = __rseq_offset + offsetof(struct rseq, cpu_id);
    dyn_info.inorder_mask = inorder;
    dyn_info.enabled      = true;
#endif
}

typedef struct ShuffleDynPriv {
    int64_t       rseq_cpu_id;
    uint64_t      inorder_mask;
    SwsCompiledOp comp[2]; /* for out-of-order and in-order cores */
} ShuffleDynPriv;

/**
 * Run the variant for the kind of the current core. A thread that is
 * migrated during the call keeps running the variant it started with,
 * which only affects the speed.
 */
static void shuffle_dyn(const SwsOpExec *exec, const void *priv,
                        int bx_start, int y_start, int bx_end, int y_end)
{
    const ShuffleDynPriv *p = priv;
    const char *tp = __builtin_thread_pointer();
    const uint32_t cpu = *(const volatile uint32_t *) (tp + p->rseq_cpu_id);
    const SwsCompiledOp *comp = &p->comp[(p->inorder_mask >> cpu) & 1];
    comp->func(exec, comp->priv, bx_start, y_start, bx_end, y_end);
}

static void shuffle_dyn_free(void *priv)
{
    ShuffleDynPriv *p = priv;
    for (int i = 0; i < 2; i++)
        ff_sws_compiled_op_unref(&p->comp[i]);
    av_free(p);
}

static int compile_shuffle_dyn(SwsContext *ctx, const SwsCompiledOp *ooo,
                               const SwsCompiledOp *inorder, SwsCompiledOp *out)
{
    if (ooo->block_size != inorder->block_size)
        return AVERROR(ENOTSUP);

    ShuffleDynPriv *p = av_mallocz(sizeof(*p));
    if (!p)
        return AVERROR(ENOMEM);
    p->rseq_cpu_id  = dyn_info.rseq_cpu_id;
    p->inorder_mask = dyn_info.inorder_mask;
    p->comp[0]      = *ooo;
    p->comp[1]      = *inorder;

    *out = (SwsCompiledOp) {
        .func        = shuffle_dyn,
        .priv        = p,
        .free        = shuffle_dyn_free,
        .slice_align = FFMAX(ooo->slice_align, inorder->slice_align),
        .block_size  = ooo->block_size,
        .cpu_flags   = AV_CPU_FLAG_NEON,
    };
    for (int i = 0; i < 4; i++) {
        out->over_read[i]  = FFMAX(ooo->over_read[i],  inorder->over_read[i]);
        out->over_write[i] = FFMAX(ooo->over_write[i], inorder->over_write[i]);
    }

    av_log(ctx, AV_LOG_VERBOSE, "Choosing the packed shuffle variant per core\n");
    return 0;
}

static int compile_shuffle_ldst_aarch64(SwsContext *ctx, const SwsUOp *uop,
                                        SwsCompiledOp *out)
{
    SwsAArch64OpImplParams params;
    if (!convert_to_aarch64_ldst(uop, &params))
        return AVERROR(ENOTSUP);
    SwsOpFunc func = (SwsOpFunc) aarch64_lookup(&params);
    if (!func)
        return AVERROR(ENOTSUP);

    /* The clear value is broadcast to all bytes of two vector registers. */
    uint8_t *priv = av_malloc(32);
    if (!priv)
        return AVERROR(ENOMEM);
    memset(priv, uop->par.shuffle.clear_value, 32);

    const int elems = 16 / ff_sws_pixel_type_size(params.type);
    *out = (SwsCompiledOp) {
        .func        = func,
        .priv        = priv,
        .free        = av_free,
        .slice_align = 1,
        .block_size  = shuffle_ldst_count(&params.ldst) * elems,
        .cpu_flags   = AV_CPU_FLAG_NEON,
    };

    av_log(ctx, AV_LOG_VERBOSE, "Using ld%d/st%d for the packed shuffle\n",
           params.ldst.num_in, params.ldst.num_out);
    return 0;
}

/**
 * tbl variant of the packed shuffle functions. The tbl indices (and the
 * clear values) are kept in vector registers for the whole call. See
 * shuffle_layout() in ops_impl.h for more details.
 */
static int compile_shuffle_tbl_aarch64(SwsContext *ctx, const SwsUOp *uop,
                                       SwsCompiledOp *out)
{
    const SwsShuffleUOp *par = &uop->par.shuffle;

    SwsAArch64OpImplParams params = { 0 };
    convert_to_aarch64_impl(uop, 0, &params);
    SwsOpFunc func = (SwsOpFunc) aarch64_lookup(&params);
    if (!func)
        return AVERROR(ENOTSUP);

    const SwsShuffleUOp *ipar = &params.par.shuffle;
    SwsAArch64ShuffleLayout l;
    if (shuffle_layout(ipar->read_size, ipar->write_size, params.block_size, &l) < 0)
        return AVERROR(ENOTSUP);

    /* Input byte index for each output byte (or -1 to clear). */
    int8_t shuffle[SHUFFLE_MAX_BYTES];
    int pixels;
    if (params.block_size) {
        /* Lane mode: repeat the pattern of a single period in every lane. */
        const SwsShuffleMask *data = &uop->data.shuffle;
        int period;
        shuffle_lane_bytes(uop, &period);
        for (int pos = 0; pos < l.out_bytes; pos++) {
            const int m = data->mask[pos % period];
            shuffle[pos] = m < 0 ? -1 : m + pos - pos % period;
        }
        av_assert0(par->read_size % data->pixels == 0);
        const int pixel_size = par->read_size / data->pixels;
        av_assert0(l.in_bytes % pixel_size == 0);
        pixels = l.in_bytes / pixel_size;
    } else {
        const int max_size = FFMAX(par->read_size, par->write_size);
        const int groups = ff_sws_shuffle_mask(uop, shuffle, l.groups * max_size);
        if (groups < 0)
            return groups;
        av_assert0(groups == l.groups);
        pixels = l.groups * uop->data.shuffle.pixels;
    }

    /**
     * There is one tbl index vector per distinct set of indices (relative
     * to the first input vector register they reference), followed by the
     * distinct clear value vectors (if needed). See shuffle_layout() for
     * which output vector registers share them. Bytes that are to be
     * cleared get an out of range index, so that tbl sets them to zero.
     */
    const bool clear = par->clear_value != 0;
    uint8_t *priv = av_malloc(16 * (l.num_idx + (clear ? l.num_clr : 0)));
    if (!priv)
        return AVERROR(ENOMEM);
    for (int k = 0; k < l.num_out; k++) {
        uint8_t *idx = priv + 16 * l.idx[k];
        uint8_t *clr = priv + 16 * (l.num_idx + l.clr[k]);
        for (int j = 0; j < 16; j++) {
            const int pos = 16 * k + j;
            const int val = pos < l.out_bytes ? shuffle[pos] : -1;
            av_assert0(val < 0 || (val >= 16 * l.lo[k] && val < 16 * (l.hi[k] + 1)));
            idx[j] = val >= 0 ? val - 16 * l.lo[k] : 0xff;
            if (clear)
                clr[j] = val < 0 ? par->clear_value : 0;
        }
    }

    *out = (SwsCompiledOp) {
        .func        = func,
        .priv        = priv,
        .free        = av_free,
        .slice_align = 1,
        .block_size  = pixels,
        .over_read   = { 0 }, /* partial input vectors are loaded exactly */
        .over_write  = { 0 }, /* partial output vectors are stored exactly */
        .cpu_flags   = AV_CPU_FLAG_NEON,
    };

    return 0;
}

/**
 * Compile both variants and choose between them on each call, or return
 * AVERROR(ENOTSUP) if there is no structured variant.
 */
static int compile_shuffle_per_core(SwsContext *ctx, const SwsUOp *uop,
                                    SwsCompiledOp *out)
{
    SwsCompiledOp inorder;
    int ret = compile_shuffle_ldst_aarch64(ctx, uop, &inorder);
    if (ret < 0)
        return ret;

    SwsCompiledOp ooo;
    ret = compile_shuffle_tbl_aarch64(ctx, uop, &ooo);
    if (ret < 0) {
        ff_sws_compiled_op_unref(&inorder);
        return ret;
    }

    ret = compile_shuffle_dyn(ctx, &ooo, &inorder, out);
    if (ret < 0) {
        ff_sws_compiled_op_unref(&inorder);
        if (ret != AVERROR(ENOTSUP)) {
            ff_sws_compiled_op_unref(&ooo);
            return ret;
        }
        *out = ooo; /* the variants process different block sizes */
    }
    return 0;
}

/**
 * Packed shuffle fast path. The entire conversion is performed by a single
 * standalone function (see SHUFFLE_LDST for which one).
 */
static int compile_shuffle_aarch64(SwsContext *ctx, const SwsUOp *uop,
                                   SwsCompiledOp *out)
{
    int ret;

    bool ldst = SHUFFLE_LDST == 1;
    if (SHUFFLE_LDST == 2) {
        ff_thread_once(&dyn_once, dyn_init);
        if (dyn_info.enabled) {
            ret = compile_shuffle_per_core(ctx, uop, out);
            if (ret != AVERROR(ENOTSUP))
                return ret;
        }
        ldst = av_get_cpu_flags() & AV_CPU_FLAG_INORDER;
    }

    if (ldst) {
        ret = compile_shuffle_ldst_aarch64(ctx, uop, out);
        if (ret != AVERROR(ENOTSUP))
            return ret;
    }
    return compile_shuffle_tbl_aarch64(ctx, uop, out);
}

/*********************************************************************/
static int compile_uops_aarch64(SwsContext *ctx, const SwsUOpList *uops,
                                SwsCompiledOp *out)
{
    int ret;

    const int cpu_flags = av_get_cpu_flags();
    if (!(cpu_flags & AV_CPU_FLAG_NEON))
        return AVERROR(ENOTSUP);

    if (uops->num_ops == 1 && uops->ops[0].uop == SWS_UOP_RW_SHUFFLE) {
        ret = compile_shuffle_aarch64(ctx, &uops->ops[0], out);
        if (ret >= 0) {
            char name[SWS_UOP_NAME_MAX];
            ff_sws_uop_name(&uops->ops[0], name);
            av_log(ctx, AV_LOG_VERBOSE, "Using aarch64 packed shuffle fast path: %s\n", name);
        }
        return ret;
    }

    /* Use at most two full vregs during the widest precision section */
    int block_size = (uops->pixel_size_max == 4) ? 8 : 16;

    SwsOpChain *chain = ff_sws_op_chain_alloc();
    if (!chain)
        return AVERROR(ENOMEM);
    chain->cpu_flags = AV_CPU_FLAG_NEON;
    chain->impl_align = AARCH64_IMPL_DATA_ALIGN;

    *out = (SwsCompiledOp) {
        .block_size  = block_size,
        .slice_align = 1,
    };

    /* Look up kernel functions. */
    for (int i = 0; i < uops->num_ops; i++) {
        SwsAArch64OpImplParams params = { 0 };
        convert_to_aarch64_impl(&uops->ops[i], block_size, &params);
        SwsFuncPtr func = aarch64_lookup(&params);
        if (!func) {
            ret = AVERROR(ENOTSUP);
            goto error;
        }
        SwsImplResult res = { 0 };
        ret = aarch64_setup(&uops->ops[i], &params, &res);
        if (ret < 0)
            goto error;
        av_assert1(res.priv_size == aarch64_priv_size(&params));
        ret = ff_sws_op_chain_append(chain, func, res.ref, res.priv, res.priv_size);
        av_refstruct_unref(&res.ref);
        if (ret < 0)
            goto error;
    }

    /* Look up process function. */
    void ff_sws_process_0001_neon(void);
    void ff_sws_process_0011_neon(void);
    void ff_sws_process_0111_neon(void);
    void ff_sws_process_1111_neon(void);

    SwsOpFunc process_func = NULL;
    switch (av_popcount(uops->planes_in | uops->planes_out)) {
    case 1: process_func = (SwsOpFunc) ff_sws_process_0001_neon; break;
    case 2: process_func = (SwsOpFunc) ff_sws_process_0011_neon; break;
    case 3: process_func = (SwsOpFunc) ff_sws_process_0111_neon; break;
    case 4: process_func = (SwsOpFunc) ff_sws_process_1111_neon; break;
    }

    out->func = process_func;
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

    SwsUOpFlags flags = SWS_UOP_FLAG_VEC4 | SWS_UOP_FLAG_PUNNING |
                        SWS_UOP_FLAG_PSHUFB;
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
