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

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

/*********************************************************************/
#define AVUTIL_LOG_H
#define AVUTIL_MEM_H
#define av_malloc(s)     malloc(s)
#define av_mallocz(s)    calloc(1, s)
#define av_realloc(p, s) realloc(p, s)
#define av_strdup(s)     strdup(s)
#define av_free(p)       free(p)

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

/*********************************************************************/
#include "rasm.c"
#include "rasm_print.c"
#include "ops_impl.c"

/*********************************************************************/
typedef struct SwsAArch64Context {
    AArch64Context *actx;

    AArch64Op exec;
    AArch64Op impl;
    AArch64Op bx_start;
    AArch64Op y_start;
    AArch64Op bx_end;
    AArch64Op y_end;

    AArch64Op bx;
    AArch64Op y;

    AArch64Op tmp0;
    AArch64Op tmp1;
    AArch64Op op0_func;
    AArch64Op op1_impl;
    AArch64Op cont;

    AArch64Op vl[4];
    AArch64Op vh[4];
    AArch64Op vt[8];

    AArch64Op in[4];
    AArch64Op out[4];
    AArch64Op in_padding[4];
    AArch64Op out_padding[4];

    size_t el_size;
    size_t el_count;
    size_t vec_size;
    bool use_vh;
} SwsAArch64Context;

/*********************************************************************/
#define LOOP_MASK_VH(s, p, idx) if (s->use_vh) LOOP_MASK(p, idx)
#define LOOP_MASK_BWD_VH(s, p, idx) if (s->use_vh) LOOP_MASK_BWD(p, idx)

/*********************************************************************/
static void reshape_all_vectors(SwsAArch64Context *s, int el_count, int el_size)
{
    s->vl[0] = a64op_make_vec( 0, el_count, el_size);
    s->vl[1] = a64op_make_vec( 1, el_count, el_size);
    s->vl[2] = a64op_make_vec( 2, el_count, el_size);
    s->vl[3] = a64op_make_vec( 3, el_count, el_size);
    s->vh[0] = a64op_make_vec( 4, el_count, el_size);
    s->vh[1] = a64op_make_vec( 5, el_count, el_size);
    s->vh[2] = a64op_make_vec( 6, el_count, el_size);
    s->vh[3] = a64op_make_vec( 7, el_count, el_size);
    s->vt[0] = a64op_make_vec(16, el_count, el_size);
    s->vt[1] = a64op_make_vec(17, el_count, el_size);
    s->vt[2] = a64op_make_vec(18, el_count, el_size);
    s->vt[3] = a64op_make_vec(19, el_count, el_size);
    s->vt[4] = a64op_make_vec(20, el_count, el_size);
    s->vt[5] = a64op_make_vec(21, el_count, el_size);
    s->vt[6] = a64op_make_vec(22, el_count, el_size);
    s->vt[7] = a64op_make_vec(23, el_count, el_size);
}

/*********************************************************************/
static const SwsAArch64OpImplParams impl_params[] = {
#include "ops_entries.c"
    { .op = AARCH64_SWS_OP_NONE }
};

/*********************************************************************/
/* Save registers x19-x28, along with x29 (fp) and x30 (lr). */
#define MAX_SAVED_REGS 12

static unsigned clobbered_frame_size(unsigned n)
{
    return ((n + 1) >> 1) * 16;
}

static void asmgen_prologue(SwsAArch64Context *s, const AArch64Op *regs, unsigned n)
{
    AArch64Context *a = s->actx;
    AArch64Op sp = a64op_sp();
    unsigned frame_size = clobbered_frame_size(n);
    AArch64Op sp_pre = a64op_pre(sp, -frame_size);

    if (n == 0) {
        /* no-op */
    } else if (n == 1) {
        i_str(a, regs[0], sp_pre);
    } else {
        i_stp(a, regs[0], regs[1], sp_pre);
        for (unsigned i = 2; i + 1 < n; i += 2)
            i_stp(a, regs[i], regs[i + 1], a64op_off(sp, i * sizeof(uint64_t)));
        if (n & 1)
            i_str(a, regs[n - 1], a64op_off(sp, (n - 1) * sizeof(uint64_t)));
    }
}

static void asmgen_epilogue(SwsAArch64Context *s, const AArch64Op *regs, unsigned n)
{
    AArch64Context *a = s->actx;
    AArch64Op sp = a64op_sp();
    unsigned frame_size = clobbered_frame_size(n);
    AArch64Op sp_post = a64op_post(sp, frame_size);

    if (n == 0) {
        /* no-op */
    } else if (n == 1) {
        i_ldr(a, regs[0], sp_post);
    } else {
        if (n & 1)
            i_ldr(a, regs[n - 1], a64op_off(sp, (n - 1) * sizeof(uint64_t)));
        for (unsigned i = (n & ~1u) - 2; i >= 2; i -= 2)
            i_ldp(a, regs[i], regs[i + 1], a64op_off(sp, i * sizeof(uint64_t)));
        i_ldp(a, regs[0], regs[1], sp_post);
    }
}

/*********************************************************************/
static unsigned clobbered_saved_gprs(const SwsAArch64Context *s,
                                     const SwsAArch64OpImplParams *p,
                                     AArch64Op regs[MAX_SAVED_REGS])
{
    unsigned n = 0;
    regs[n++] = s->op1_impl;
    LOOP_MASK(p, i) {
        regs[n++] = s->in_padding[i];
        regs[n++] = s->out_padding[i];
    }
    return n;
}

static void asmgen_process(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;

    char func_name[256];
    sws_aarch64_op_impl_func_name(func_name, sizeof(func_name), p);

    aarch64_func_begin(a, func_name, true);

    /* Function prologue */
    AArch64Op saved_regs[MAX_SAVED_REGS];
    unsigned nsaved = clobbered_saved_gprs(s, p, saved_regs);
    if (nsaved) {
        aarch64_add_comment(a, "prologue");
        asmgen_prologue(s, saved_regs, nsaved);
    }

    i_ldr(a, s->op0_func, a64op_off(s->impl, offsetof_impl_cont));  inlcmt(a, "SwsFuncPtr op0_func = impl->cont;");
    i_add(a, s->op1_impl, s->impl, a64op_imm(sizeof_impl));         inlcmt(a, "SwsOpImpl *op1_impl = impl + 1;");

    aarch64_add_comment(a, "exec->in");
    LOOP_MASK(p, i) i_ldr(a, s->in[i],          a64op_off(s->exec, offsetof_exec_in       + (i * sizeof(uint8_t *))));
    aarch64_add_comment(a, "exec->out");
    LOOP_MASK(p, i) i_ldr(a, s->out[i],         a64op_off(s->exec, offsetof_exec_out      + (i * sizeof(uint8_t *))));
    aarch64_add_comment(a, "exec->in_bump");
    LOOP_MASK(p, i) i_ldr(a, s->in_padding[i],  a64op_off(s->exec, offsetof_exec_in_bump  + (i * sizeof(ptrdiff_t))));
    aarch64_add_comment(a, "exec->out_bump");
    LOOP_MASK(p, i) i_ldr(a, s->out_padding[i], a64op_off(s->exec, offsetof_exec_out_bump + (i * sizeof(ptrdiff_t))));

    i_mov(a, s->bx, s->bx_start);   inlcmt(a, "bx = bx_start;");
    i_mov(a, s->impl, s->op1_impl); inlcmt(a, "impl = op1_impl;");
    i_br (a, s->op0_func);          inlcmt(a, "jump to op0_func");
}

