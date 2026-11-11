/**
 * Copyright (C) 2025-2026 Niklas Haas
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

#include <float.h>

#include "libavutil/avassert.h"
#include "libavutil/mem.h"
#include "libavutil/x86/cpu.h"

#include "../ops_chain.h"
#include "../uops.h"
#include "../uops_entries.h"
#include "uops_entries.h"
#include "uops_x86.h"

static int setup_rw_packed(const SwsImplParams *params, SwsImplResult *out)
{
    const SwsUOp *uop = params->uop;

    /* 3-component packed reads/writes process one extra garbage word */
    if (uop->mask == SWS_COMP_ELEMS(3)) {
        switch (uop->uop) {
        case SWS_UOP_READ_PACKED:  out->over_read[0]  = sizeof(uint32_t); break;
        case SWS_UOP_WRITE_PACKED: out->over_write[0] = sizeof(uint32_t); break;
        }
    }

    return 0;
}

/* Private data for the filter kernels; layout hard-coded in ops_float.asm */
typedef struct FilterPriv {
    void *weights;
    size_t filter_size;
} FilterPriv;

static_assert(offsetof(FilterPriv, filter_size) == sizeof(void *),
              "FilterPriv layout mismatch");

static int setup_filter_v(const SwsImplParams *params, SwsImplResult *out)
{
    const SwsFilterWeights *filter = params->uop->data.kernel;

    /* Pre-convert weights to float */
    float *weights = ff_refstruct_calloc(filter->num_weights, sizeof(float));
    if (!weights)
        return AVERROR(ENOMEM);

    for (int i = 0; i < filter->num_weights; i++)
        weights[i] = (float) filter->weights[i] / SWS_FILTER_SCALE;

    const FilterPriv priv = {
        .weights     = weights,
        .filter_size = filter->filter_size,
    };

    SWS_OP_PRIV_PACK(out, priv);
    out->ref = priv.weights;
    return 0;
}

static int hscale_sizeof_weight(const SwsUOp *uop)
{
    switch (uop->type) {
    case SWS_PIXEL_U8:  return sizeof(int16_t);
    case SWS_PIXEL_U16: return sizeof(int16_t);
    case SWS_PIXEL_F32: return sizeof(float);
    default:            return 0;
    }
}

static int setup_filter_h(const SwsImplParams *params, SwsImplResult *out)
{
    const SwsUOp *uop = params->uop;
    const SwsFilterWeights *filter = uop->data.kernel;

    /**
     * `vpgatherdd` gathers 32 bits at a time; so if we're filtering a smaller
     * size, we need to gather 2/4 taps simultaneously and unroll the inner
     * loop over several packed samples.
     */
    const int pixel_size = ff_sws_pixel_type_size(uop->type);
    const int taps_align = sizeof(int32_t) / pixel_size;
    const int filter_size = filter->filter_size;
    const int block_size = uop->block_size;
    const size_t aligned_size = FFALIGN(filter_size, taps_align);
    const size_t line_size = FFALIGN(filter->dst_size, block_size);
    av_assert1(FFALIGN(line_size, taps_align) == line_size);
    if (aligned_size > INT_MAX)
        return AVERROR(EINVAL);

    union {
        void *ptr;
        int16_t *i16;
        float *f32;
    } weights;

    const int sizeof_weight = hscale_sizeof_weight(uop);
    weights.ptr = ff_refstruct_calloc(line_size, sizeof_weight * aligned_size);
    if (!weights.ptr)
        return AVERROR(ENOMEM);

    /**
     * Transpose filter weights to group (aligned) taps by block
     */
    const int mmsize = block_size * 2;
    const int gather_size = mmsize / sizeof(int32_t); /* pixels per vpgatherdd */
    for (size_t x = 0; x < line_size; x += block_size) {
        const int elems = FFMIN(block_size, filter->dst_size - x);
        for (int j = 0; j < filter_size; j++) {
            const int jb = j & ~(taps_align - 1);
            const int ji = j - jb;
            const size_t idx_base = x * aligned_size + jb * block_size + ji;
            for (int i = 0; i < elems; i++) {
                const int w = filter->weights[(x + i) * filter_size + j];
                size_t idx = idx_base;
                if (uop->type == SWS_PIXEL_U8) {
                    /* Interleave the pixels within each lane, i.e.:
                     *  [a0 a1 a2 a3 | b0 b1 b2 b3 ] pixels 0-1, taps 0-3 (lane 0)
                     *  [e0 e1 e2 e3 | f0 f1 f2 f3 ] pixels 4-5, taps 0-3 (lane 1)
                     *  [c0 c1 c2 c3 | d0 d1 d2 d3 ] pixels 2-3, taps 0-3 (lane 0)
                     *  [g0 g1 g2 g3 | h0 h1 h2 h3 ] pixels 6-7, taps 0-3 (lane 1)
                     *  [i0 i1 i2 i3 | j0 j1 j2 j3 ] pixels 8-9, taps 0-3 (lane 0)
                     *  ...
                     *  [o0 o1 o2 o3 | p0 p1 p2 p3 ] pixels 14-15, taps 0-3 (lane 1)
                     *  (repeat for taps 4-7, etc.)
                     */
                    const int gather_base = i & ~(gather_size - 1);
                    const int gather_pos  = i - gather_base;
                    const int lane_idx    = gather_pos >> 2;
                    const int pos_in_lane = gather_pos & 3;
                    idx += gather_base * 4 /* which gather (m0 or m1) */
                         + (pos_in_lane >> 1) * (mmsize / 2) /* lo/hi unpack */
                         + lane_idx * 8 /* 8 ints per lane */
                         + (pos_in_lane & 1) * 4; /* 4 taps per pair */
                } else {
                    idx += i * taps_align;
                }

                switch (uop->type) {
                case SWS_PIXEL_U8:  weights.i16[idx] = w; break;
                case SWS_PIXEL_U16: weights.i16[idx] = w; break;
                case SWS_PIXEL_F32: weights.f32[idx] = w; break;
                }
            }
        }
    }

    const FilterPriv priv = {
        .weights     = weights.ptr,
        .filter_size = aligned_size,
    };

    SWS_OP_PRIV_PACK(out, priv);
    out->ref = priv.weights;

    for (int i = 0; i < 4; i++) {
        if (uop->mask & SWS_COMP(i))
            out->over_read[i] = (aligned_size - filter_size) * pixel_size;
    }
    return 0;
}

