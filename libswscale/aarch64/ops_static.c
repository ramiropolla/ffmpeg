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

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

/**
 * This file is compiled as a standalone build-time tool and must not depend
 * on internal FFmpeg libraries. The necessary utils are redefined below using
 * standard C equivalents.
 */

#define AVUTIL_AVASSERT_H
#define AVUTIL_LOG_H
#define AVUTIL_MACROS_H
#define AVUTIL_MEM_H
#define AV_STRINGIFY(s)         AV_TOSTRING(s)
#define AV_TOSTRING(s) #s
#define av_assert0(cond) do {                                           \
    if (!(cond)) {                                                      \
        fprintf(stderr, "Assertion %s failed at %s:%d\n",               \
               AV_STRINGIFY(cond), __FILE__, __LINE__);                 \
        abort();                                                        \
    }                                                                   \
} while (0)
#define av_malloc(s)     malloc(s)
#define av_mallocz(s)    calloc(1, s)
#define av_realloc(p, s) realloc(p, s)
#define av_strdup(s)     strdup(s)
#define av_free(p)       free(p)
#define FFMAX(a,b) ((a) > (b) ? (a) : (b))
#define FFMIN(a,b) ((a) > (b) ? (b) : (a))
#define FF_ARRAY_ELEMS(a) (sizeof(a) / sizeof((a)[0]))
#define MKTAG(a,b,c,d) ((a) | ((b) << 8) | ((c) << 16) | ((unsigned)(d) << 24))
#define FFALIGN(x, a) (((x)+(a)-1)&~((a)-1))

static void av_freep(void *ptr)
{
    void **pptr = (void **) ptr;
    if (pptr) {
        ptr = *pptr;
        if (ptr)
            free(ptr);
        *pptr = NULL;
    }
}

static void *av_memdup(const void *p, size_t size)
{
    void *ptr = NULL;
    if (p) {
        ptr = av_malloc(size);
        if (ptr)
            memcpy(ptr, p, size);
    }
    return ptr;
}

static int av_popcount(uint32_t x)
{
    x -= (x >> 1) & 0x55555555;
    x = (x & 0x33333333) + ((x >> 2) & 0x33333333);
    x = (x + (x >> 4)) & 0x0F0F0F0F;
    x += x >> 8;
    return (x + (x >> 16)) & 0x3F;
}

#include "libavutil/dynarray.h"

static void *av_dynarray2_add(void **tab_ptr, int *nb_ptr, size_t elem_size,
                              const uint8_t *elem_data)
{
    uint8_t *tab_elem_data = NULL;

    FF_DYNARRAY_ADD(INT_MAX, elem_size, *tab_ptr, *nb_ptr, {
        tab_elem_data = (uint8_t *)*tab_ptr + (*nb_ptr) * elem_size;
        if (elem_data)
            memcpy(tab_elem_data, elem_data, elem_size);
    }, {
        av_freep(tab_ptr);
        *nb_ptr = 0;
    });
    return tab_elem_data;
}

#include "libavutil/bprint.c"

/*********************************************************************/
#include "rasm.c"
#include "rasm_print.c"
#include "ops_impl.h"

/**
 * Implementation parameters for all exported functions. This list is
 * compiled by performing a dummy run of all conversions in sws_ops and
 * collecting all functions that need to be generated. This is achieved
 * by running:
 *   make fate-sws-ops-entries-aarch64 GEN=1
 */
typedef struct SwsAArch64OpEntry {
    const char *name;
    SwsAArch64OpImplParams params;
} SwsAArch64OpEntry;

static const SwsAArch64OpEntry ops_entries[] = {
#define ENTRY(fname, ...) { .name = #fname, .params = __VA_ARGS__ },
#include "ops_entries.c"
#undef ENTRY
    { NULL }
};

#include "ops_asmgen.c"

/*********************************************************************/
#define IMPL_PRIV(s) a64op_off(s->impl, AARCH64_IMPL_DATA_OFFSET)

/**
 * Set node where the continuation address will be loaded and impl will
 * be incremented. This should be done right after impl->priv has been
 * used.
 */
static void asmgen_set_load_cont_node(SwsAArch64Context *s)
{
    RasmContext *r = s->rctx;
    s->load_cont_node = rasm_get_current_node(r);
}

/*********************************************************************/
/* SWS_UOP_READ_BIT:    fractional read (1 bit) from single plane */
/* SWS_UOP_READ_NIBBLE: fractional read (4 bits) from single plane */
/* SWS_UOP_READ_PACKED: simple packed byte-aligned read */
/* SWS_UOP_READ_PLANAR: simple planar byte-aligned read */