static void asmgen_process_return(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;

    char func_name[256];
    sws_aarch64_op_impl_func_name(func_name, sizeof(func_name), p);

    aarch64_func_begin(a, func_name, true);

    int loop = aarch64_new_label(a, NULL);
    int end  = aarch64_new_label(a, NULL);

    i_mov(a, s->impl, s->op1_impl); inlcmt(a, "impl = op1_impl;");

    aarch64_add_comment(a, "horizontal loop");
    i_add(a, s->bx, s->bx, a64op_imm(1));
    i_cmp(a, s->bx, s->bx_end);
    i_bne(a, loop);

    aarch64_add_comment(a, "vertical loop");
    i_add(a, s->y, s->y, a64op_imm(1));
    i_cmp(a, s->y, s->y_end);
    i_beq(a, end);

    aarch64_add_comment(a, "padding");
    LOOP_MASK(p, i) i_add(a, s->in[i],  s->in[i],  s->in_padding[i]);
    LOOP_MASK(p, i) i_add(a, s->out[i], s->out[i], s->out_padding[i]);

    aarch64_add_comment(a, "bx = bx_start;");
    i_mov(a, s->bx, s->bx_start);

    aarch64_add_label(a, loop);
    i_br (a, s->op0_func);  inlcmt(a, "jump to op0_func");

    aarch64_add_label(a, end);

    /* Function epilogue */
    AArch64Op saved_regs[MAX_SAVED_REGS];
    unsigned nsaved = clobbered_saved_gprs(s, p, saved_regs);
    if (nsaved) {
        aarch64_add_comment(a, "epilogue");
        asmgen_epilogue(s, saved_regs, nsaved);
    }

    i_ret(a);
}

/*********************************************************************/
/* gather raw pixels from planes */
/* AARCH64_SWS_OP_READ_BIT */
/* AARCH64_SWS_OP_READ_NIBBLE */
/* AARCH64_SWS_OP_READ_PACKED */
/* AARCH64_SWS_OP_READ_PLANAR */

static void asmgen_op_read_bit(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    AArch64Op *vl = s->vl;
    AArch64Op shift_vec   = s->vt[0];
    AArch64Op bitmask_vec = s->vt[1];
    AArch64Op vtmp        = s->vt[2];
    AArch64Op wtmp        = a64op_w(s->tmp0);

    aarch64_annotate_next(a, "v128 shift_vec = impl->priv.v128;");
    i_ldr(a, v_q(shift_vec), a64op_off(s->impl, offsetof_impl_priv));

    if (p->block_size == 8) {
        i_ldrb(a, wtmp, a64op_post(s->in[0], 1)); inlcmt(a, "uint8_t tmp = *in[0]++;");
    } else {
        i_ldrh(a, wtmp, a64op_post(s->in[0], 2)); inlcmt(a, "uint16_t tmp = *in[0]++;");
    }

    if (p->block_size == 8)
        aarch64_annotate_next(a, "v128 bitmask_vec = {1 <repeats 8 times>, 0 <repeats 8 times>};");
    else
        aarch64_annotate_next(a, "v128 bitmask_vec = {1 <repeats 16 times>};");
    i_movi(a, bitmask_vec, a64op_imm(1));

    i_dup (a, v_8b(vl[0]), wtmp);                       inlcmt(a, "vl[0].lo = broadcast(tmp);");
    if (p->block_size == 16) {
        i_lsr(a,      wtmp,      wtmp, a64op_imm(8));   inlcmt(a, "tmp >>= 8;");
        i_dup(a, v_8b(vtmp),     wtmp);                 inlcmt(a, "vtmp.lo = broadcast(tmp);");
        i_ins(a, ve_d(vl[0], 1), ve_d(vtmp, 0));        inlcmt(a, "vl[0].hi = vtmp.lo;");
    }
    i_ushl(a, vl[0], vl[0], shift_vec);                 inlcmt(a, "vl[0] >>= shift_vec;");
    i_and (a, vl[0], vl[0], bitmask_vec);               inlcmt(a, "vl[0] &= bitmask_vec;");
}

static void asmgen_op_read_nibble(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    AArch64Op *vl = s->vl;
    AArch64Op nibble_mask = s->vt[0];
    AArch64Op vtmp        = s->vt[1];

    aarch64_annotate_next(a, "v128 nibble_mask = {0xf <repeats 8 times>, 0x0 <repeats 8 times>};");
    i_movi(a, v_8b(nibble_mask), a64op_imm(0x0f));

    aarch64_annotate_next(a, "vl[0] = *in[0]++;");
    if (p->block_size == 8)
        i_ldr (a, v_s(vl[0]), a64op_post(s->in[0], p->block_size / 2));
    else
        i_ldr (a, v_d(vl[0]), a64op_post(s->in[0], p->block_size / 2));
    i_ushr(a, v_8b(vtmp),  v_8b(vl[0]), a64op_imm(4));      inlcmt(a, "vtmp.lo = vl[0] >> 4;");
    i_and (a, v_8b(vl[0]), v_8b(vl[0]), v_8b(nibble_mask)); inlcmt(a, "vl[0].lo &= nibble_mask;");
    i_zip1(a,      vl[0],       vtmp,   vl[0]);             inlcmt(a, "interleave");
}

static void asmgen_op_read_packed(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    AArch64Op *vl = s->vl;
    AArch64Op *vh = s->vh;

    if (p->mask == 0x0001) {
        switch ((s->use_vh ? 0x100 : 0) | s->vec_size) {
        case 0x008: i_ldr(a, v_d(vl[0]),             a64op_post(s->in[0], s->vec_size * 1)); break;
        case 0x010: i_ldr(a, v_q(vl[0]),             a64op_post(s->in[0], s->vec_size * 1)); break;
        case 0x108: i_ldp(a, v_d(vl[0]), v_d(vh[0]), a64op_post(s->in[0], s->vec_size * 2)); break;
        case 0x110: i_ldp(a, v_q(vl[0]), v_q(vh[0]), a64op_post(s->in[0], s->vec_size * 2)); break;
        }
    } else {
        switch (p->mask) {
        case 0x0011: i_ld2(a, vv_2(vl[0], vl[1]),               a64op_post(s->in[0], s->vec_size * 2)); break;
        case 0x0111: i_ld3(a, vv_3(vl[0], vl[1], vl[2]),        a64op_post(s->in[0], s->vec_size * 3)); break;
        case 0x1111: i_ld4(a, vv_4(vl[0], vl[1], vl[2], vl[3]), a64op_post(s->in[0], s->vec_size * 4)); break;
        }
        if (s->use_vh) {
            switch (p->mask) {
            case 0x0011: i_ld2(a, vv_2(vh[0], vh[1]),               a64op_post(s->in[0], s->vec_size * 2)); break;
            case 0x0111: i_ld3(a, vv_3(vh[0], vh[1], vh[2]),        a64op_post(s->in[0], s->vec_size * 3)); break;
            case 0x1111: i_ld4(a, vv_4(vh[0], vh[1], vh[2], vh[3]), a64op_post(s->in[0], s->vec_size * 4)); break;
            }
        }
    }
}