static bool check_filter_h_4x4(const SwsImplParams *params)
{
    SwsContext *ctx = params->ctx;
    const SwsUOp *uop = params->uop;
    if ((ctx->flags & SWS_BITEXACT) && uop->type == SWS_PIXEL_F32)
        return false; /* different accumulation order due to 4x4 transpose */

    const int cpu_flags = av_get_cpu_flags();
    if (cpu_flags & AV_CPU_FLAG_SLOW_GATHER)
        return true; /* always prefer over gathers if gathers are slow */

    /**
     * Otherwise, prefer it above a certain filter size. Empirically, this
     * kernel seems to be faster whenever the reference/gather kernel crosses
     * a breakpoint for the number of gathers needed, but this filter doesn't.
     *
     * Tested on a Lunar Lake (Intel Core Ultra 7 258V) system.
     */
    const SwsFilterWeights *filter = uop->data.kernel;
    return uop->type == SWS_PIXEL_U8  && filter->filter_size > 12 ||
           uop->type == SWS_PIXEL_U16 && filter->filter_size > 4  ||
           uop->type == SWS_PIXEL_F32 && filter->filter_size > 1;
}

static int setup_filter_h_4x4(const SwsImplParams *params, SwsImplResult *out)
{
    const SwsUOp *uop = params->uop;
    const SwsFilterWeights *filter = uop->data.kernel;
    const int pixel_size = ff_sws_pixel_type_size(uop->type);
    const int sizeof_weights = hscale_sizeof_weight(uop);
    const int block_size = uop->block_size;
    const int taps_align = 16 / sizeof_weights; /* taps per iteration (XMM) */
    const int pixels_align = 4; /* pixels per iteration */
    const int filter_size = filter->filter_size;
    const size_t aligned_size = FFALIGN(filter_size, taps_align);
    const int line_size = FFALIGN(filter->dst_size, block_size);
    av_assert1(FFALIGN(line_size, pixels_align) == line_size);

    union {
        void *ptr;
        int16_t *i16;
        float *f32;
    } weights;

    weights.ptr = ff_refstruct_calloc(line_size, aligned_size * sizeof_weights);
    if (!weights.ptr)
        return AVERROR(ENOMEM);

    /**
     * Desired memory layout: [w][taps][pixels_align][taps_align]
     *
     * Example with taps_align=8, pixels_align=4:
     *   [a0, a1, ... a7]  weights for pixel 0, taps 0..7
     *   [b0, b1, ... b7]  weights for pixel 1, taps 0..7
     *   [c0, c1, ... c7]  weights for pixel 2, taps 0..7
     *   [d0, d1, ... d7]  weights for pixel 3, taps 0..7
     *   [a8, a9, ... a15] weights for pixel 0, taps 8..15
     *   ...
     *   repeat for all taps, then move on to pixels 4..7, etc.
     */
    for (int x = 0; x < filter->dst_size; x++) {
        for (int j = 0; j < filter_size; j++) {
            const int xb = x & ~(pixels_align - 1);
            const int jb = j & ~(taps_align - 1);
            const int xi = x - xb, ji = j - jb;
            const int w = filter->weights[x * filter_size + j];
            const int idx = xb * aligned_size + jb * pixels_align + xi * taps_align + ji;

            switch (uop->type) {
            case SWS_PIXEL_U8:  weights.i16[idx] = w; break;
            case SWS_PIXEL_U16: weights.i16[idx] = w; break;
            case SWS_PIXEL_F32: weights.f32[idx] = w; break;
            }
        }
    }

    const FilterPriv priv = {
        .weights     = weights.ptr,
        .filter_size = aligned_size * sizeof_weights,
    };

    SWS_OP_PRIV_PACK(out, priv);
    out->ref = priv.weights;

    for (int i = 0; i < 4; i++) {
        if (uop->mask & SWS_COMP(i))
            out->over_read[i] = (aligned_size - filter_size) * pixel_size;
    }
    return 0;
}