static void asmgen_setup_read_bit(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                                  SwsAArch64OpRegs *regs)
{
    RasmContext *r = s->rctx;
    AArch64VecViews shift_vec   = a64op_vec_views(regs->vk[0]);
    AArch64VecViews bitmask_vec = a64op_vec_views(regs->vk[1]);

    rasm_annotate_next(r, "v128 shift_vec = impl->priv.v128;");
    i_ldr(r, shift_vec.q, IMPL_PRIV(s));
    asmgen_set_load_cont_node(s);
    if (p->block_size == 16) {
        i_movi(r, bitmask_vec.b16, IMM(1));                 CMT("v128 bitmask_vec = {1 <repeats 16 times>};");
    } else {
        i_movi(r, bitmask_vec.b8,  IMM(1));                 CMT("v128 bitmask_vec = {1 <repeats 8 times>, 0 <repeats 8 times>};");
    }
}

static void asmgen_setup_read_nibble(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                                     SwsAArch64OpRegs *regs)
{
    RasmContext *r = s->rctx;
    AArch64VecViews nibble_mask = a64op_vec_views(regs->vk[0]);

    rasm_annotate_next(r, "v128 nibble_mask = {0xf <repeats 8 times>, 0x0 <repeats 8 times>};");
    i_movi(r, nibble_mask.b8, IMM(0x0f));
}

static void asmgen_setup_write_bit(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                                   SwsAArch64OpRegs *regs)
{
    RasmContext *r = s->rctx;
    AArch64VecViews shift_vec = a64op_vec_views(regs->vk[0]);

    rasm_annotate_next(r, "v128 shift_vec = impl->priv.v128;");
    i_ldr(r, shift_vec.q, IMPL_PRIV(s));
    asmgen_set_load_cont_node(s);
}

static void asmgen_setup_unpack(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                                SwsAArch64OpRegs *regs)
{
    RasmContext *r = s->rctx;
    RasmOp *vmask = regs->vk;
    RasmOp mask_gpr = a64op_w(s->tmp0);
    uint32_t mask_val[4] = { 0 };

    /* Generate masks. */
    rasm_add_comment(r, "generate masks");
    LOOP_MASK(p, i) {
        uint32_t val = (1u << p->par.pack.pattern[i]) - 1;
        for (int j = 0; j < 4; j++) {
            if (mask_val[j] == val) {
                mask_val[i] = mask_val[j];
                vmask[i] = vmask[j];
                break;
            }
        }
        if (!mask_val[i]) {
            /**
             * All-one values in movi only work up to 8-bit, and then
             * at full 16- or 32-bit, but not for intermediate values
             * like 10-bit. In those cases, we use mov + dup instead.
             */
            if (val <= 0xff || val == 0xffff) {
                i_movi(r, vmask[i], IMM(val));
            } else {
                i_mov (r, mask_gpr, IMM(val));
                i_dup (r, vmask[i], mask_gpr);
            }
            mask_val[i] = val;
            vmask[i] = v_16b(vmask[i]);
        }
    }
}

static void asmgen_setup_clear(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                               SwsAArch64OpRegs *regs)
{
    RasmContext *r = s->rctx;
    RasmOp *vk = regs->vk;

    /**
     * TODO
     * - pack elements in impl->priv and perform smaller loads
     * - if only 1 element and not vh, load directly with ld1r
     */

    bool load_priv = false;
    LOOP_MASK(p, i) {
        if (!((p->par.clear.zero | p->par.clear.one) & SWS_COMP(i)))
            load_priv = true;
    }
    if (load_priv) {
        i_ldr(r, v_q(vk[0]), IMPL_PRIV(s));         CMT("v128 clear_vec = impl->priv.v128;");
        asmgen_set_load_cont_node(s);
    }
}

static void asmgen_setup_vec4_by_element(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                                         SwsAArch64OpRegs *regs, const char *name)
{
    RasmContext *r = s->rctx;
    RasmOp *vk = regs->vk;

    RasmOp vec = vk[0];

    /**
     * The second source register in the 16-bit by-element variants of multiplication
     * instructions is restricted to v0-v15, so we use v8 instead of vk.
     */
    if (p->type == SWS_PIXEL_U16)
        vec = a64op_make_vec(8, s->el_count, s->el_size);

    /* Load priv data. */
    int count = ff_sws_vec4_count(&p->par.vec4, p->mask);
    av_assert0(count && !p->par.vec4.zero);

    size_t size = count * ff_sws_pixel_type_size(p->type);
    size = size <= 2 ? 2 :
           size <= 4 ? 4 :
           size <= 8 ? 8 : 16;
    i_ldr(r, a64op_scalar(vec, size), IMPL_PRIV(s));                CMTF("v%s = impl->priv[0];", name);
    asmgen_set_load_cont_node(s);

    /* Update vk for asmgen. */
    LOOP_MASK(p, i) {
        vk[i] = a64op_elem(vec, p->par.vec4.indices[i]);
    }
}