static void asmgen_op_read_planar(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    AArch64Op *vl = s->vl;
    AArch64Op *vh = s->vh;

    LOOP_MASK(p, i) {
        switch ((s->use_vh ? 0x100 : 0) | s->vec_size) {
        case 0x008: i_ldr(a, v_d(vl[i]),             a64op_post(s->in[i], s->vec_size * 1)); break;
        case 0x010: i_ldr(a, v_q(vl[i]),             a64op_post(s->in[i], s->vec_size * 1)); break;
        case 0x108: i_ldp(a, v_d(vl[i]), v_d(vh[i]), a64op_post(s->in[i], s->vec_size * 2)); break;
        case 0x110: i_ldp(a, v_q(vl[i]), v_q(vh[i]), a64op_post(s->in[i], s->vec_size * 2)); break;
        }
    }
}

/*********************************************************************/
/* write raw pixels to planes */
/* AARCH64_SWS_OP_WRITE_BIT */
/* AARCH64_SWS_OP_WRITE_NIBBLE */
/* AARCH64_SWS_OP_WRITE_PACKED */
/* AARCH64_SWS_OP_WRITE_PLANAR */

static void asmgen_op_write_bit(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    AArch64Op *vl = s->vl;
    AArch64Op shift_vec = s->vt[0];
    AArch64Op vtmp0     = s->vt[1];
    AArch64Op vtmp1     = s->vt[2];

    aarch64_annotate_next(a, "v128 shift_vec = impl->priv.v128;");
    i_ldr(a, v_q(shift_vec), a64op_off(s->impl, offsetof_impl_priv));

    i_ushl(a, vl[0],      vl[0], shift_vec);                    inlcmt(a, "vl[0] <<= shift_vec;");
    i_addv(a, v_b(vtmp0), v_8b(vl[0]));                         inlcmt(a, "vtmp0[0] = add_accross(vl[0].lo);");
    if (s->vec_size == 8) {
        i_str (a, v_b (vtmp0),    a64op_post(s->out[0], 1));    inlcmt(a, "*out[0]++ = vtmp0;");
    } else {
        i_ins (a, ve_d(vtmp1, 0), ve_d(vl[0], 1));              inlcmt(a, "vtmp1.lo = vtmp0.hi;");
        i_addv(a, v_b (vtmp1),    v_8b(vtmp1));                 inlcmt(a, "vtmp1[0] = add_accross(vtmp1);");
        i_ins (a, ve_b(vtmp0, 1), ve_b(vtmp1, 0));              inlcmt(a, "vtmp0[1] = vtmp1[0];");
        i_str (a, v_h (vtmp0),    a64op_post(s->out[0], 2));    inlcmt(a, "*out[0]++ = vtmp0;");
    }
}

static void asmgen_op_write_nibble(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    AArch64Op *vl = s->vl;
    AArch64Op vtmp0 = s->vt[0];
    AArch64Op vtmp1 = s->vt[1];

    if (s->vec_size == 8) {
        i_shl (a, v_4h(vtmp0), v_4h(vl[0]), a64op_imm(4));
        i_ushr(a, v_4h(vtmp1), v_4h(vl[0]), a64op_imm(8));
        i_orr (a, vl[0], vtmp0, vtmp1);
        i_xtn (a, vtmp0, v_8h(vl[0]));
        i_str (a, v_s(vtmp0), a64op_post(s->out[0], 4));
    } else {
        i_shl (a, v_8h(vtmp0), v_8h(vl[0]), a64op_imm(4));
        i_ushr(a, v_8h(vtmp1), v_8h(vl[0]), a64op_imm(8));
        i_orr (a, vl[0], vtmp0, vtmp1);
        i_xtn (a, v_8b(vtmp0), v_8h(vl[0]));
        i_str (a, v_d(vtmp0), a64op_post(s->out[0], 8));
    }
}

static void asmgen_op_write_packed(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    AArch64Op *vl = s->vl;
    AArch64Op *vh = s->vh;

    if (p->mask == 0x0001) {
        switch ((s->use_vh ? 0x100 : 0) | s->vec_size) {
        case 0x008: i_str(a, v_d(vl[0]),             a64op_post(s->out[0], s->vec_size * 1)); break;
        case 0x010: i_str(a, v_q(vl[0]),             a64op_post(s->out[0], s->vec_size * 1)); break;
        case 0x108: i_stp(a, v_d(vl[0]), v_d(vh[0]), a64op_post(s->out[0], s->vec_size * 2)); break;
        case 0x110: i_stp(a, v_q(vl[0]), v_q(vh[0]), a64op_post(s->out[0], s->vec_size * 2)); break;
        }
    } else {
        switch (p->mask) {
        case 0x0011: i_st2(a, vv_2(vl[0], vl[1]),               a64op_post(s->out[0], s->vec_size * 2)); break;
        case 0x0111: i_st3(a, vv_3(vl[0], vl[1], vl[2]),        a64op_post(s->out[0], s->vec_size * 3)); break;
        case 0x1111: i_st4(a, vv_4(vl[0], vl[1], vl[2], vl[3]), a64op_post(s->out[0], s->vec_size * 4)); break;
        }
        if (s->use_vh) {
            switch (p->mask) {
            case 0x0011: i_st2(a, vv_2(vh[0], vh[1]),               a64op_post(s->out[0], s->vec_size * 2)); break;
            case 0x0111: i_st3(a, vv_3(vh[0], vh[1], vh[2]),        a64op_post(s->out[0], s->vec_size * 3)); break;
            case 0x1111: i_st4(a, vv_4(vh[0], vh[1], vh[2], vh[3]), a64op_post(s->out[0], s->vec_size * 4)); break;
            }
        }
    }
}

static void asmgen_op_write_planar(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    AArch64Op *vl = s->vl;
    AArch64Op *vh = s->vh;

    LOOP_MASK(p, i) {
        switch ((s->use_vh ? 0x100 : 0) | s->vec_size) {
        case 0x008: i_str(a, v_d(vl[i]),             a64op_post(s->out[i], s->vec_size * 1)); break;
        case 0x010: i_str(a, v_q(vl[i]),             a64op_post(s->out[i], s->vec_size * 1)); break;
        case 0x108: i_stp(a, v_d(vl[i]), v_d(vh[i]), a64op_post(s->out[i], s->vec_size * 2)); break;
        case 0x110: i_stp(a, v_q(vl[i]), v_q(vh[i]), a64op_post(s->out[i], s->vec_size * 2)); break;
        }
    }
}

/*********************************************************************/
/* swap byte order (for differing endianness) */
/* AARCH64_SWS_OP_SWAP_BYTES */