static int setup_scale(const SwsImplParams *params, SwsImplResult *out)
{
    const SwsPixel scalar = params->uop->data.scalar;
    switch (params->uop->type) {
    case SWS_PIXEL_U8: {
        const uint32_t u32 = scalar.u8; /* for movd */
        SWS_OP_PRIV_PACK(out, u32);
        break;
    }
    case SWS_PIXEL_U16: SWS_OP_PRIV_PACK(out, scalar.u16); break;
    case SWS_PIXEL_U32: SWS_OP_PRIV_PACK(out, scalar.u32); break;
    case SWS_PIXEL_F32: SWS_OP_PRIV_PACK(out, scalar.f32); break;
    default: return AVERROR(EINVAL);
    }

    return 0;
}

static int setup_clear(const SwsImplParams *params, SwsImplResult *out)
{
    const SwsUOp *uop = params->uop;
    for (int i = 0; i < 4; i++)
        SWS_OP_PRIV_PACK(out, uop->data.vec4[i].u32);
    return 0;
}

static int setup_dither(const SwsImplParams *params, SwsImplResult *out)
{
    ff_sws_op_priv_pack_refptr(out, av_refstruct_ref(params->uop->data.ptr));
    return 0;
}

static void splat_lane(void *dst, SwsPixelType type, SwsPixel px)
{
    switch (ff_sws_pixel_type_size(type)) {
    case 1:
        memset(dst, px.u8, 16);
        break;
    case 2:
        for (int i = 0; i < 8; i++)
            ((uint16_t *) dst)[i] = px.u16;
        break;
    case 4:
        for (int i = 0; i < 4; i++)
            ((uint32_t *) dst)[i] = px.u32;
        break;
    }
}

static int setup_linear(const SwsImplParams *params, SwsImplResult *out)
{
    const SwsUOp *uop = params->uop;
    if (uop->type == SWS_PIXEL_F32) {
        /* Layout hard-coded in ops_float.asm */
        static_assert(sizeof(uop->data.mat4x5) == 4 * 5 * sizeof(float),
                      "mat4x5 layout mismatch");
        SWS_OP_PRIV_PACK(out, uop->data.mat4x5);
        return 0;
    }

    uint8_t *mat = av_refstruct_allocz(4 * 5 * 16); /* one lane per component */
    if (!mat)
        return AVERROR(ENOMEM);
    ff_sws_op_priv_pack_refptr(out, mat);

    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 5; j++) {
            SwsPixel px = uop->data.mat4x5[i][j];
            SwsPixelType type = uop->type;
            if (type == SWS_PIXEL_U8) {
                type = SWS_PIXEL_U16; /* for pmullw */
                px.u16 = (px.u8 << 8) | px.u8;
            }

            splat_lane(mat, type, px);
            mat += 16;
        }
    }
    return 0;
}