static void asmgen_setup_vec4(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                              SwsAArch64OpRegs *regs, const char *name)
{
    RasmContext *r = s->rctx;
    RasmOp *vk = regs->vk;

    /* Load priv data. */
    int count = ff_sws_vec4_count(&p->par.vec4, p->mask);
    if (count) {
        char vname[16];
        snprintf(vname, sizeof(vname), "v%s", name);
        load_regs(s, count, 16,
                  vname, vk,
                  "impl->priv", s->impl, AARCH64_IMPL_DATA_OFFSET);
        asmgen_set_load_cont_node(s);
    }

    /* Load zero vector. */
    if (p->par.vec4.zero) {
        i_movi(r, vk[count], IMM(0));                               CMTF("%s = 0;", VEC4_NAME(p, name, 0));
    }

    /* Update vk for asmgen. */
    RasmOp orig_vk[4];
    memcpy(orig_vk, vk, sizeof(orig_vk));
    LOOP_MASK(p, i) {
        if (p->par.vec4.zero & SWS_COMP(i)) {
            vk[i] = vk[count];
        } else {
            vk[i] = orig_vk[p->par.vec4.indices[i]];
        }
    }
}

static void asmgen_setup_add(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                             SwsAArch64OpRegs *regs)
{
    asmgen_setup_vec4(s, p, regs, "add");
}

static void asmgen_setup_min(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                             SwsAArch64OpRegs *regs)
{
    asmgen_setup_vec4(s, p, regs, "min");
}

static void asmgen_setup_max(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                             SwsAArch64OpRegs *regs)
{
    asmgen_setup_vec4(s, p, regs, "max");
}

static void asmgen_setup_mul(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                             SwsAArch64OpRegs *regs)
{
    if (mul_by_element(p->type)) {
        asmgen_setup_vec4_by_element(s, p, regs, "mul");
    } else {
        asmgen_setup_vec4(s, p, regs, "mul");
    }
}

static void asmgen_setup_linear(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                                SwsAArch64OpRegs *regs)
{
    RasmContext *r = s->rctx;
    RasmOp *sl = regs->sl;
    RasmOp *sh = regs->sh;
    RasmOp *vc = regs->vk;
    RasmOp *vt = regs->vt;

    /* Preload coefficients from impl->priv. */
    const int num_vregs = linear_num_vregs(p);
    av_assert0(num_vregs <= 4);
    load_regs(s, num_vregs, s->vec_size,
              "vcoeff", vc,
              "impl->priv", s->impl, AARCH64_IMPL_DATA_OFFSET);
    asmgen_set_load_cont_node(s);

    /**
     * Integer mul/mla by element does not exist for 8-bit elements, and
     * requires the element register to be in v0-v15 for 16-bit elements.
     * For these types, the coefficients used as multiplication operands
     * are broadcast into full temp vectors instead. Temp vectors 8-11
     * are otherwise only used by the floating-point fmul+fadd path, so
     * they are free for integer types.
     */
    const bool dup_coeffs = (p->type == SWS_PIXEL_U8 || p->type == SWS_PIXEL_U16);
    const int nelems = linear_vreg_nelems(p->type);
    RasmOp *vint = &vt[8];
    int i_vint = 0;

    /**
     * Populate operands matrix from packed data into linear_vcoeff matrix
     * and compute mask for rows that must be saved before being overwritten.
     */
    SwsCompMask save_mask = 0;
    bool overwritten[4] = { false, false, false, false };
    int i_coeff = 0;
    LOOP_MASK(p, i) {
        bool first = true;
        for (int j = 0; j < 5; j++) {
            bool is_offset = (j == 0);
            int src_j = is_offset ? 4 : (j - 1);
            if (p->par.lin.zero & SWS_MASK(i, src_j))
                continue;
            uint8_t vc_i = i_coeff / nelems;
            uint8_t vc_j = i_coeff & (nelems - 1);
            RasmOp vcoeff = a64op_elem(vc[vc_i], vc_j);
            /**
             * The first coefficient of a row is not used as a multiplication
             * operand if it is either an offset (broadcast) or one (move).
             */
            bool is_one = !is_offset && (p->par.lin.one & SWS_MASK(i, src_j));
            if (dup_coeffs && !(first && (is_offset || is_one))) {
                av_assert0(i_vint < 4);
                i_dup(r, vint[i_vint], vcoeff);             CMTF("vcoeff%u = broadcast(coeff[%u][%u]);", i_vint, i, src_j);
                vcoeff = vint[i_vint++];
            }
            regs->linear_vcoeff[i][j] = vcoeff;
            i_coeff++;
            first = false;
            if (!is_offset && overwritten[src_j])
                save_mask |= SWS_COMP(src_j);
            overwritten[i] = true;
        }
    }

    /**
     * Save rows that need to be used as input after they have been already
     * written to.
     */
    RasmOp *tl = &vt[0];
    RasmOp *th = &vt[4];
    LOOP      (save_mask, i) { i_mov16b(r, tl[i], sl[i]);  CMTF("vsrcl[%u] = vl[%u];", i, i); }
    LOOP_VH(s, save_mask, i) { i_mov16b(r, th[i], sh[i]);  CMTF("vsrch[%u] = vh[%u];", i, i); }
    LOOP      (save_mask, i) { sl[i] = tl[i]; }
    LOOP_VH(s, save_mask, i) { sh[i] = th[i]; }
}