static void asmgen_op_swap_bytes(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    AArch64Op *vl = s->vl;
    AArch64Op *vh = s->vh;

    switch (sws_aarch64_pixel_size(p->type)) {
    case sizeof(uint16_t):
        LOOP_MASK      (p, i) i_rev16(a, v_16b(vl[i]), v_16b(vl[i]));
        LOOP_MASK_VH(s, p, i) i_rev16(a, v_16b(vh[i]), v_16b(vh[i]));
        break;
    case sizeof(uint32_t):
        LOOP_MASK      (p, i) i_rev32(a, v_16b(vl[i]), v_16b(vl[i]));
        LOOP_MASK_VH(s, p, i) i_rev32(a, v_16b(vh[i]), v_16b(vh[i]));
        break;
    }
}

/*********************************************************************/
/* rearrange channel order, or duplicate channels */
/* AARCH64_SWS_OP_SWIZZLE */

#define SWIZZLE_TMP 0xf

static const char *print_swizzle_v(char buf[8], uint8_t n, bool vh)
{
    if (n == SWIZZLE_TMP)
        snprintf(buf, sizeof(char[8]), "vtmp%c", vh ? 'h' : 'l');
    else
        snprintf(buf, sizeof(char[8]), "v%c[%u]", vh ? 'h' : 'l', n);
    return buf;
}
#define PRINT_SWIZZLE_V(n, vh) print_swizzle_v((char[8]){ 0 }, n, vh)

static void swizzle_emit(SwsAArch64Context *s, uint8_t dst, uint8_t src)
{
    AArch64Context *a = s->actx;
    AArch64Op src_op[2] = {
        (src == SWIZZLE_TMP) ? s->vt[0] : s->vl[src],
        (src == SWIZZLE_TMP) ? s->vt[1] : s->vh[src],
    };
    AArch64Op dst_op[2] = {
        (dst == SWIZZLE_TMP) ? s->vt[0] : s->vl[dst],
        (dst == SWIZZLE_TMP) ? s->vt[1] : s->vh[dst],
    };
    char buf[32];
    aarch64_annotate_nextf(a, buf, sizeof(buf), "%s = %s;",
                           PRINT_SWIZZLE_V(dst, false),
                           PRINT_SWIZZLE_V(src, false));
    i_mov(a, dst_op[0], src_op[0]);
    if (s->use_vh) {
        aarch64_annotate_nextf(a, buf, sizeof(buf), "%s = %s;",
                               PRINT_SWIZZLE_V(dst, true),
                               PRINT_SWIZZLE_V(src, true));
        i_mov(a, dst_op[1], src_op[1]);
    }
}

static void asmgen_op_swizzle(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    /* Compute used vectors (src and dst) */
    uint8_t src_used[4] = { 0 };
    bool done[4] = { true, true, true, true };
    LOOP_MASK(p, dst) {
        uint8_t src = MASK_GET(p->swizzle, dst);
        src_used[src]++;
        done[dst] = false;
    }

    /* Unobstructed copies */
    for (bool progress = true; progress; ) {
        progress = false;
        for (int dst = 0; dst < 4; dst++) {
            if (done[dst] || src_used[dst])
                continue;
            uint8_t src = MASK_GET(p->swizzle, dst);
            swizzle_emit(s, dst, src);
            src_used[src]--;
            done[dst] = true;
            progress = true;
        }
    }

    /* Swap and rotate */
    for (int dst = 0; dst < 4; dst++) {
        if (done[dst])
            continue;

        swizzle_emit(s, SWIZZLE_TMP, dst);

        uint8_t cur_dst = dst;
        uint8_t src = MASK_GET(p->swizzle, cur_dst);
        while (src != dst) {
            swizzle_emit(s, cur_dst, src);
            done[cur_dst] = true;
            cur_dst = src;
            src = MASK_GET(p->swizzle, cur_dst);
        }

        swizzle_emit(s, cur_dst, SWIZZLE_TMP);
        done[cur_dst] = true;
    }
}

#undef SWIZZLE_TMP

/*********************************************************************/
/* split tightly packed data into components */
/* AARCH64_SWS_OP_UNPACK */

static void asmgen_op_unpack(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    AArch64Op *vl = s->vl;
    AArch64Op *vh = s->vh;
    AArch64Op *vt = s->vt;
    uint32_t mask_val[4] = { 0 };
    uint8_t mask_idx[4] = { 0 };
    uint8_t cur_vt = 0;

    const int offsets[4] = {
        MASK_GET(p->pack, 3) + MASK_GET(p->pack, 2) + MASK_GET(p->pack, 1),
        MASK_GET(p->pack, 3) + MASK_GET(p->pack, 2),
        MASK_GET(p->pack, 3),
        0
    };

    aarch64_add_comment(a, "generate masks");
    LOOP_MASK(p, i) {
        uint32_t val = (1u << MASK_GET(p->pack, i)) - 1;
        for (int j = 0; j < 4; j++) {
            if (mask_val[j] == val) {
                mask_val[i] = mask_val[j];
                mask_idx[i] = mask_idx[j];
                break;
            }
        }
        if (!mask_val[i]) {
            /* All-one values in movi only work up to 8-bit, and then
             * at full 16- or 32-bit, but not for intermediate values
             * like 10. In those cases, we use mov + dup instead.
             */
            if (val <= 0xff || val == 0xffff) {
                i_movi(a, vt[cur_vt], a64op_imm(val));
            } else {
                AArch64Op mask_gpr = a64op_w(s->tmp0);
                i_mov (a, mask_gpr, a64op_imm(val));
                i_dup (a, vt[cur_vt], mask_gpr);
            }
            mask_val[i] = val;
            mask_idx[i] = cur_vt++;
        }
    }

    aarch64_add_comment(a, "shift right");
    /* Loop backwards to avoid clobbering component 0. */
    LOOP_MASK_BWD      (p, i) {
        if (offsets[i])
            i_ushr(a, vl[i], vl[0], a64op_imm(offsets[i]));
        else if (i)
            i_mov (a, v_16b(vl[i]), v_16b(vl[0]));
    }
    LOOP_MASK_BWD_VH(s, p, i) {
        if (offsets[i])
            i_ushr(a, vh[i], vh[0], a64op_imm(offsets[i]));
        else if (i)
            i_mov (a, v_16b(vh[i]), v_16b(vh[0]));
    }

    aarch64_add_comment(a, "apply masks");
    reshape_all_vectors(s, 16, 1);
    LOOP_MASK_BWD      (p, i) i_and(a, vl[i], vl[i], vt[mask_idx[i]]);
    LOOP_MASK_BWD_VH(s, p, i) i_and(a, vh[i], vh[i], vt[mask_idx[i]]);
}

/*********************************************************************/
/* compress components into tightly packed data */
/* AARCH64_SWS_OP_PACK */