/**
 * Check and setup functions of each uop type, as arguments of M. Several
 * kernel variants are declared in order of preference, distinguished by a
 * name suffix.
 */
#define X86_ENTRY_READ_PLANAR(M, ...)        M(NULL, NULL, , __VA_ARGS__)
#define X86_ENTRY_READ_PLANAR_FH(M, ...)                                        \
    M(check_filter_h_4x4, setup_filter_h_4x4, _4x4, __VA_ARGS__)                \
    M(NULL, setup_filter_h, , __VA_ARGS__)
#define X86_ENTRY_READ_PLANAR_FV(M, ...)     M(NULL, setup_filter_v, , __VA_ARGS__)
#define X86_ENTRY_READ_PLANAR_FV_FMA(M, ...) M(NULL, setup_filter_v, , __VA_ARGS__)
#define X86_ENTRY_READ_PACKED(M, ...)        M(NULL, setup_rw_packed, , __VA_ARGS__)
#define X86_ENTRY_READ_NIBBLE(M, ...)        M(NULL, NULL, , __VA_ARGS__)
#define X86_ENTRY_READ_BIT(M, ...)           M(NULL, NULL, , __VA_ARGS__)
#define X86_ENTRY_READ_PALETTE(M, ...)       M(NULL, NULL, , __VA_ARGS__)
#define X86_ENTRY_WRITE_PLANAR(M, ...)       M(NULL, NULL, , __VA_ARGS__)
#define X86_ENTRY_WRITE_PACKED(M, ...)       M(NULL, setup_rw_packed, , __VA_ARGS__)
#define X86_ENTRY_WRITE_NIBBLE(M, ...)       M(NULL, NULL, , __VA_ARGS__)
#define X86_ENTRY_WRITE_BIT(M, ...)          M(NULL, NULL, , __VA_ARGS__)
#define X86_ENTRY_RW_SHUFFLE(M, ...)         M(NULL, NULL, , __VA_ARGS__)
#define X86_ENTRY_PERMUTE(M, ...)            M(NULL, NULL, , __VA_ARGS__)
#define X86_ENTRY_COPY(M, ...)               M(NULL, NULL, , __VA_ARGS__)
#define X86_ENTRY_SWAP_BYTES(M, ...)         M(NULL, NULL, , __VA_ARGS__)
#define X86_ENTRY_EXPAND_BIT(M, ...)         M(NULL, NULL, , __VA_ARGS__)
#define X86_ENTRY_EXPAND_PAIR(M, ...)        M(NULL, NULL, , __VA_ARGS__)
#define X86_ENTRY_EXPAND_QUAD(M, ...)        M(NULL, NULL, , __VA_ARGS__)
#define X86_ENTRY_TO_U8(M, ...)              M(NULL, NULL, , __VA_ARGS__)
#define X86_ENTRY_TO_U16(M, ...)             M(NULL, NULL, , __VA_ARGS__)
#define X86_ENTRY_TO_U32(M, ...)             M(NULL, NULL, , __VA_ARGS__)
#define X86_ENTRY_TO_F32(M, ...)             M(NULL, NULL, , __VA_ARGS__)
#define X86_ENTRY_SCALE(M, ...)              M(NULL, setup_scale, , __VA_ARGS__)
#define X86_ENTRY_ADD(M, ...)                M(NULL, ff_sws_setup_vec4, , __VA_ARGS__)
#define X86_ENTRY_MIN(M, ...)                M(NULL, ff_sws_setup_vec4, , __VA_ARGS__)
#define X86_ENTRY_MAX(M, ...)                M(NULL, ff_sws_setup_vec4, , __VA_ARGS__)
#define X86_ENTRY_UNPACK(M, ...)             M(NULL, NULL, , __VA_ARGS__)
#define X86_ENTRY_PACK(M, ...)               M(NULL, NULL, , __VA_ARGS__)
#define X86_ENTRY_LSHIFT(M, ...)             M(NULL, NULL, , __VA_ARGS__)
#define X86_ENTRY_RSHIFT(M, ...)             M(NULL, NULL, , __VA_ARGS__)
#define X86_ENTRY_CLEAR(M, ...)              M(NULL, setup_clear, , __VA_ARGS__)
#define X86_ENTRY_LINEAR(M, ...)             M(NULL, setup_linear, , __VA_ARGS__)
#define X86_ENTRY_LINEAR_FMA(M, ...)         M(NULL, setup_linear, , __VA_ARGS__)
#define X86_ENTRY_DITHER(M, ...)             M(NULL, setup_dither, , __VA_ARGS__)