static void asmgen_setup_dither(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                                SwsAArch64OpRegs *regs)
{
    RasmContext *r = s->rctx;
    RasmOp src_ptr = s->tmp0;

    regs->dither_ptr = src_ptr;
    i_ldr(r, src_ptr, IMPL_PRIV(s));                        CMT("void *ptr = impl->priv.ptr;");
    asmgen_set_load_cont_node(s);
}

/*********************************************************************/
/**
 * Register assignment for CPS functions.
 *
 * The entry point of the SwsOpFunc is the `process` function. The
 * first kernel function is called from `process`, and subsequent
 * kernel functions are chained by directly branching to the next
 * operation, using a continuation-passing style design. The last
 * operation must be a write operation, which returns from the call
 * to the `process` function.
 *
 * The GPRs used by the entire call-chain are listed below.
 *
 * Function arguments are passed in r0-r5. After the parameters from
 * `exec` have been read, r0 is reused to branch to the continuation
 * functions. After the original parameters from `impl` have been
 * computed, r1 is reused as the `impl` pointer for each operation.
 *
 * Loop iterators are r6 for `bx` and r3 for `y`, reused from
 * `y_start`, which doesn't need to be preserved.
 *
 * The intra-procedure-call temporary registers (r16 and r17) are used
 * as scratch registers. They may be used by call veneers and PLT code
 * inserted by the linker, so we cannot expect them to persist across
 * branches between functions.
 *
 * The Platform Register (r18) is not used.
 *
 * The read/write data pointers and padding values first use up the
 * remaining free caller-saved registers, and only then are the
 * callee-saved registers (r19-r29) used.
 *
 * The Link Register (r30) is used when calling the first kernel, so it
 * must be saved.
 */

static const int rw_gprs[] = {
     9, 10, 11, 12,
    13, 14, 15, 19,
    20, 21, 22, 23,
    24, 25, 26, 27,
};

static void asmgen_common_frame(SwsAArch64Context *s, SwsCompMask imask, SwsCompMask omask)
{
    /* Loop iterator variables. */
    s->bx        = a64op_gpw(6);
    s->y         = a64op_gpw(3);    /* Reused from SwsOpFunc.y_start argument. */

    /* Scratch registers. */
    s->tmp0      = a64op_gpx(16);   /* IP0 */
    s->tmp1      = a64op_gpx(17);   /* IP1 */

    /* Read/Write data pointers. */
    LOOP(imask, i) { s->in [i] = a64op_gpx(rw_gprs[(i * 4) + 0]); }
    LOOP(omask, i) { s->out[i] = a64op_gpx(rw_gprs[(i * 4) + 1]); }
}

static void asmgen_process_frame(SwsAArch64Context *s, SwsCompMask imask, SwsCompMask omask)
{
    asmgen_common_frame(s, imask, omask);

    /* SwsOpFunc arguments. */
    s->exec      = a64op_gpx(0);    // const SwsOpExec *exec
    s->impl      = a64op_gpx(1);    // const void *priv
    s->bx_start  = a64op_gpw(2);    // int bx_start
    s->y_start   = a64op_gpw(3);    // int y_start
    s->bx_end    = a64op_gpw(4);    // int bx_end
    s->y_end     = a64op_gpw(5);    // int y_end

    /* CPS-related variables. */
    s->op0_func  = a64op_gpx(7);
    s->op0_impl  = a64op_gpx(8);

    /* Read/Write data pointer padding. */
    LOOP(imask, i) { s->in_bump [i] = a64op_gpx(rw_gprs[(i * 4) + 2]); }
    LOOP(omask, i) { s->out_bump[i] = a64op_gpx(rw_gprs[(i * 4) + 3]); }
}

static void asmgen_op_frame(SwsAArch64Context *s, SwsCompMask imask, SwsCompMask omask)
{
    asmgen_common_frame(s, imask, omask);

    /* CPS-related variables. */
    s->cont      = a64op_gpx(0);    /* Reused from SwsOpFunc.exec argument. */
    s->impl      = a64op_gpx(1);    /* Same as SwsOpFunc.impl argument. */
}