static void asmgen_op_pack(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    AArch64Op *vl = s->vl;
    AArch64Op *vh = s->vh;

    const int offsets[4] = {
        MASK_GET(p->pack, 3) + MASK_GET(p->pack, 2) + MASK_GET(p->pack, 1),
        MASK_GET(p->pack, 3) + MASK_GET(p->pack, 2),
        MASK_GET(p->pack, 3),
        0
    };

    aarch64_add_comment(a, "shift left");
    LOOP_MASK      (p, i) if (offsets[i]) i_shl(a, vl[i], vl[i], a64op_imm(offsets[i]));
    LOOP_MASK_VH(s, p, i) if (offsets[i]) i_shl(a, vh[i], vh[i], a64op_imm(offsets[i]));

    aarch64_add_comment(a, "combine");
    reshape_all_vectors(s, 16, 1);
    LOOP_MASK      (p, i) {
        if (i != 0) {
            i_orr    (a, vl[0], vl[0], vl[i]);
            if (s->use_vh)
                i_orr(a, vh[0], vh[0], vh[i]);
        }
    }
}

/*********************************************************************/
/* logical left shift of raw pixel values by (u8) */
/* AARCH64_SWS_OP_LSHIFT */

static void asmgen_op_lshift(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    AArch64Op *vl = s->vl;
    AArch64Op *vh = s->vh;

    LOOP_MASK      (p, i) { i_shl(a, vl[i], vl[i], a64op_imm(p->shift)); inlcmtf(a, "vl[%u] <<= %u;", i, p->shift); }
    LOOP_MASK_VH(s, p, i) { i_shl(a, vh[i], vh[i], a64op_imm(p->shift)); inlcmtf(a, "vh[%u] <<= %u;", i, p->shift); }
}

/*********************************************************************/
/* right shift of raw pixel values by (u8) */
/* AARCH64_SWS_OP_RSHIFT */

static void asmgen_op_rshift(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    AArch64Op *vl = s->vl;
    AArch64Op *vh = s->vh;

    LOOP_MASK      (p, i) { i_ushr(a, vl[i], vl[i], a64op_imm(p->shift)); inlcmtf(a, "vl[%u] >>= %u;", i, p->shift); }
    LOOP_MASK_VH(s, p, i) { i_ushr(a, vh[i], vh[i], a64op_imm(p->shift)); inlcmtf(a, "vh[%u] >>= %u;", i, p->shift); }
}

/*********************************************************************/
/* clear pixel values */
/* AARCH64_SWS_OP_CLEAR */

static void asmgen_op_clear(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    AArch64Op *vl = s->vl;
    AArch64Op *vh = s->vh;
    AArch64Op clear_vec = a64op_make_vec(a64op_vec_n(s->vt[0]), 0, s->el_size);

    aarch64_annotate_next(a, "v128 clear_vec = impl->priv.v128;");
    i_ldr(a, v_q(clear_vec), a64op_off(s->impl, offsetof_impl_priv));

    LOOP_MASK      (p, i) { i_dup(a, vl[i], a64op_elem(clear_vec, i)); inlcmtf(a, "vl[%u] = broadcast(clear_vec[%u])", i, i); }
    LOOP_MASK_VH(s, p, i) { i_dup(a, vh[i], a64op_elem(clear_vec, i)); inlcmtf(a, "vh[%u] = broadcast(clear_vec[%u])", i, i); }
}

/*********************************************************************/
/* convert (cast) between formats */
/* AARCH64_SWS_OP_CONVERT */

static void asmgen_op_convert(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    AArch64Op *vl = s->vl;
    AArch64Op *vh = s->vh;

    size_t src_el_size = s->el_size;
    size_t dst_el_size = sws_aarch64_pixel_size(p->to_type);

    /* This function assumes block_size is either 8 or 16, and that
     * we're always using the most amount of vector registers possible.
     * Therefore, u32 always uses the high vector bank.
     */
    if (p->type == AARCH64_PIXEL_F32) {
        aarch64_add_comment(a, "f32 -> u32");
        LOOP_MASK(p, i) i_fcvtzu(a, v_4s(vl[i]), v_4s(vl[i]));
        LOOP_MASK(p, i) i_fcvtzu(a, v_4s(vh[i]), v_4s(vh[i]));
    }

    if (p->block_size == 8) {
        if (src_el_size == 1 && dst_el_size > src_el_size) {
            aarch64_add_comment(a, "u8 -> u16");
            LOOP_MASK(p, i) i_uxtl (a, v_8h(vl[i]), v_8b(vl[i]));
            src_el_size = 2;
        } else if (src_el_size == 4 && dst_el_size < src_el_size) {
            aarch64_add_comment(a, "u32 -> u16");
            LOOP_MASK(p, i) i_xtn  (a, v_4h(vl[i]), v_4s(vl[i]));
            LOOP_MASK(p, i) i_xtn  (a, v_4h(vh[i]), v_4s(vh[i]));
            LOOP_MASK(p, i) i_ins  (a, ve_d(vl[i], 1), ve_d(vh[i], 0));
            src_el_size = 2;
        }
        if (src_el_size == 2 && dst_el_size == 4) {
            aarch64_add_comment(a, "u16 -> u32");
            LOOP_MASK(p, i) i_uxtl2(a, v_4s(vh[i]), v_8h(vl[i]));
            LOOP_MASK(p, i) i_uxtl (a, v_4s(vl[i]), v_4h(vl[i]));
            src_el_size = 4;
        } else if (src_el_size == 2 && dst_el_size == 1) {
            aarch64_add_comment(a, "u16 -> u8");
            LOOP_MASK(p, i) i_xtn  (a, v_8b(vl[i]), v_8h(vl[i]));
            src_el_size = 1;
        }
    } else /* if (p->block_size == 16) */ {
        if (src_el_size == 1 && dst_el_size == 2) {
            aarch64_add_comment(a, "u8 -> u16");
            LOOP_MASK(p, i) i_uxtl2(a, v_8h(vh[i]), v_16b(vl[i]));
            LOOP_MASK(p, i) i_uxtl (a, v_8h(vl[i]), v_8b(vl[i]));
        } else if (src_el_size == 2 && dst_el_size == 1) {
            aarch64_add_comment(a, "u16 -> u8");
            LOOP_MASK(p, i) i_xtn  (a, v_8b(vl[i]), v_8h(vl[i]));
            LOOP_MASK(p, i) i_xtn  (a, v_8b(vh[i]), v_8h(vh[i]));
            LOOP_MASK(p, i) i_ins  (a, ve_d(vl[i], 1), ve_d(vh[i], 0));
        }
    }

    /* See comment above for high vector bank usage for u32. */
    if (p->to_type == AARCH64_PIXEL_F32) {
        aarch64_add_comment(a, "u32 -> f32");
        LOOP_MASK(p, i) i_ucvtf(a, v_4s(vl[i]), v_4s(vl[i]));
        LOOP_MASK(p, i) i_ucvtf(a, v_4s(vh[i]), v_4s(vh[i]));
    }
}

/*********************************************************************/
/* expand integers to the full range */
/* AARCH64_SWS_OP_EXPAND */