#define X86_ENTRY(M, EXT, NAME, TYPE, UOP, ...)                                 \
    X86_ENTRY_##UOP(M, EXT, NAME, __VA_ARGS__)

#define X86_DECL(CHECK, SETUP, SUFFIX, EXT, NAME, BLOCK_SIZE, ARGS, INIT)       \
    void ff_##NAME##SUFFIX##EXT(void);                                          \
    static const SwsUOpEntry uop_##NAME##SUFFIX##EXT = {                        \
        SWS_UNPAREN INIT,                                                       \
        .func  = (SwsFuncPtr) ff_##NAME##SUFFIX##EXT,                           \
        .check = CHECK,                                                         \
        .setup = SETUP,                                                         \
    };

#define X86_REF(CHECK, SETUP, SUFFIX, EXT, NAME, ...) &uop_##NAME##SUFFIX##EXT,

#define X86_TABLE(LIST, EXT, FLAG)                                              \
    SWS_ENTRIES_##LIST(X86_ENTRY, X86_DECL, EXT)                                \
    static const SwsUOpTable uops##EXT = {                                      \
        .cpu_flags = AV_CPU_FLAG_##FLAG,                                        \
        .entries = {                                                            \
            SWS_ENTRIES_##LIST(X86_ENTRY, X86_REF, EXT)                         \
            NULL                                                                \
        },                                                                      \
    };

X86_TABLE(X86_SSE4,      _sse4,      SSE4)
X86_TABLE(X86_AVX2,      _avx2,      AVX2)
X86_TABLE(X86_AVX512,    _avx512,    AVX512)
X86_TABLE(X86_AVX512ICL, _avx512icl, AVX512ICL)

static const SwsUOpTable *const tables[SWS_X86_ISA_NB] = {
    [SWS_X86_SSE4]      = &uops_sse4,
    [SWS_X86_AVX2]      = &uops_avx2,
    [SWS_X86_AVX512]    = &uops_avx512,
    [SWS_X86_AVX512ICL] = &uops_avx512icl,
};

SWS_DECL_FUNC(ff_sws_process1_x86);
SWS_DECL_FUNC(ff_sws_process2_x86);
SWS_DECL_FUNC(ff_sws_process3_x86);
SWS_DECL_FUNC(ff_sws_process4_x86);

/* AV_CPU_FLAG_* of the instruction sets the kernels were assembled for */
static int get_cpu_flags(void)
{
    const int cpu_flags = av_get_cpu_flags();
    int flags = 0;
    if (EXTERNAL_SSE4(cpu_flags))
        flags |= AV_CPU_FLAG_SSE4;
    if (EXTERNAL_AVX2(cpu_flags))
        flags |= AV_CPU_FLAG_AVX2;
    if (EXTERNAL_FMA3(cpu_flags))
        flags |= AV_CPU_FLAG_FMA3;
    if (EXTERNAL_AVX512(cpu_flags))
        flags |= AV_CPU_FLAG_AVX512;
    if (EXTERNAL_AVX512ICL(cpu_flags))
        flags |= AV_CPU_FLAG_AVX512ICL;
    return flags;
}

static int get_level(void)
{
    return ff_sws_x86_level(get_cpu_flags());
}

static int movsize(const int bytes, const int mmsize)
{
    return bytes <= 4  ? 4  : /* movd */
           bytes <= 8  ? 8  : /* movq */
           bytes <= 16 ? 16 : /* xmm movu */
           bytes <= 32 ? 32 : /* ymm movu */
           mmsize;            /* zmm movu */
}