/*********************************************************************/
/* Vector register assignment. */
static void init_vectors_cps(SwsAArch64Context *s, SwsAArch64OpRegs *regs)
{
    regs->sl[ 0] = a64op_vec( 0);
    regs->sl[ 1] = a64op_vec( 1);
    regs->sl[ 2] = a64op_vec( 2);
    regs->sl[ 3] = a64op_vec( 3);
    regs->sh[ 0] = a64op_vec( 4);
    regs->sh[ 1] = a64op_vec( 5);
    regs->sh[ 2] = a64op_vec( 6);
    regs->sh[ 3] = a64op_vec( 7);
    regs->dl[ 0] = a64op_vec( 0);
    regs->dl[ 1] = a64op_vec( 1);
    regs->dl[ 2] = a64op_vec( 2);
    regs->dl[ 3] = a64op_vec( 3);
    regs->dh[ 0] = a64op_vec( 4);
    regs->dh[ 1] = a64op_vec( 5);
    regs->dh[ 2] = a64op_vec( 6);
    regs->dh[ 3] = a64op_vec( 7);
    regs->vt[ 0] = a64op_vec(16);
    regs->vt[ 1] = a64op_vec(17);
    regs->vt[ 2] = a64op_vec(18);
    regs->vt[ 3] = a64op_vec(19);
    regs->vt[ 4] = a64op_vec(20);
    regs->vt[ 5] = a64op_vec(21);
    regs->vt[ 6] = a64op_vec(22);
    regs->vt[ 7] = a64op_vec(23);
    regs->vt[ 8] = a64op_vec(24);
    regs->vt[ 9] = a64op_vec(25);
    regs->vt[10] = a64op_vec(26);
    regs->vt[11] = a64op_vec(27);
    regs->vk[ 0] = a64op_vec(28);
    regs->vk[ 1] = a64op_vec(29);
    regs->vk[ 2] = a64op_vec(30);
    regs->vk[ 3] = a64op_vec(31);
}

/*********************************************************************/
static void asmgen_process_cps(SwsAArch64Context *s, SwsCompMask mask)
{
    RasmContext *r = s->rctx;
    char func_name[128];

    snprintf(func_name, sizeof(func_name), "ff_sws_process_%04x_neon", nibble_mask(mask));
    rasm_func_begin(r, func_name, true, false);
    asmgen_process_frame(s, mask, mask);

    s->extra_regs_count = 0;
    s->extra_regs[s->extra_regs_count++] = a64op_lr();
    s->extra_regs[s->extra_regs_count++] = a64op_vecd(8);

    asmgen_process(s, mask, mask);

    /* Load values from impl. */
    rasm_set_current_node(r, s->setup);
    RasmOp impl_func = a64op_off(s->impl, offsetof_impl_func);
    i_ldr(r, s->op0_func, impl_func);                   CMT("SwsFuncPtr op0_func = impl->func;");
    i_mov(r, s->op0_impl, s->impl);                     CMT("SwsOpImpl *op0_impl = impl;");

    /* Reset impl and call first kernel. */
    rasm_set_current_node(r, s->loop);
    i_mov(r, s->impl, s->op0_impl);                     CMT("impl = op0_impl;");
    i_blr(r, s->op0_func);                              CMT("op0_func();");
}

/*********************************************************************/
/* SWS_UOP_RW_SHUFFLE: packed shuffle (standalone function) */

/**
 * The input and output vector registers are loaded and stored with ldp/ldr
 * (stp/str), and the pointers are incremented either with post-increment
 * on each load/store (IN_ORDER_CORE=1, best on Cortex-A55), or with offsets
 * from the same base followed by a single add (IN_ORDER_CORE=0, best on
 * Cortex-A76).
 */
#ifndef IN_ORDER_CORE
#define IN_ORDER_CORE 1
#endif

static RasmOp veclist_n(const RasmOp *v, int n)
{
    switch (n) {
    case 1: return vv_1(v[0]);
    case 2: return vv_2(v[0], v[1]);
    case 3: return vv_3(v[0], v[1], v[2]);
    case 4: return vv_4(v[0], v[1], v[2], v[3]);
    }
    av_assert0(0);
    return OPN;
}

/**
 * Store the first `rem` (1-15) bytes of vector register v exactly, in
 * pieces of 8, 4, 2, and 1 bytes. Pieces that are not at the start of the
 * register are first moved to vtmp with dup.
 */
static void shuffle_store_partial(SwsAArch64Context *s, RasmOp ptr, RasmOp v,
                                  RasmOp vtmp, int rem)
{
    RasmContext *r = s->rctx;
    int offset = 0;
    for (int size = 8; size > 0; size >>= 1) {
        if (!(rem & size))
            continue;
        RasmOp src = v;
        if (offset) {
            RasmOp el = a64op_elem(a64op_make_vec(a64op_vec_n(v), 0, size), offset / size);
            i_dup(r, a64op_make_vec(a64op_vec_n(vtmp), 16 / size, size), el);
            src = vtmp;
        }
        i_str(r, a64op_make_vec(a64op_vec_n(src), 0, size), a64op_post(ptr, size));
        offset += size;
    }
}