static void asmgen_op_expand(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    AArch64Op *vl = s->vl;
    AArch64Op *vh = s->vh;

    size_t src_el_size = s->el_size;
    size_t dst_el_size = sws_aarch64_pixel_size(p->to_type);
    size_t dst_total_size = p->block_size * dst_el_size;
    size_t dst_vec_size = FFMIN(dst_total_size, 16);

    if (!s->use_vh)
        s->use_vh = (dst_vec_size != dst_total_size);

    if (src_el_size == 1) {
        aarch64_add_comment(a, "u8 -> u16");
        reshape_all_vectors(s, 16, 1);
        LOOP_MASK_VH(s, p, i) i_zip2(a, vh[i], vl[i], vl[i]);
        LOOP_MASK      (p, i) i_zip1(a, vl[i], vl[i], vl[i]);
    }
    if (dst_el_size == 4) {
        aarch64_add_comment(a, "u16 -> u32");
        reshape_all_vectors(s, 8, 2);
        LOOP_MASK_VH(s, p, i) i_zip2(a, vh[i], vl[i], vl[i]);
        LOOP_MASK      (p, i) i_zip1(a, vl[i], vl[i], vl[i]);
    }
}

/*********************************************************************/
/* numeric minimum (q4) */
/* AARCH64_SWS_OP_MIN */

static void asmgen_op_min(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    AArch64Op *vl = s->vl;
    AArch64Op *vh = s->vh;
    AArch64Op *vt = s->vt;
    AArch64Op min_vec = a64op_make_vec(a64op_vec_n(s->vt[4]), 0, s->el_size);

    aarch64_annotate_next(a, "v128 min_vec = impl->priv.v128;");
    i_ldr(a, v_q(min_vec), a64op_off(s->impl, offsetof_impl_priv));
    aarch64_add_comment(a, "broadcast each lane of min_vec into a full vector");
    LOOP_MASK(p, i) i_dup(a, vt[i], a64op_elem(min_vec, i));

    if (p->type == AARCH64_PIXEL_F32) {
        aarch64_add_comment(a, "min (float)");
        LOOP_MASK      (p, i) i_fmin(a, vl[i], vl[i], vt[i]);
        LOOP_MASK_VH(s, p, i) i_fmin(a, vh[i], vh[i], vt[i]);
    } else {
        aarch64_add_comment(a, "min (integer)");
        LOOP_MASK      (p, i) i_umin(a, vl[i], vl[i], vt[i]);
        LOOP_MASK_VH(s, p, i) i_umin(a, vh[i], vh[i], vt[i]);
    }
}

/*********************************************************************/
/* numeric maximum (q4) */
/* AARCH64_SWS_OP_MAX */

static void asmgen_op_max(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    AArch64Op *vl = s->vl;
    AArch64Op *vh = s->vh;
    AArch64Op *vt = s->vt;
    AArch64Op max_vec = a64op_make_vec(a64op_vec_n(s->vt[4]), 0, s->el_size);

    aarch64_annotate_next(a, "v128 max_vec = impl->priv.v128;");
    i_ldr(a, v_q(max_vec), a64op_off(s->impl, offsetof_impl_priv));
    aarch64_add_comment(a, "broadcast each lane of max_vec into a full vector");
    LOOP_MASK      (p, i) i_dup(a, vt[i], a64op_elem(max_vec, i));

    if (p->type == AARCH64_PIXEL_F32) {
        aarch64_add_comment(a, "max (float)");
        LOOP_MASK      (p, i) i_fmax(a, vl[i], vl[i], vt[i]);
        LOOP_MASK_VH(s, p, i) i_fmax(a, vh[i], vh[i], vt[i]);
    } else {
        aarch64_add_comment(a, "max (integer)");
        LOOP_MASK      (p, i) i_umax(a, vl[i], vl[i], vt[i]);
        LOOP_MASK_VH(s, p, i) i_umax(a, vh[i], vh[i], vt[i]);
    }
}

/*********************************************************************/
/* multiplication by scalar (q) */
/* AARCH64_SWS_OP_SCALE */

static void asmgen_op_scale(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    AArch64Op *vl = s->vl;
    AArch64Op *vh = s->vh;
    AArch64Op priv_ptr = s->tmp0;
    AArch64Op scale_vec = s->vt[0];

    aarch64_annotate_next(a, "v128 *scale_vec_ptr = &impl->priv");
    i_add(a, priv_ptr, s->impl, a64op_imm(offsetof_impl_priv));

    aarch64_annotate_next(a, "v128 scale_vec = broadcast(*scale_vec_ptr);");
    i_ld1r(a, vv_1(scale_vec), a64op_base(priv_ptr));

    if (p->type == AARCH64_PIXEL_F32) {
        LOOP_MASK      (p, i) { i_fmul(a, vl[i], vl[i], scale_vec); inlcmtf(a, "vl[%u] *= scale_vec;", i); }
        LOOP_MASK_VH(s, p, i) { i_fmul(a, vh[i], vh[i], scale_vec); inlcmtf(a, "vh[%u] *= scale_vec;", i); }
    } else {
        LOOP_MASK      (p, i) { i_mul (a, vl[i], vl[i], scale_vec); inlcmtf(a, "vl[%u] *= scale_vec;", i); }
        LOOP_MASK_VH(s, p, i) { i_mul (a, vh[i], vh[i], scale_vec); inlcmtf(a, "vh[%u] *= scale_vec;", i); }
    }
}

/*********************************************************************/
/* generalized linear affine transform */
/* AARCH64_SWS_OP_LINEAR */

/* One vl/vh pass of the generalized linear affine transform.
 * v[]  is the working register array (vl or vh); vt[] holds saved sources.
 * Coefficients are preloaded into v21-v24; k is the flat coefficient index
 * at the start of this pass (always 0 — shared between passes). */
static void linear_pass(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                        AArch64Op *vx, AArch64Op *vt, AArch64Op *vc,
                        int save_mask, const int fdata_swizzle[5],
                        int vh)
{
    AArch64Context *a = s->actx;
    int k = 0;

    if (vh && !s->use_vh)
        return;

    if (save_mask)
        aarch64_add_comment(a, "save input rows");
    for (int sj = 0; sj < 4; sj++)
        if (MASK_GET(save_mask, sj))
            i_mov(a, v_16b(vt[sj]), v_16b(vx[sj]));

    aarch64_add_comment(a, "affine transform");
    LOOP_MASK(p, i) {
        bool first = true;
        for (int j = 0; j < 5; j++) {
            int sj = fdata_swizzle[j];
            if (!LINEAR_MASK_GET(p->linear, i, sj))
                continue;
            AArch64Op vcoeff = vc[k / 4];
            int lane = k % 4;
            k++;
            AArch64Op vsrc = (sj < 4) ? (MASK_GET(save_mask, sj) ? vt[sj] : vx[sj])
                                       : OPN;
            if (first) {
                if (sj == 4)
                    i_dup(a, vx[i], ve_s(vcoeff, lane));
                else
                    i_fmul(a, vx[i], vsrc, ve_s(vcoeff, lane));
                first = false;
            } else {
                i_fmla(a, vx[i], vsrc, ve_s(vcoeff, lane));
            }
        }
    }
}