static int translate_shuffle(const SwsUOp *uop, SwsX86ISA level, SwsCompiledOp *out)
{
    /* We can't shuffle across lanes, so restrict the vector size to XMM
     * whenever the read/write size would be a subset of the full vector,
     * unless we have access to AVX-512 ICL vpermb */
    const SwsShuffleUOp *par = &uop->par.shuffle;
    const int lane_aligned = par->read_size == par->write_size &&
                             16 % par->read_size == 0;
    const SwsX86ISA isa = ff_sws_uop_shuffle_isa_x86(level, uop);
    const int mmsize = ff_sws_mmsize_x86(isa);

    /* Generate the shuffle mask */
    const int mask_size = lane_aligned ? 16 : mmsize;
    int8_t *mask = av_malloc(mask_size);
    if (!mask)
        return AVERROR(ENOMEM);

    const int groups = ff_sws_shuffle_mask(uop, mask, mask_size);
    if (groups < 0) {
        av_free(mask);
        return groups;
    }

    const int read_chunk  = groups * par->read_size;
    const int write_chunk = groups * par->write_size;
    const int num_lanes   = lane_aligned ? mmsize / 16 : 1;
    const int in_total    = num_lanes * read_chunk;
    const int out_total   = num_lanes * write_chunk;
    *out = (SwsCompiledOp) {
        .priv        = mask,
        .free        = av_free,
        .slice_align = 1,
        .block_size  = groups * uop->data.shuffle.pixels * num_lanes,
        .over_read   = { movsize(in_total,  mmsize) - in_total },
        .over_write  = { movsize(out_total, mmsize) - out_total },
    };

    const SwsUOpTable *table = tables[isa];
    for (int i = 0; table->entries[i]; i++) {
        const SwsUOpEntry *entry = table->entries[i];
        if (entry->uop == SWS_UOP_RW_SHUFFLE &&
            !memcmp(&uop->par, &entry->par, sizeof(uop->par))) {
            out->func = (SwsOpFunc) entry->func;
            out->cpu_flags = table->cpu_flags;
            break;
        }
    }

    if (!out->func) {
        av_free(mask);
        return AVERROR(ENOTSUP);
    }

    return 0;
}

static int compile_uops_x86(SwsContext *ctx, const SwsUOpList *uops, SwsCompiledOp *out)
{
    int ret;
    const int level = get_level();
    if (level < 0)
        return level;

    if (uops->num_ops == 1 && uops->ops[0].uop == SWS_UOP_RW_SHUFFLE) {
        const SwsUOp *uop = &uops->ops[0];
        ret = translate_shuffle(uop, level, out);
        if (ret >= 0) {
            char name[SWS_UOP_NAME_MAX];
            ff_sws_uop_name(uop, name);
            av_log(ctx, AV_LOG_VERBOSE, "Using x86 packed shuffle fast path: %s\n", name);
        }
        return ret;
    }

    SwsOpChain *chain = ff_sws_op_chain_alloc();
    if (!chain)
        return AVERROR(ENOMEM);

    *out = (SwsCompiledOp) {
        .block_size  = ff_sws_uop_block_size_x86(level, uops),
        .slice_align = 1,
    };

    for (int i = 0; i < uops->num_ops; i++) {
        SwsUOp uop = uops->ops[i];
        uop.block_size = out->block_size;
        ret = ff_sws_uop_simplify_x86(level, &uop);
        if (ret < 0)
            goto fail;

        ret = ff_sws_uop_lookup(ctx, &tables[ret], 1, &uop, chain);
        if (ret < 0)
            goto fail;
    }

    switch (av_popcount(uops->planes_in | uops->planes_out)) {
    case 1: out->func = ff_sws_process1_x86; break;
    case 2: out->func = ff_sws_process2_x86; break;
    case 3: out->func = ff_sws_process3_x86; break;
    case 4: out->func = ff_sws_process4_x86; break;
    }

    ff_sws_uop_list_print(ctx, AV_LOG_DEBUG, uops);
    ff_sws_op_chain_attach(chain, out);
    return 0;

fail:
    ff_sws_op_chain_free(chain);
    return ret;
}

static int compile_ops_x86(SwsContext *ctx, const SwsOpList *ops, SwsCompiledOp *out)
{
    const int cpu_flags = av_get_cpu_flags();
    const int level = get_level();
    if (level < 0)
        return level;

    const SwsUOpFlags flags = ff_sws_uop_flags_x86(EXTERNAL_FMA3(cpu_flags));
    SwsUOpList *uops = ff_sws_uop_list_alloc();
    if (!uops)
        return AVERROR(ENOMEM);

    int ret = ff_sws_ops_translate(ctx, ops, flags, uops);
    if (ret < 0)
        goto fail;

    ret = compile_uops_x86(ctx, uops, out);

fail:
    ff_sws_uop_list_free(&uops);
    return ret;
}

const SwsOpBackend backend_x86 = {
    .name           = "x86",
    .flags          = SWS_BACKEND_X86,
    .compile        = compile_ops_x86,
    .compile_uops   = compile_uops_x86,
    .hw_format      = AV_PIX_FMT_NONE,
};