static void shuffle_load(SwsAArch64Context *s, RasmOp ptr, const RasmOp *v,
                         int n, int bytes)
{
    RasmContext *r = s->rctx;

#if IN_ORDER_CORE
    for (int i = 0; i < n; i += 2) {
        const int left = bytes - 16 * i; /* remaining pointer increment */
        if (i + 1 < n) {
            /* ldp post-index immediates must be multiples of 16 */
            i_ldp(r, v_q(v[i]), v_q(v[i + 1]), a64op_post(ptr, 32));
            if (i + 2 == n && left != 32)
                i_add(r, ptr, ptr, IMM(left - 32));
        } else {
            i_ldr(r, v_q(v[i]), a64op_post(ptr, left));
        }
    }
#else
    for (int i = 0; i < n; i += 2) {
        if (i + 1 < n)
            i_ldp(r, v_q(v[i]), v_q(v[i + 1]), a64op_off(ptr, 16 * i));
        else
            i_ldr(r, v_q(v[i]), a64op_off(ptr, 16 * i));
    }
    i_add(r, ptr, ptr, IMM(bytes));
#endif
}

/* Store one or two whole vector registers at `offset` from the block start. */
static void shuffle_store(SwsAArch64Context *s, RasmOp ptr, const RasmOp *v,
                          int n, int offset)
{
    RasmContext *r = s->rctx;

#if IN_ORDER_CORE
    if (n == 2)
        i_stp(r, v_q(v[0]), v_q(v[1]), a64op_post(ptr, 32));
    else
        i_str(r, v_q(v[0]), a64op_post(ptr, 16));
#else
    if (n == 2)
        i_stp(r, v_q(v[0]), v_q(v[1]), a64op_off(ptr, offset));
    else
        i_str(r, v_q(v[0]), a64op_off(ptr, offset));
#endif
}

static void asmgen_shuffle(SwsAArch64Context *s, const SwsAArch64OpEntry *entry)
{
    const SwsAArch64OpImplParams *p = &entry->params;
    const SwsShuffleUOp *par = &p->par.shuffle;
    RasmContext *r = s->rctx;

    SwsAArch64ShuffleLayout l;
    av_assert0(shuffle_layout(par->read_size, par->write_size, &l) == 0);
    const bool clear = par->clear_value;

    /**
     * Register assignment: inputs from v0 (tbl needs consecutive table
     * registers), tbl indices followed by clear values from v16 (in the
     * same order as in priv), and outputs in the remaining registers of
     * v0-v7 and v16-v31, so that d8-d15 never need to be saved.
     */
    const int num_const = l.num_idx + (clear ? l.num_clr : 0);
    av_assert0(l.num_in <= 8 && l.num_out <= 8);
    av_assert0(l.num_in + num_const + l.num_out <= 24);

    RasmOp vin[8];
    RasmOp vout[8];
    RasmOp vconst[16];
    for (int i = 0; i < l.num_in; i++)
        vin[i] = a64op_vec16b(i);
    for (int i = 0; i < num_const; i++)
        vconst[i] = a64op_vec16b(16 + i);
    for (int i = 0, n = l.num_in; i < l.num_out; i++, n++) {
        if (n == 8)
            n = 16 + num_const;
        vout[i] = a64op_vec16b(n);
    }
    const RasmOp *vidx = vconst;
    const RasmOp *vclr = vconst + l.num_idx;

    rasm_func_begin(r, entry->name, true, false);
    asmgen_process_frame(s, SWS_COMP_ELEMS(1), SWS_COMP_ELEMS(1));

    s->extra_regs_count = 0;
    asmgen_process(s, SWS_COMP_ELEMS(1), SWS_COMP_ELEMS(1));

    /* Load tbl indices and clear values from priv. */
    rasm_set_current_node(r, s->setup);
    load_regs(s, num_const, 16,
              "vconst", vconst,
              "priv", s->impl, 0);

    /* Shuffle one block. */
    rasm_set_current_node(r, s->loop);
    shuffle_load(s, s->in[0], vin, l.num_in, l.in_bytes);
    CMTF("read %d bytes", l.in_bytes);

    /**
     * Store each pair of output vector registers right after computing it.
     * Storing all of them at the end of the iteration can drop Cortex-A55
     * out of write streaming mode, depending on the output alignment.
     */
    const int full = l.out_bytes >> 4;
    RasmOp ptr = s->out[0];
    for (int k = 0; k < l.num_out; k += 2) {
        const int n = FFMIN(2, l.num_out - k);
        for (int j = k; j < k + n; j++) {
            const int nr = l.hi[j] - l.lo[j] + 1;
            i_tbl(r, vout[j], veclist_n(&vin[l.lo[j]], nr), vidx[l.idx[j]]);
            CMTF("vout[%d] = tbl(vin[%d..%d], vidx[%d]);", j, l.lo[j], l.hi[j], l.idx[j]);
        }
        for (int j = k; clear && j < k + n; j++) {
            i_orr(r, vout[j], vout[j], vclr[l.clr[j]]);
            CMTF("vout[%d] |= vclr[%d];", j, l.clr[j]);
        }

        const int num_full = FFMIN(n, full - k);
        if (num_full)
            shuffle_store(s, ptr, &vout[k], num_full, 16 * k);
        if (num_full < n) {
            /**
             * Never store past the end of the output of this iteration.
             * All tbl instructions have been emitted, so an input vector
             * register can be used as a temporary.
             */
#if !IN_ORDER_CORE
            i_add(r, ptr, ptr, IMM(16 * full));
#endif
            shuffle_store_partial(s, ptr, vout[full], vin[0], l.out_bytes & 15);
        }
    }
#if !IN_ORDER_CORE
    if (!(l.out_bytes & 15))
        i_add(r, ptr, ptr, IMM(l.out_bytes));
#endif
    CMTF("write %d bytes", l.out_bytes);
}