static void asmgen_op_linear(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    AArch64Op *vl = s->vl;
    AArch64Op *vh = s->vh;
    AArch64Op *vt = s->vt;
    AArch64Op *vc = &vt[4];
    AArch64Op vcoeff_ptr = s->tmp0;

    /* Process offset first (column 4), then cross-row columns 0..3 */
    const int fdata_swizzle[5] = { 4, 0, 1, 2, 3 };

    /*
     * Count non-zero coefficients and preload them all into v20-v23
     * (4 floats per register, 4 registers = up to 16 coefficients).
     * v20-v23 are caller-saved and not used by vl (v0-v3), vh (v4-v7),
     * or vt (v16-v19). The coefficient array is allocated with padding
     * so the ld1 at the last register never reads past the allocation.
     */
    int count = 0;
    LOOP_MASK(p, i) {
        for (int j = 0; j < 5; j++) {
            int sj = fdata_swizzle[j];
            if (LINEAR_MASK_GET(p->linear, i, sj))
                count++;
        }
    }
    int num_regs = (count + 3) / 4;
    assert(num_regs <= 4);

    aarch64_add_comment(a, "preload coefficients");

    aarch64_annotate_next(a, "v128 *vcoeff_ptr = impl->priv.ptr;");
    i_ldr(a, vcoeff_ptr, a64op_off(s->impl, offsetof_impl_priv));
    switch (num_regs) {
    case 1: i_ld1(a, vv_1(vc[0]),                      a64op_base(vcoeff_ptr)); break;
    case 2: i_ld1(a, vv_2(vc[0], vc[1]),               a64op_base(vcoeff_ptr)); break;
    case 3: i_ld1(a, vv_3(vc[0], vc[1], vc[2]),        a64op_base(vcoeff_ptr)); break;
    case 4: i_ld1(a, vv_4(vc[0], vc[1], vc[2], vc[3]), a64op_base(vcoeff_ptr)); break;
    }

    /*
     * Determine which source columns need saving to vt[] before computation.
     * A column sj needs saving if v[sj] may be overwritten before its last use:
     *   1. Any row i > sj (processed later) reads column sj, so v[sj] will be
     *      overwritten when row sj is computed before row i reads it.
     *   2. Row sj itself reads column sj, but it is not the first non-zero term,
     *      so v[sj] is overwritten by an earlier term before the diagonal read.
     * If neither applies, v[sj] is still the original value when needed, and
     * we can read it directly without a save.
     */
    uint16_t save_mask = 0;
    for (int sj = 0; sj < 4; sj++) {
        /* Condition 1: any row i > sj uses column sj */
        for (int i = sj + 1; i < 4; i++) {
            if (MASK_GET(p->mask, i) && LINEAR_MASK_GET(p->linear, i, sj)) {
                MASK_SET(save_mask, sj, 1);
                break;
            }
        }
        if (MASK_GET(save_mask, sj))
            continue;
        /* Condition 2: diagonal entry exists but is not the first term for row sj */
        if (!MASK_GET(p->mask, sj))
            continue;
        if (!LINEAR_MASK_GET(p->linear, sj, sj))
            continue;
        for (int j = 0; fdata_swizzle[j] != sj; j++) {
            if (LINEAR_MASK_GET(p->linear, sj, fdata_swizzle[j])) {
                MASK_SET(save_mask, sj, 1);
                break;
            }
        }
    }

    linear_pass(s, p, vl, vt, vc, save_mask, fdata_swizzle, 0);
    linear_pass(s, p, vh, vt, vc, save_mask, fdata_swizzle, 1);
}

/*********************************************************************/
/* add dithering noise */
/* AARCH64_SWS_OP_DITHER */

static void asmgen_op_dither(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;

    const int size_log2   = p->dither.size_log2;
    const int stride_log2 = size_log2 + 2; /* size floats × 4 bytes/float */
    const int x_log2      = size_log2 - __builtin_ctz(p->block_size);

    /* Collect active components (nibble != 0xf) sorted ascending by y_off.
     * This is pure codegen-time work — no assembly emitted here. */
    int y_offs[4];
    for (int i = 0; i < 4; i++)
        y_offs[i] = (p->dither.y_offset >> (i * 4)) & 0xf;

    int sorted[4], nsorted = 0;
    bool used[4] = { false };
    for (int pass = 0; pass < 4; pass++) {
        int best = -1;
        for (int i = 0; i < 4; i++) {
            if (used[i] || y_offs[i] == 0xf)
                continue;
            if (best < 0 || y_offs[i] < y_offs[best])
                best = i;
        }
        if (best < 0)
            break;
        sorted[nsorted++] = best;
        used[best] = true;
    }

    if (nsorted == 0)
        return;

    AArch64Op ptr    = s->tmp0;
    AArch64Op tmp1   = s->tmp1;
    AArch64Op w_tmp1 = a64op_w(s->tmp1);

    aarch64_add_comment(a, "load dither matrix pointer");

    aarch64_annotate_next(a, "void *ptr = impl->priv.ptr;");
    i_ldr(a, ptr, a64op_off(s->impl, offsetof_impl_priv));

    if (x_log2 > 0) {
        /* ptr += (bx & x_mask) * block_size * sizeof(float)
         * ubfiz extracts bits [x_log2-1:0] from bx and shifts left by 5
         * (= log2(block_size * sizeof(float)) = log2(8*4) = 5). */
        aarch64_add_comment(a, "add x offset");
        i_ubfiz(a, tmp1, a64op_x(s->bx), a64op_imm(5), a64op_imm(x_log2));
        i_add(a, ptr, ptr, tmp1);
    }

    aarch64_add_comment(a, "dither");
    int last_y_off = -1;
    for (int k = 0; k < nsorted; k++) {
        int i     = sorted[k];
        int y_off = y_offs[i];
        bool do_load = (y_off != last_y_off);

        if (k == 0) {
            /* First component: compute row byte offset via ubfiz (mask+shift
             * in one instruction: bits [size_log2-1:0] of (y+y_off), shifted
             * left by stride_log2) and advance ptr. */
            if (y_off == 0) {
                i_ubfiz(a, tmp1, a64op_x(s->y), a64op_imm(stride_log2), a64op_imm(size_log2));
            } else {
                i_add(a, w_tmp1, s->y, a64op_imm(y_off));
                i_ubfiz(a, tmp1, tmp1, a64op_imm(stride_log2), a64op_imm(size_log2));
            }
            i_add(a, ptr, ptr, tmp1);
        } else if (do_load) {
            /* Subsequent component with different y_off: advance ptr by the
             * compile-time byte delta between consecutive y offsets. */
            int delta = (y_off - last_y_off) << stride_log2;
            i_add(a, ptr, ptr, a64op_imm(delta));
        }

        if (do_load)
            i_ldp(a, v_q(s->vt[0]), v_q(s->vt[1]), a64op_base(ptr));

        i_fadd(a, s->vl[i], s->vl[i], s->vt[0]);
        if (s->use_vh)
            i_fadd(a, s->vh[i], s->vh[i], s->vt[1]);

        last_y_off = y_off;
    }
}