/*********************************************************************/
static void asmgen_op_cps(SwsAArch64Context *s, const SwsAArch64OpEntry *entry)
{
    const SwsAArch64OpImplParams *p = &entry->params;
    RasmContext *r = s->rctx;

    bool is_read = false;
    bool is_write = false;
    switch (p->uop) {
    case SWS_UOP_READ_BIT:
    case SWS_UOP_READ_NIBBLE:
    case SWS_UOP_READ_PACKED:
    case SWS_UOP_READ_PLANAR:
        is_read = true;
        break;
    case SWS_UOP_WRITE_BIT:
    case SWS_UOP_WRITE_NIBBLE:
    case SWS_UOP_WRITE_PACKED:
    case SWS_UOP_WRITE_PLANAR:
        is_write = true;
        break;
    default:
        break;
    }

    rasm_func_begin(r, entry->name, true, !is_read);
    asmgen_op_frame(s, is_read ? p->mask : 0, is_write ? p->mask : 0);

    /**
     * Set up vector register dimensions and reshape all vectors
     * accordingly.
     */
    size_t el_size = ff_sws_pixel_type_size(p->type);
    size_t total_size = p->block_size * el_size;

    s->vec_size = FFMIN(total_size, 16);
    s->use_vh = (s->vec_size != total_size);

    s->el_size = el_size;
    s->el_count = s->vec_size / el_size;
    init_vectors_cps(s, &s->regs);
    reshape_io_vectors(&s->regs, s->el_count, el_size);
    reshape_temp_vectors(&s->regs, s->el_count, el_size);
    reshape_const_vectors(&s->regs, s->el_count, el_size);

    /* Common start for continuation-passing style (CPS) functions. */
    asmgen_set_load_cont_node(s);

    /* Set up constants. */
    switch (p->uop) {
    case SWS_UOP_READ_BIT:     asmgen_setup_read_bit(s, p, &s->regs);     break;
    case SWS_UOP_READ_NIBBLE:  asmgen_setup_read_nibble(s, p, &s->regs);  break;
    case SWS_UOP_WRITE_BIT:    asmgen_setup_write_bit(s, p, &s->regs);    break;
    case SWS_UOP_UNPACK:       asmgen_setup_unpack(s, p, &s->regs);       break;
    case SWS_UOP_CLEAR:        asmgen_setup_clear(s, p, &s->regs);        break;
    case SWS_UOP_ADD:          asmgen_setup_add(s, p, &s->regs);          break;
    case SWS_UOP_MIN:          asmgen_setup_min(s, p, &s->regs);          break;
    case SWS_UOP_MAX:          asmgen_setup_max(s, p, &s->regs);          break;
    case SWS_UOP_MUL:          asmgen_setup_mul(s, p, &s->regs);          break;
    case SWS_UOP_LINEAR:       asmgen_setup_linear(s, p, &s->regs);       break;
    case SWS_UOP_LINEAR_FMA:   asmgen_setup_linear(s, p, &s->regs);       break;
    case SWS_UOP_DITHER:       asmgen_setup_dither(s, p, &s->regs);       break;
    default:
        break;
    }

    /* Emit uop kernel. */
    switch (p->uop) {
    case SWS_UOP_READ_BIT:     asmgen_op_read_bit(s, p, &s->regs);     break;
    case SWS_UOP_READ_NIBBLE:  asmgen_op_read_nibble(s, p, &s->regs);  break;
    case SWS_UOP_READ_PACKED:  asmgen_op_read_packed(s, p, &s->regs);  break;
    case SWS_UOP_READ_PLANAR:  asmgen_op_read_planar(s, p, &s->regs);  break;
    case SWS_UOP_WRITE_BIT:    asmgen_op_write_bit(s, p, &s->regs);    break;
    case SWS_UOP_WRITE_NIBBLE: asmgen_op_write_nibble(s, p, &s->regs); break;
    case SWS_UOP_WRITE_PACKED: asmgen_op_write_packed(s, p, &s->regs); break;
    case SWS_UOP_WRITE_PLANAR: asmgen_op_write_planar(s, p, &s->regs); break;
    case SWS_UOP_SWAP_BYTES:   asmgen_op_swap_bytes(s, p, &s->regs);   break;
    case SWS_UOP_PERMUTE:      asmgen_op_move(s, p, &s->regs);         break;
    case SWS_UOP_COPY:         asmgen_op_move(s, p, &s->regs);         break;
    case SWS_UOP_UNPACK:       asmgen_op_unpack(s, p, &s->regs);       break;
    case SWS_UOP_PACK:         asmgen_op_pack(s, p, &s->regs);         break;
    case SWS_UOP_LSHIFT:       asmgen_op_lshift(s, p, &s->regs);       break;
    case SWS_UOP_RSHIFT:       asmgen_op_rshift(s, p, &s->regs);       break;
    case SWS_UOP_CLEAR:        asmgen_op_clear(s, p, &s->regs);        break;
    case SWS_UOP_TO_U8:        asmgen_op_convert(s, p, &s->regs);      break;
    case SWS_UOP_TO_U16:       asmgen_op_convert(s, p, &s->regs);      break;
    case SWS_UOP_TO_U32:       asmgen_op_convert(s, p, &s->regs);      break;
    case SWS_UOP_TO_F32:       asmgen_op_convert(s, p, &s->regs);      break;
    case SWS_UOP_EXPAND_PAIR:  asmgen_op_expand(s, p, &s->regs);       break;
    case SWS_UOP_EXPAND_QUAD:  asmgen_op_expand(s, p, &s->regs);       break;
    case SWS_UOP_ADD:          asmgen_op_add(s, p, &s->regs);          break;
    case SWS_UOP_MIN:          asmgen_op_min(s, p, &s->regs);          break;
    case SWS_UOP_MAX:          asmgen_op_max(s, p, &s->regs);          break;
    case SWS_UOP_MUL:          asmgen_op_mul(s, p, &s->regs);          break;
    case SWS_UOP_LINEAR:       asmgen_op_linear(s, p, &s->regs);       break;
    case SWS_UOP_LINEAR_FMA:   asmgen_op_linear(s, p, &s->regs);       break;
    case SWS_UOP_DITHER:       asmgen_op_dither(s, p, &s->regs);       break;
    default:
        break;
    }

    if (is_write) {
        /* Write functions return directly. */
        i_ret(r);
    } else {
        /* Load continuation address and increment impl pointer. */
        const size_t priv_size = FFALIGN(aarch64_priv_size(p), AARCH64_IMPL_DATA_ALIGN);
        RasmNode *node = rasm_set_current_node(r, s->load_cont_node);
        RasmOp impl_pre = a64op_pre(s->impl, AARCH64_IMPL_DATA_OFFSET + priv_size);
        i_ldr(r, s->cont, impl_pre);                    CMT("SwsFuncPtr cont = (++impl)->func;");
        rasm_set_current_node(r, node);
        /* Common end for remaining CPS functions. */
        i_br (r, s->cont);                              CMT("jump to cont");
    }
}