/*********************************************************************/
static void asmgen_op(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;

    switch (p->op) {
    case AARCH64_SWS_OP_PROCESS:
        asmgen_process(s, p);
        return;
    case AARCH64_SWS_OP_PROCESS_RETURN:
        asmgen_process_return(s, p);
        return;
    default:
        break;
    }

    char func_name[256];
    sws_aarch64_op_impl_func_name(func_name, sizeof(func_name), p);

    aarch64_func_begin(a, func_name, true);

    aarch64_annotate_next(a, "SwsFuncPtr cont = impl->cont;");
    i_ldr(a, s->cont, a64op_off(s->impl, offsetof_impl_cont));

    size_t el_size = sws_aarch64_pixel_size(p->type);
    size_t total_size = p->block_size * el_size;
    s->vec_size = FFMIN(total_size, 16);

    s->use_vh = (s->vec_size != total_size);

    s->el_size = el_size;
    s->el_count = s->vec_size / el_size;

    reshape_all_vectors(s, s->el_count, el_size);

    switch (p->op) {
    case AARCH64_SWS_OP_READ_BIT:     asmgen_op_read_bit(s, p);     break;
    case AARCH64_SWS_OP_READ_NIBBLE:  asmgen_op_read_nibble(s, p);  break;
    case AARCH64_SWS_OP_READ_PACKED:  asmgen_op_read_packed(s, p);  break;
    case AARCH64_SWS_OP_READ_PLANAR:  asmgen_op_read_planar(s, p);  break;
    case AARCH64_SWS_OP_WRITE_BIT:    asmgen_op_write_bit(s, p);    break;
    case AARCH64_SWS_OP_WRITE_NIBBLE: asmgen_op_write_nibble(s, p); break;
    case AARCH64_SWS_OP_WRITE_PACKED: asmgen_op_write_packed(s, p); break;
    case AARCH64_SWS_OP_WRITE_PLANAR: asmgen_op_write_planar(s, p); break;
    case AARCH64_SWS_OP_SWAP_BYTES:   asmgen_op_swap_bytes(s, p);   break;
    case AARCH64_SWS_OP_SWIZZLE:      asmgen_op_swizzle(s, p);      break;
    case AARCH64_SWS_OP_UNPACK:       asmgen_op_unpack(s, p);       break;
    case AARCH64_SWS_OP_PACK:         asmgen_op_pack(s, p);         break;
    case AARCH64_SWS_OP_LSHIFT:       asmgen_op_lshift(s, p);       break;
    case AARCH64_SWS_OP_RSHIFT:       asmgen_op_rshift(s, p);       break;
    case AARCH64_SWS_OP_CLEAR:        asmgen_op_clear(s, p);        break;
    case AARCH64_SWS_OP_CONVERT:      asmgen_op_convert(s, p);      break;
    case AARCH64_SWS_OP_EXPAND:       asmgen_op_expand(s, p);       break;
    case AARCH64_SWS_OP_MIN:          asmgen_op_min(s, p);          break;
    case AARCH64_SWS_OP_MAX:          asmgen_op_max(s, p);          break;
    case AARCH64_SWS_OP_SCALE:        asmgen_op_scale(s, p);        break;
    case AARCH64_SWS_OP_LINEAR:       asmgen_op_linear(s, p);       break;
    case AARCH64_SWS_OP_DITHER:       asmgen_op_dither(s, p);       break;
    default:
        break;
    }

    aarch64_annotate_next(a, "impl += 1;");
    i_add(a, s->impl, s->impl, a64op_imm(sizeof_impl));
    aarch64_annotate_next(a, "jump to cont");
    i_br (a, s->cont);
}

/*********************************************************************/
static int lookup_gen(void)
{
    printf("#include \"libswscale/aarch64/ops_lookup.h\"\n");
    printf("\n");
    for (const SwsAArch64OpImplParams *p = impl_params; p->op; p++) {
        char func_name[256];
        sws_aarch64_op_impl_func_name(func_name, sizeof(func_name), p);
        printf("extern void %s(void);\n", func_name);
    }
    printf("\n");
    printf("SwsFuncPtr ff_sws_aarch64_find_op(const SwsAArch64OpImplParams *p)\n{\n");
    for (const SwsAArch64OpImplParams *p = impl_params; p->op; p++) {
        char func_name[256];
        char cond_str[256];
        sws_aarch64_op_impl_func_name(func_name, sizeof(func_name), p);
        sws_aarch64_op_impl_cond_str(cond_str, sizeof(cond_str), p, "p->");
        printf("    if (%s) return (SwsFuncPtr) %s;\n", cond_str, func_name);
    }
    printf("    return NULL;\n");
    printf("}\n");
    return 0;
}

/*********************************************************************/
static int asmgen(void)
{
    AArch64Context *actx = aarch64_alloc();
    if (!actx)
        return AVERROR(ENOMEM);

    SwsAArch64Context s = { 0 };
    s.actx = actx;

    /* Process function arguments */
    s.exec      = a64op_gpx(0); // const SwsOpExec *exec
    s.impl      = a64op_gpx(1); // const void *priv
    s.bx_start  = a64op_gpw(2); // int bx_start
    s.y_start   = a64op_gpw(3); // int y_start
    s.bx_end    = a64op_gpw(4); // int bx_end
    s.y_end     = a64op_gpw(5); // int y_end

    s.bx        = a64op_gpw(6);
    s.y         = s.y_start;
    s.tmp0      = a64op_gpx(7);
    s.tmp1      = a64op_gpx(8);
    s.op0_func  = a64op_gpx(9);
    s.op1_impl  = a64op_gpx(28);
    s.cont      = s.exec;

    s.in         [0] = a64op_gpx(10);
    s.in         [1] = a64op_gpx(11);
    s.in         [2] = a64op_gpx(12);
    s.in         [3] = a64op_gpx(13);
    s.out        [0] = a64op_gpx(14);
    s.out        [1] = a64op_gpx(15);
    s.out        [2] = a64op_gpx(16);
    s.out        [3] = a64op_gpx(17);
    s.in_padding [0] = a64op_gpx(20);
    s.in_padding [1] = a64op_gpx(21);
    s.in_padding [2] = a64op_gpx(22);
    s.in_padding [3] = a64op_gpx(23);
    s.out_padding[0] = a64op_gpx(24);
    s.out_padding[1] = a64op_gpx(25);
    s.out_padding[2] = a64op_gpx(26);
    s.out_padding[3] = a64op_gpx(27);

    const SwsAArch64OpImplParams *params = impl_params;
    while (params->op)
        asmgen_op(&s, params++);

    printf("#include \"libavutil/aarch64/asm.S\"\n");
    printf("\n");

    aarch64_print(s.actx, stdout);

    aarch64_free(&s.actx);

    return 0;
}

/*********************************************************************/
int main(int argc, char *argv[])
{
    bool lookup = false;
    bool ops = false;

#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-ops"))
            ops = true;
        else if (!strcmp(argv[i], "-lookup"))
            lookup = true;
    }
    if ((lookup && ops) || (!lookup && !ops)) {
        fprintf(stderr, "Exactly one of -ops or -lookup must be specified.\n");
        return -1;
    }

    return lookup ? lookup_gen() : asmgen();
}