/*********************************************************************/

/* Generate all functions described by ops_entries.c */
static int asmgen(void)
{
    RasmContext *rctx = rasm_alloc();
    if (!rctx)
        return AVERROR(ENOMEM);

    SwsAArch64Context s = { .rctx = rctx };
    AVBPrint bp;
    int ret;

    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);

    /* Generate all process functions using rasm. */
    asmgen_process_cps(&s, SWS_COMP_ELEMS(1));
    asmgen_process_cps(&s, SWS_COMP_ELEMS(2));
    asmgen_process_cps(&s, SWS_COMP_ELEMS(3));
    asmgen_process_cps(&s, SWS_COMP_ELEMS(4));

    /* Generate all functions from ops_entries.c using rasm. */
    const SwsAArch64OpEntry *entries = ops_entries;
    while (entries->name) {
        if (entries->params.uop == SWS_UOP_RW_SHUFFLE)
            asmgen_shuffle(&s, entries++);
        else
            asmgen_op_cps(&s, entries++);
        if (rctx->error) {
            ret = rctx->error;
            goto error;
        }
    }

    /* Print all rasm functions to stdout. */
    printf("#include \"libavutil/aarch64/asm.S\"\n");
    printf("\n");
    ret = rasm_print(s.rctx, &bp);
    if (ret < 0)
        goto error;
    fputs(bp.str, stdout);

error:
    av_bprint_finalize(&bp, NULL);
    rasm_free(&s.rctx);
    return ret;
}

/*********************************************************************/
int main(int argc, char *argv[])
{
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    return asmgen();
}
