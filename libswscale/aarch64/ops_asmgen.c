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
#define CMT(comment) aarch64_annotate(a, comment)
#define CMTF(fmt, ...) aarch64_annotatef(a, (char[128]){0}, 128, fmt, __VA_ARGS__)

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
    AArch64Op in_bump[4];
    AArch64Op out_bump[4];

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
            i_ldr(a, regs[n - 1],              a64op_off(sp, (n - 1) * sizeof(uint64_t)));
        for (unsigned i = (n & ~1u) - 2; i >= 2; i -= 2)
            i_ldp(a, regs[i],     regs[i + 1], a64op_off(sp, i * sizeof(uint64_t)));
        i_ldp(a, regs[0], regs[1], sp_post);
    }
}

/*********************************************************************/
static unsigned clobbered_gprs(const SwsAArch64Context *s,
                               const SwsAArch64OpImplParams *p,
                               AArch64Op regs[MAX_SAVED_REGS])
{
    unsigned n = 0;
    regs[n++] = s->op1_impl;
    LOOP_MASK(p, i) {
        regs[n++] = s->in_bump[i];
        regs[n++] = s->out_bump[i];
    }
    return n;
}

static void asmgen_process(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    char func_name[128];
    char buf[64];

    sws_aarch64_op_impl_func_name(func_name, sizeof(func_name), p);

    aarch64_func_begin(a, func_name, true);

    /* Function prologue */
    AArch64Op saved_regs[MAX_SAVED_REGS];
    unsigned nsaved = clobbered_gprs(s, p, saved_regs);
    if (nsaved) {
        aarch64_add_comment(a, "prologue");
        asmgen_prologue(s, saved_regs, nsaved);
    }

    i_ldr(a, s->op0_func, a64op_off(s->impl, offsetof_impl_cont));  CMT("SwsFuncPtr op0_func = impl->cont;");
    i_add(a, s->op1_impl, s->impl, a64op_imm(sizeof_impl));         CMT("SwsOpImpl *op1_impl = impl + 1;");

    LOOP_MASK(p, i) {
        aarch64_annotate_nextf(a, buf, sizeof(buf), "in[%u] = exec->in[%u];", i, i);
        i_ldr(a, s->in[i],       a64op_off(s->exec, offsetof_exec_in       + (i * sizeof(uint8_t *))));
    }
    LOOP_MASK(p, i) {
        aarch64_annotate_nextf(a, buf, sizeof(buf), "out[%u] = exec->out[%u];", i, i);
        i_ldr(a, s->out[i],      a64op_off(s->exec, offsetof_exec_out      + (i * sizeof(uint8_t *))));
    }
    LOOP_MASK(p, i) {
        aarch64_annotate_nextf(a, buf, sizeof(buf), "in_bump[%u] = exec->in_bump[%u];", i, i);
        i_ldr(a, s->in_bump[i],  a64op_off(s->exec, offsetof_exec_in_bump  + (i * sizeof(ptrdiff_t))));
    }
    LOOP_MASK(p, i) {
        aarch64_annotate_nextf(a, buf, sizeof(buf), "out_bump[%u] = exec->out_bump[%u];", i, i);
        i_ldr(a, s->out_bump[i], a64op_off(s->exec, offsetof_exec_out_bump + (i * sizeof(ptrdiff_t))));
    }

    i_mov(a, s->bx, s->bx_start);   CMT("bx = bx_start;");
    i_mov(a, s->impl, s->op1_impl); CMT("impl = op1_impl;");
    i_br (a, s->op0_func);          CMT("jump to op0_func");
}

static void asmgen_process_return(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    char func_name[128];

    sws_aarch64_op_impl_func_name(func_name, sizeof(func_name), p);

    aarch64_func_begin(a, func_name, true);

    i_mov(a, s->impl, s->op1_impl);         CMT("impl = op1_impl;");

    /* horizontal loop */
    int loop = aarch64_new_label(a, NULL);
    i_add(a, s->bx, s->bx, a64op_imm(1));   CMT("bx += 1;");
    i_cmp(a, s->bx, s->bx_end);             CMT("if (bx != bx_end)");
    i_bne(a, loop);                         CMT("    goto loop;");

    /* vertical loop */
    int end = aarch64_new_label(a, NULL);
    i_add(a, s->y, s->y, a64op_imm(1));     CMT("y += 1;");
    i_cmp(a, s->y, s->y_end);               CMT("if (y == y_end)");
    i_beq(a, end);                          CMT("    goto end;");

    /* padding */
    LOOP_MASK(p, i) { i_add(a, s->in[i],  s->in[i],  s->in_bump[i]);  CMTF("in[%u] += in_bump[%u];", i, i); }
    LOOP_MASK(p, i) { i_add(a, s->out[i], s->out[i], s->out_bump[i]); CMTF("out[%u] += out_bump[%u];", i, i); }
    i_mov(a, s->bx, s->bx_start);           CMT("bx = bx_start;");

    aarch64_add_label(a, loop);             CMT("loop:");
    i_br (a, s->op0_func);                  CMT("jump to op0_func");
    aarch64_add_label(a, end);              CMT("end:");

    /* Function epilogue */
    AArch64Op saved_regs[MAX_SAVED_REGS];
    unsigned nsaved = clobbered_gprs(s, p, saved_regs);
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
    AArch64OpVecOp vl[1];
    AArch64OpVecOp vtmp;
    AArch64OpVecOp shift_vec;
    AArch64Op bitmask_vec = s->vt[1];
    AArch64Op wtmp = a64op_w(s->tmp0);

    a64op_vec_struct(s->vt[0], &shift_vec);
    a64op_vec_struct(s->vl[0], &vl[0]);
    a64op_vec_struct(s->vt[2], &vtmp);

    /* Note that shift_vec has negative values, so that using it with
     * ushl actually performs a right shift. */
    aarch64_annotate_next(a, "v128 shift_vec = impl->priv.v128;");
    i_ldr(a, shift_vec.q, a64op_off(s->impl, offsetof_impl_priv));

    if (p->block_size == 16) {
        i_ldrh(a, wtmp,        a64op_post(s->in[0], 2));    CMT("uint16_t tmp = *in[0]++;");
        i_movi(a, bitmask_vec, a64op_imm(1));               CMT("v128 bitmask_vec = {1 <repeats 16 times>};");
        i_dup (a, vl[0].b8,    wtmp);                       CMT("vl[0].lo = broadcast(tmp);");
        i_lsr (a, wtmp,        wtmp, a64op_imm(8));         CMT("tmp >>= 8;");
        i_dup (a, vtmp.b8,     wtmp);                       CMT("vtmp.lo = broadcast(tmp);");
        i_ins (a, vl[0].de[1], vtmp.de[0]);                 CMT("vl[0].hi = vtmp.lo;");
        i_ushl(a, vl[0].b16,   vl[0].b16, shift_vec.b16);   CMT("vl[0] <<= shift_vec;");
        i_and (a, vl[0].b16,   vl[0].b16, bitmask_vec);     CMT("vl[0] &= bitmask_vec;");
    } else {
        i_ldrb(a, wtmp,        a64op_post(s->in[0], 1));    CMT("uint8_t tmp = *in[0]++;");
        i_movi(a, bitmask_vec, a64op_imm(1));               CMT("v128 bitmask_vec = {1 <repeats 8 times>, 0 <repeats 8 times>};");
        i_dup (a, vl[0].b8,    wtmp);                       CMT("vl[0].lo = broadcast(tmp);");
        i_ushl(a, vl[0].b8,    vl[0].b8,  shift_vec.b8);    CMT("vl[0] <<= shift_vec;");
        i_and (a, vl[0].b8,    vl[0].b8,  bitmask_vec);     CMT("vl[0] &= bitmask_vec;");
    }
}

static void asmgen_op_read_nibble(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    AArch64OpVecOp vl[1];
    AArch64OpVecOp vtmp;
    AArch64Op nibble_mask = v_8b(s->vt[0]);

    a64op_vec_struct(s->vl[0], &vl[0]);
    a64op_vec_struct(s->vt[1], &vtmp);

    aarch64_annotate_next(a, "v128 nibble_mask = {0xf <repeats 8 times>, 0x0 <repeats 8 times>};");
    i_movi(a, nibble_mask, a64op_imm(0x0f));

    if (p->block_size == 8) {
        i_ldr (a, vl[0].s,   a64op_post(s->in[0], 4));      CMT("vl[0] = *in[0]++;");
        i_ushr(a, vtmp.b8,   vl[0].b8, a64op_imm(4));       CMT("vtmp.lo = vl[0] >> 4;");
        i_and (a, vl[0].b8,  vl[0].b8, nibble_mask);        CMT("vl[0].lo &= nibble_mask;");
        i_zip1(a, vl[0].b8,  vtmp.b8,  vl[0].b8);           CMT("interleave");
    } else {
        i_ldr (a, vl[0].d,   a64op_post(s->in[0], 8));      CMT("vl[0] = *in[0]++;");
        i_ushr(a, vtmp.b8,   vl[0].b8, a64op_imm(4));       CMT("vtmp.lo = vl[0] >> 4;");
        i_and (a, vl[0].b8,  vl[0].b8, nibble_mask);        CMT("vl[0].lo &= nibble_mask;");
        i_zip1(a, vl[0].b16, vtmp.b16, vl[0].b16);          CMT("interleave");
    }
}

static void asmgen_op_read_packed_1(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    AArch64OpVecOp vl[1];
    AArch64OpVecOp vh[1];

    a64op_vec_struct(s->vl[0], &vl[0]);
    a64op_vec_struct(s->vh[0], &vh[0]);

    switch ((s->use_vh ? 0x100 : 0) | s->vec_size) {
    case 0x008: i_ldr(a, vl[0].d,          a64op_post(s->in[0], s->vec_size * 1)); break;
    case 0x010: i_ldr(a, vl[0].q,          a64op_post(s->in[0], s->vec_size * 1)); break;
    case 0x108: i_ldp(a, vl[0].d, vh[0].d, a64op_post(s->in[0], s->vec_size * 2)); break;
    case 0x110: i_ldp(a, vl[0].q, vh[0].q, a64op_post(s->in[0], s->vec_size * 2)); break;
    }
}

static void asmgen_op_read_packed_n(SwsAArch64Context *s, const SwsAArch64OpImplParams *p, AArch64Op *vx)
{
    AArch64Context *a = s->actx;

    switch (p->mask) {
    case 0x0011: i_ld2(a, vv_2(vx[0], vx[1]),               a64op_post(s->in[0], s->vec_size * 2)); break;
    case 0x0111: i_ld3(a, vv_3(vx[0], vx[1], vx[2]),        a64op_post(s->in[0], s->vec_size * 3)); break;
    case 0x1111: i_ld4(a, vv_4(vx[0], vx[1], vx[2], vx[3]), a64op_post(s->in[0], s->vec_size * 4)); break;
    }
}

static void asmgen_op_read_packed(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    if (p->mask == 0x0001) {
        asmgen_op_read_packed_1(s, p);
    } else {
        asmgen_op_read_packed_n(s, p, s->vl);
        if (s->use_vh)
            asmgen_op_read_packed_n(s, p, s->vh);
    }
}

static void asmgen_op_read_planar(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    AArch64OpVecOp vl[4];
    AArch64OpVecOp vh[4];

    for (int i = 0; i < 4; i++) {
        a64op_vec_struct(s->vl[i], &vl[i]);
        a64op_vec_struct(s->vh[i], &vh[i]);
    }

    LOOP_MASK(p, i) {
        switch ((s->use_vh ? 0x100 : 0) | s->vec_size) {
        case 0x008: i_ldr(a, vl[i].d,          a64op_post(s->in[i], s->vec_size * 1)); break;
        case 0x010: i_ldr(a, vl[i].q,          a64op_post(s->in[i], s->vec_size * 1)); break;
        case 0x108: i_ldp(a, vl[i].d, vh[i].d, a64op_post(s->in[i], s->vec_size * 2)); break;
        case 0x110: i_ldp(a, vl[i].q, vh[i].q, a64op_post(s->in[i], s->vec_size * 2)); break;
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
    AArch64OpVecOp vl[1];
    AArch64OpVecOp shift_vec;
    AArch64OpVecOp vtmp0;
    AArch64OpVecOp vtmp1;

    a64op_vec_struct(s->vl[0], &vl[0]);
    a64op_vec_struct(s->vt[0], &shift_vec);
    a64op_vec_struct(s->vt[1], &vtmp0);
    a64op_vec_struct(s->vt[2], &vtmp1);

    aarch64_annotate_next(a, "v128 shift_vec = impl->priv.v128;");
    i_ldr(a, shift_vec.q, a64op_off(s->impl, offsetof_impl_priv));

    if (p->block_size == 8) {
        i_ushl(a, vl[0].b8,    vl[0].b8,   shift_vec.b8);   CMT("vl[0] <<= shift_vec;");
        i_addv(a, vtmp0.b,     vl[0].b8);                   CMT("vtmp0[0] = add_accross(vl[0].lo);");
        i_str (a, vtmp0.b,     a64op_post(s->out[0], 1));   CMT("*out[0]++ = vtmp0;");
    } else {
        i_ushl(a, vl[0].b16,   vl[0].b16,  shift_vec.b16);  CMT("vl[0] <<= shift_vec;");
        i_addv(a, vtmp0.b,     vl[0].b8);                   CMT("vtmp0[0] = add_accross(vl[0].lo);");
        i_ins (a, vtmp1.de[0], vl[0].de[1]);                CMT("vtmp1.lo = vtmp0.hi;");
        i_addv(a, vtmp1.b,     vtmp1.b8);                   CMT("vtmp1[0] = add_accross(vtmp1);");
        i_ins (a, vtmp0.be[1], vtmp1.be[0]);                CMT("vtmp0[1] = vtmp1[0];");
        i_str (a, vtmp0.h,     a64op_post(s->out[0], 2));   CMT("*out[0]++ = vtmp0;");
    }
}

static void asmgen_op_write_nibble(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    AArch64OpVecOp vl[4];
    AArch64OpVecOp vtmp0;
    AArch64OpVecOp vtmp1;

    for (int i = 0; i < 4; i++)
        a64op_vec_struct(s->vl[i], &vl[i]);
    a64op_vec_struct(s->vt[0], &vtmp0);
    a64op_vec_struct(s->vt[1], &vtmp1);

    if (p->block_size == 8) {
        i_shl (a, vtmp0.h4,  vl[0].h4,  a64op_imm(4));
        i_ushr(a, vtmp1.h4,  vl[0].h4,  a64op_imm(8));
        i_orr (a, vl[0].b8,  vtmp0.b8,  vtmp1.b8);
        i_xtn (a, vtmp0.b8,  vl[0].h8);
        i_str (a, vtmp0.s,   a64op_post(s->out[0], 4));
    } else {
        i_shl (a, vtmp0.h8,  vl[0].h8,  a64op_imm(4));
        i_ushr(a, vtmp1.h8,  vl[0].h8,  a64op_imm(8));
        i_orr (a, vl[0].b16, vtmp0.b16, vtmp1.b16);
        i_xtn (a, vtmp0.b8,  vl[0].h8);
        i_str (a, vtmp0.d,   a64op_post(s->out[0], 8));
    }
}

static void asmgen_op_write_packed_1(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    AArch64OpVecOp vl[1];
    AArch64OpVecOp vh[1];

    a64op_vec_struct(s->vl[0], &vl[0]);
    a64op_vec_struct(s->vh[0], &vh[0]);

    switch ((s->use_vh ? 0x100 : 0) | s->vec_size) {
    case 0x008: i_str(a, vl[0].d,          a64op_post(s->out[0], s->vec_size * 1)); break;
    case 0x010: i_str(a, vl[0].q,          a64op_post(s->out[0], s->vec_size * 1)); break;
    case 0x108: i_stp(a, vl[0].d, vh[0].d, a64op_post(s->out[0], s->vec_size * 2)); break;
    case 0x110: i_stp(a, vl[0].q, vh[0].q, a64op_post(s->out[0], s->vec_size * 2)); break;
    }
}

static void asmgen_op_write_packed_n(SwsAArch64Context *s, const SwsAArch64OpImplParams *p, AArch64Op *vx)
{
    AArch64Context *a = s->actx;

    switch (p->mask) {
    case 0x0011: i_st2(a, vv_2(vx[0], vx[1]),               a64op_post(s->out[0], s->vec_size * 2)); break;
    case 0x0111: i_st3(a, vv_3(vx[0], vx[1], vx[2]),        a64op_post(s->out[0], s->vec_size * 3)); break;
    case 0x1111: i_st4(a, vv_4(vx[0], vx[1], vx[2], vx[3]), a64op_post(s->out[0], s->vec_size * 4)); break;
    }
}

static void asmgen_op_write_packed(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    if (p->mask == 0x0001) {
        asmgen_op_write_packed_1(s, p);
    } else {
        asmgen_op_write_packed_n(s, p, s->vl);
        if (s->use_vh)
            asmgen_op_write_packed_n(s, p, s->vh);
    }
}

static void asmgen_op_write_planar(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    AArch64OpVecOp vl[4];
    AArch64OpVecOp vh[4];

    for (int i = 0; i < 4; i++) {
        a64op_vec_struct(s->vl[i], &vl[i]);
        a64op_vec_struct(s->vh[i], &vh[i]);
    }

    LOOP_MASK(p, i) {
        switch ((s->use_vh ? 0x100 : 0) | s->vec_size) {
        case 0x008: i_str(a, vl[i].d,          a64op_post(s->out[i], s->vec_size * 1)); break;
        case 0x010: i_str(a, vl[i].q,          a64op_post(s->out[i], s->vec_size * 1)); break;
        case 0x108: i_stp(a, vl[i].d, vh[i].d, a64op_post(s->out[i], s->vec_size * 2)); break;
        case 0x110: i_stp(a, vl[i].q, vh[i].q, a64op_post(s->out[i], s->vec_size * 2)); break;
        }
    }
}

/*********************************************************************/
/* swap byte order (for differing endianness) */
/* AARCH64_SWS_OP_SWAP_BYTES */

static void asmgen_op_swap_bytes(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    AArch64OpVecOp vl[4];
    AArch64OpVecOp vh[4];

    for (int i = 0; i < 4; i++) {
        a64op_vec_struct(s->vl[i], &vl[i]);
        a64op_vec_struct(s->vh[i], &vh[i]);
    }

    switch (sws_aarch64_pixel_size(p->type)) {
    case sizeof(uint16_t):
        LOOP_MASK      (p, i) i_rev16(a, vl[i].b16, vl[i].b16);
        LOOP_MASK_VH(s, p, i) i_rev16(a, vh[i].b16, vh[i].b16);
        break;
    case sizeof(uint32_t):
        LOOP_MASK      (p, i) i_rev32(a, vl[i].b16, vl[i].b16);
        LOOP_MASK_VH(s, p, i) i_rev32(a, vh[i].b16, vh[i].b16);
        break;
    }
}

/*********************************************************************/
/* rearrange channel order, or duplicate channels */
/* AARCH64_SWS_OP_SWIZZLE */

#define SWIZZLE_TMP 0xf

static const char *print_swizzle_v(char buf[8], uint8_t n, uint8_t vh)
{
    if (n == SWIZZLE_TMP)
        snprintf(buf, sizeof(char[8]), "vtmp%c", vh ? 'h' : 'l');
    else
        snprintf(buf, sizeof(char[8]), "v%c[%u]", vh ? 'h' : 'l', n);
    return buf;
}
#define PRINT_SWIZZLE_V(n, vh) print_swizzle_v((char[8]){ 0 }, n, vh)

static AArch64Op swizzle_a64op(SwsAArch64Context *s, uint8_t n, uint8_t vh)
{
    if (n == SWIZZLE_TMP)
        return s->vt[vh];
    return vh ? s->vh[n] : s->vl[n];
}

static void swizzle_emit(SwsAArch64Context *s, uint8_t dst, uint8_t src)
{
    AArch64Context *a = s->actx;
    AArch64Op src_op[2] = { swizzle_a64op(s, src, 0), swizzle_a64op(s, src, 1) };
    AArch64Op dst_op[2] = { swizzle_a64op(s, dst, 0), swizzle_a64op(s, dst, 1) };

    i_mov    (a, dst_op[0], src_op[0]); CMTF("%s = %s;", PRINT_SWIZZLE_V(dst, 0), PRINT_SWIZZLE_V(src, 0));
    if (s->use_vh) {
        i_mov(a, dst_op[1], src_op[1]); CMTF("%s = %s;", PRINT_SWIZZLE_V(dst, 1), PRINT_SWIZZLE_V(src, 1));
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
    AArch64Op mask_gpr = a64op_w(s->tmp0);
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
             * like 10-bit. In those cases, we use mov + dup instead.
             */
            if (val <= 0xff || val == 0xffff) {
                i_movi(a, vt[cur_vt], a64op_imm(val));
            } else {
                i_mov (a, mask_gpr,   a64op_imm(val));
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
            i_ushr  (a, vl[i], vl[0], a64op_imm(offsets[i]));
        else if (i)
            i_mov16b(a, vl[i], vl[0]);
    }
    LOOP_MASK_BWD_VH(s, p, i) {
        if (offsets[i])
            i_ushr  (a, vh[i], vh[0], a64op_imm(offsets[i]));
        else if (i)
            i_mov16b(a, vh[i], vh[0]);
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

    LOOP_MASK      (p, i) { i_shl(a, vl[i], vl[i], a64op_imm(p->shift)); CMTF("vl[%u] <<= %u;", i, p->shift); }
    LOOP_MASK_VH(s, p, i) { i_shl(a, vh[i], vh[i], a64op_imm(p->shift)); CMTF("vh[%u] <<= %u;", i, p->shift); }
}

/*********************************************************************/
/* right shift of raw pixel values by (u8) */
/* AARCH64_SWS_OP_RSHIFT */

static void asmgen_op_rshift(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    AArch64Op *vl = s->vl;
    AArch64Op *vh = s->vh;

    LOOP_MASK      (p, i) { i_ushr(a, vl[i], vl[i], a64op_imm(p->shift)); CMTF("vl[%u] >>= %u;", i, p->shift); }
    LOOP_MASK_VH(s, p, i) { i_ushr(a, vh[i], vh[i], a64op_imm(p->shift)); CMTF("vh[%u] >>= %u;", i, p->shift); }
}

/*********************************************************************/
/* clear pixel values */
/* AARCH64_SWS_OP_CLEAR */

static void asmgen_op_clear(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    AArch64Op *vl = s->vl;
    AArch64Op *vh = s->vh;
    AArch64Op clear_vec = s->vt[0];

    /* TODO
     * - pack elements in impl->priv and perform smaller loads
     * - if only 1 element and not vh, load directly with ld1r
     */

    i_ldr(a, v_q(clear_vec), a64op_off(s->impl, offsetof_impl_priv));   CMT("v128 clear_vec = impl->priv.v128;");

    LOOP_MASK      (p, i) { i_dup(a, vl[i], a64op_elem(clear_vec, i));  CMTF("vl[%u] = broadcast(clear_vec[%u])", i, i); }
    LOOP_MASK_VH(s, p, i) { i_dup(a, vh[i], a64op_elem(clear_vec, i));  CMTF("vh[%u] = broadcast(clear_vec[%u])", i, i); }
}

/*********************************************************************/
/* convert (cast) between formats */
/* AARCH64_SWS_OP_CONVERT */

static void asmgen_op_convert(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    AArch64OpVecOp vl[4];
    AArch64OpVecOp vh[4];

    for (int i = 0; i < 4; i++) {
        a64op_vec_struct(s->vl[i], &vl[i]);
        a64op_vec_struct(s->vh[i], &vh[i]);
    }

    size_t src_el_size = s->el_size;
    size_t dst_el_size = sws_aarch64_pixel_size(p->to_type);

    /* This function assumes block_size is either 8 or 16, and that
     * we're always using the most amount of vector registers possible.
     * Therefore, u32 always uses the high vector bank.
     */
    if (p->type == AARCH64_PIXEL_F32) {
        aarch64_add_comment(a, "f32 -> u32");
        LOOP_MASK(p, i) i_fcvtzu(a, vl[i].s4, vl[i].s4);
        LOOP_MASK(p, i) i_fcvtzu(a, vh[i].s4, vh[i].s4);
    }

    if (p->block_size == 8) {
        if (src_el_size == 1 && dst_el_size > src_el_size) {
            aarch64_add_comment(a, "u8 -> u16");
            LOOP_MASK(p, i) i_uxtl (a, vl[i].h8,    vl[i].b8);
            src_el_size = 2;
        } else if (src_el_size == 4 && dst_el_size < src_el_size) {
            aarch64_add_comment(a, "u32 -> u16");
            LOOP_MASK(p, i) i_xtn  (a, vl[i].h4,    vl[i].s4);
            LOOP_MASK(p, i) i_xtn  (a, vh[i].h4,    vh[i].s4);
            LOOP_MASK(p, i) i_ins  (a, vl[i].de[1], vh[i].de[0]);
            src_el_size = 2;
        }
        if (src_el_size == 2 && dst_el_size == 4) {
            aarch64_add_comment(a, "u16 -> u32");
            LOOP_MASK(p, i) i_uxtl2(a, vh[i].s4,    vl[i].h8);
            LOOP_MASK(p, i) i_uxtl (a, vl[i].s4,    vl[i].h4);
            src_el_size = 4;
        } else if (src_el_size == 2 && dst_el_size == 1) {
            aarch64_add_comment(a, "u16 -> u8");
            LOOP_MASK(p, i) i_xtn  (a, vl[i].b8,    vl[i].h8);
            src_el_size = 1;
        }
    } else /* if (p->block_size == 16) */ {
        if (src_el_size == 1 && dst_el_size == 2) {
            aarch64_add_comment(a, "u8 -> u16");
            LOOP_MASK(p, i) i_uxtl2(a, vh[i].h8,    vl[i].b16);
            LOOP_MASK(p, i) i_uxtl (a, vl[i].h8,    vl[i].b8);
        } else if (src_el_size == 2 && dst_el_size == 1) {
            aarch64_add_comment(a, "u16 -> u8");
            LOOP_MASK(p, i) i_xtn  (a, vl[i].b8,    vl[i].h8);
            LOOP_MASK(p, i) i_xtn  (a, vh[i].b8,    vh[i].h8);
            LOOP_MASK(p, i) i_ins  (a, vl[i].de[1], vh[i].de[0]);
        }
    }

    /* See comment above for high vector bank usage for u32. */
    if (p->to_type == AARCH64_PIXEL_F32) {
        aarch64_add_comment(a, "u32 -> f32");
        LOOP_MASK(p, i) i_ucvtf(a, vl[i].s4, vl[i].s4);
        LOOP_MASK(p, i) i_ucvtf(a, vh[i].s4, vh[i].s4);
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
    AArch64Op min_vec = s->vt[4];

    i_ldr(a, v_q(min_vec), a64op_off(s->impl, offsetof_impl_priv)); CMT("v128 min_vec = impl->priv.v128;");
    LOOP_MASK(p, i) { i_dup(a, vt[i], a64op_elem(min_vec, i));      CMTF("v128 vmin%u = min_vec[%u];", i, i); }

    if (p->type == AARCH64_PIXEL_F32) {
        LOOP_MASK      (p, i) { i_fmin(a, vl[i], vl[i], vt[i]);     CMTF("vl[%u] = min(vl[%u], vmin%u);", i, i, i); }
        LOOP_MASK_VH(s, p, i) { i_fmin(a, vh[i], vh[i], vt[i]);     CMTF("vh[%u] = min(vh[%u], vmin%u);", i, i, i); }
    } else {
        LOOP_MASK      (p, i) { i_umin(a, vl[i], vl[i], vt[i]);     CMTF("vl[%u] = min(vl[%u], vmin%u);", i, i, i); }
        LOOP_MASK_VH(s, p, i) { i_umin(a, vh[i], vh[i], vt[i]);     CMTF("vh[%u] = min(vh[%u], vmin%u);", i, i, i); }
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
    AArch64Op max_vec = s->vt[4];

    i_ldr(a, v_q(max_vec), a64op_off(s->impl, offsetof_impl_priv)); CMT("v128 max_vec = impl->priv.v128;");
    LOOP_MASK(p, i) { i_dup(a, vt[i], a64op_elem(max_vec, i));      CMTF("v128 vmax%u = max_vec[%u];", i, i); }

    if (p->type == AARCH64_PIXEL_F32) {
        LOOP_MASK      (p, i) { i_fmax(a, vl[i], vl[i], vt[i]);     CMTF("vl[%u] = max(vl[%u], vmax%u);", i, i, i); }
        LOOP_MASK_VH(s, p, i) { i_fmax(a, vh[i], vh[i], vt[i]);     CMTF("vh[%u] = max(vh[%u], vmax%u);", i, i, i); }
    } else {
        LOOP_MASK      (p, i) { i_umax(a, vl[i], vl[i], vt[i]);     CMTF("vl[%u] = max(vl[%u], vmax%u);", i, i, i); }
        LOOP_MASK_VH(s, p, i) { i_umax(a, vh[i], vh[i], vt[i]);     CMTF("vh[%u] = max(vh[%u], vmax%u);", i, i, i); }
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

    i_add (a, priv_ptr, s->impl, a64op_imm(offsetof_impl_priv));    CMT("v128 *scale_vec_ptr = &impl->priv;");
    i_ld1r(a, vv_1(scale_vec), a64op_base(priv_ptr));               CMT("v128 scale_vec = broadcast(*scale_vec_ptr);");

    if (p->type == AARCH64_PIXEL_F32) {
        LOOP_MASK      (p, i) { i_fmul(a, vl[i], vl[i], scale_vec); CMTF("vl[%u] *= scale_vec;", i); }
        LOOP_MASK_VH(s, p, i) { i_fmul(a, vh[i], vh[i], scale_vec); CMTF("vh[%u] *= scale_vec;", i); }
    } else {
        LOOP_MASK      (p, i) { i_mul (a, vl[i], vl[i], scale_vec); CMTF("vl[%u] *= scale_vec;", i); }
        LOOP_MASK_VH(s, p, i) { i_mul (a, vh[i], vh[i], scale_vec); CMTF("vh[%u] *= scale_vec;", i); }
    }
}

/*********************************************************************/
/* generalized linear affine transform */
/* AARCH64_SWS_OP_LINEAR */

/* Performs one pass of the linear transform over a single vector bank
 * (low or high). */
static void linear_pass(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                        AArch64Op *vt, AArch64Op *vc,
                        int save_mask, bool vh_pass)
{
    AArch64Context *a = s->actx;
    AArch64Op *vx = vh_pass ? s->vh : s->vl;
    char cvh = vh_pass ? 'h' : 'l';

    if (vh_pass && !s->use_vh)
        return;

    AArch64Op src_vx[4] = { vx[0], vx[1], vx[2], vx[3] };
    if (save_mask) {
        for (int i = 0; i < 4; i++) {
            if (MASK_GET(save_mask, i)) {
                src_vx[i] = vt[i];
                i_mov16b(a, vt[i], vx[i]);  CMTF("vsrc[%u] = v%c[%u];", i, cvh, i);
            }
        }
    }

    int i_coeff = 0;
    LOOP_MASK(p, i) {
        bool first = true;
        for (int j = 0; j < 5; j++) {
            if (!LINEAR_MASK_GET(p->linear, i, j))
                continue;
            uint8_t vc_i = i_coeff / 4;
            uint8_t vc_j = i_coeff & 3;
            AArch64Op vcoeff = a64op_elem(vc[vc_i], vc_j);
            i_coeff++;
            bool is_offset = (j == 0);
            int src_j = j - 1; // Map row index back to 0..3
            AArch64Op vsrc = src_vx[src_j];
            if (first && is_offset) {
                i_dup (a, vx[i], vcoeff);       CMTF("v%c[%u]  = broadcast(vc[%u][%u]);", cvh, i, vc_i, vc_j);
            } else if (first && !is_offset) {
                i_fmul(a, vx[i], vsrc, vcoeff); CMTF("v%c[%u]  = vsrc[%u] * vc[%u][%u];", cvh, i, src_j, vc_i, vc_j);
            } else {
                i_fmla(a, vx[i], vsrc, vcoeff); CMTF("v%c[%u] += vsrc[%u] * vc[%u][%u];", cvh, i, src_j, vc_i, vc_j);
            }
            first = false;
        }
    }
}

static void asmgen_op_linear(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    AArch64Op *vt = s->vt;
    AArch64Op *vc = &vt[4]; /* The coefficients are loaded starting from temp vector 4 */
    AArch64Op vcoeff_ptr = s->tmp0;
    AArch64Op coeff_veclist;

    /* Preload coefficients from impl->priv. */
    i_ldr(a, vcoeff_ptr, a64op_off(s->impl, offsetof_impl_priv)); CMT("v128 *vcoeff_ptr = impl->priv.ptr;");

    switch (linear_num_vregs(p)) {
    case 1: coeff_veclist = vv_1(vc[0]);                      break;
    case 2: coeff_veclist = vv_2(vc[0], vc[1]);               break;
    case 3: coeff_veclist = vv_3(vc[0], vc[1], vc[2]);        break;
    case 4: coeff_veclist = vv_4(vc[0], vc[1], vc[2], vc[3]); break;
    }

    aarch64_annotate_next(a, "coeff_veclist = *vcoeff_ptr;");
    i_ld1(a, coeff_veclist, a64op_base(vcoeff_ptr));

    /* Compute mask for rows that must be saved before being overwritten. */
    uint16_t save_mask = 0;
    bool overwritten[4] = { false, false, false, false };
    LOOP_MASK(p, i) {
        for (int j = 0; j < 5; j++) {
            if (!LINEAR_MASK_GET(p->linear, i, j))
                continue;
            bool is_offset = (j == 0);
            int src_j = j - 1; // Map row index back to 0..3
            if (!is_offset && overwritten[src_j])
                MASK_SET(save_mask, j - 1, 1);
            overwritten[i] = true;
        }
    }

    linear_pass(s, p, vt, vc, save_mask, false);
    linear_pass(s, p, vt, vc, save_mask, true);
}

/*********************************************************************/
/* add dithering noise */
/* AARCH64_SWS_OP_DITHER */

static void asmgen_op_dither(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    AArch64Op *vl = s->vl;
    AArch64Op *vh = s->vh;
    AArch64Op ptr = s->tmp0;
    AArch64Op tmp1 = s->tmp1;
    AArch64Op dither_vl = s->vt[0];
    AArch64Op dither_vh = s->vt[1];

    /* Sort y_offset so that we can safely always increment the pointers. */
    int sorted[4];
    int n_comps = 0;
    bool used[4] = { false };
    for (int pass = 0; pass < 4; pass++) {
        int best = -1;
        LOOP_MASK(p, i) {
            uint8_t y_off = MASK_GET(p->dither.y_offset, i);
            if (used[i] || y_off == 0xf)
                continue;
            if (best < 0 || y_off < MASK_GET(p->dither.y_offset, best))
                best = i;
        }
        if (best < 0)
            break;
        sorted[n_comps++] = best;
        used[best] = true;
    }

    aarch64_annotate_next(a, "void *ptr = impl->priv.ptr;");
    i_ldr(a, ptr, a64op_off(s->impl, offsetof_impl_priv));

    /*
     * We use ubfiz to mask and shift left in one single instruction:
     *   ubfiz <Wd>, <Wn>, #<lsb>, #<width>
     *   Wd = (Wn & ((1 << width) - 1)) << lsb;
     *
     * Given:
     *  block_size    =  8, log2(block_size)    = 3
     *  dither_size   = 16, log2(dither_size)   = 4, dither_mask = 0b1111
     *  sizeof(float) =  4, log2(sizeof(float)) = 2
     *
     * Suppose we have bx = 0bvvvv. To get x, we left shift by
     * log2(block_size) and end up with 0bvvvv000. Then we mask against
     * dither_mask, and end up with 0bv000. Finally we multiply by
     * sizeof(float), which is the same as shifting left by
     * log2(sizeof(float)). The result is 0bv00000.
     *
     * Therefore:
     *  width = log2(dither_size) - log2(block_size)
     *  lsb   = log2(block_size) + log2(sizeof(float))
     */
    const int block_size_log2   = (p->block_size == 16) ? 4: 3;
    const int dither_size_log2  = p->dither.size_log2;
    const int sizeof_float_log2 = 2;
    if (dither_size_log2 != block_size_log2) {
        aarch64_annotate_next(a, "tmp1 = (bx & ((dither_size / block_size) - 1)) * block_size * sizeof(float);");
        const int lsb   = block_size_log2 + sizeof_float_log2;
        const int width = dither_size_log2 - block_size_log2;
        i_ubfiz(a, tmp1, a64op_x(s->bx), a64op_imm(lsb), a64op_imm(width));
        aarch64_annotate_next(a, "ptr += tmp1;");
        i_add(a, ptr, ptr, tmp1);
    }

    int last_y_off = -1;
    int prev_i;
    for (int sorted_i = 0; sorted_i < n_comps; sorted_i++) {
        int i = sorted[sorted_i];
        uint8_t y_off = MASK_GET(p->dither.y_offset, i);
        bool do_load = (y_off != last_y_off);
        char buf[128];

        if (last_y_off < 0) {
            const int lsb   = dither_size_log2 + sizeof_float_log2;
            const int width = dither_size_log2;
            /* On the first run, calculate pointer inside dither_matrix */
            if (y_off == 0) {
                aarch64_annotate_next(a, "tmp1 = (y & (dither_size - 1)) * dither_size * sizeof(float);");
                i_ubfiz(a, tmp1, a64op_x(s->y), a64op_imm(lsb), a64op_imm(width));
            } else {
                aarch64_annotate_nextf(a, buf, sizeof(buf), "tmp1 = y + y_off[%u];", i);
                i_add(a, a64op_w(tmp1), s->y, a64op_imm(y_off));
                aarch64_annotate_next(a, "tmp1 = (tmp1 & (dither_size - 1)) * dither_size * sizeof(float);");
                i_ubfiz(a, tmp1, tmp1, a64op_imm(lsb), a64op_imm(width));
            }
            aarch64_annotate_next(a, "ptr += tmp1;");
            i_add(a, ptr, ptr, tmp1);
        } else if (do_load) {
            /* On subsequent runs, just increment the pointer.
             * The matrix repeats itself at the end, so we don't risk overreading.
             */
            int delta = (y_off - last_y_off) * (1 << dither_size_log2) * sizeof(float);
            aarch64_annotate_nextf(a, buf, sizeof(buf), "ptr += (y_off[%u] - y_off[%u]) * dither_size * sizeof(float);", i, prev_i);
            i_add(a, ptr, ptr, a64op_imm(delta));
        }

        if (do_load) {
            aarch64_annotate_next(a, "{ ditherl, ditherh } = *ptr;");
            i_ldp(a, v_q(dither_vl), v_q(dither_vh), a64op_base(ptr));
        }

        aarch64_annotate_nextf(a, buf, sizeof(buf), "vl[%u] += vditherl;", i);
        i_fadd    (a, vl[i], vl[i], dither_vl);
        if (s->use_vh) {
            aarch64_annotate_nextf(a, buf, sizeof(buf), "vh[%u] += vditherh;", i);
            i_fadd(a, vh[i], vh[i], dither_vh);
        }

        last_y_off = y_off;
        prev_i = i;
    }
}

/*********************************************************************/
static void asmgen_op(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    char func_name[128];

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

    sws_aarch64_op_impl_func_name(func_name, sizeof(func_name), p);

    aarch64_func_begin(a, func_name, true);

    // TODO comments
    size_t el_size = sws_aarch64_pixel_size(p->type);
    size_t total_size = p->block_size * el_size;
    s->vec_size = FFMIN(total_size, 16);
    s->use_vh = (s->vec_size != total_size);
    s->el_size = el_size;
    s->el_count = s->vec_size / el_size;
    reshape_all_vectors(s, s->el_count, el_size);

    i_ldr(a, s->cont, a64op_off(s->impl, offsetof_impl_cont));  CMT("SwsFuncPtr cont = impl->cont;");

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
    /* TODO implement AARCH64_SWS_OP_SHUFFLE */
    default:
        break;
    }

    i_add(a, s->impl, s->impl, a64op_imm(sizeof_impl)); CMT("impl += 1;");
    i_br (a, s->cont);                                  CMT("jump to cont");
}

/*********************************************************************/
static int lookup_gen(void)
{
    char buf[1024];

    /* External function declarations. */
    printf("#include \"libswscale/aarch64/ops_lookup.h\"\n");
    printf("\n");
    for (const SwsAArch64OpImplParams *p = impl_params; p->op; p++) {
        sws_aarch64_op_impl_func_name(buf, sizeof(buf), p);
        printf("extern void %s(void);\n", buf);
    }
    printf("\n");

    /* Lookup function. */
    printf("SwsFuncPtr ff_sws_aarch64_lookup(const SwsAArch64OpImplParams *p)\n");
    printf("{\n");
    const SwsAArch64OpImplParams *prev = NULL;
    for (const SwsAArch64OpImplParams *p = impl_params; p->op; p++) {
        sws_aarch64_op_impl_cond_str(buf, sizeof(buf), p, prev, "p->");
        printf("%s", buf);
        prev = p;
    }
    sws_aarch64_op_impl_cond_str(buf, sizeof(buf), NULL, prev, "p->");
    printf("%s", buf);
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

    s.in      [0] = a64op_gpx(10);
    s.in      [1] = a64op_gpx(11);
    s.in      [2] = a64op_gpx(12);
    s.in      [3] = a64op_gpx(13);
    s.out     [0] = a64op_gpx(14);
    s.out     [1] = a64op_gpx(15);
    s.out     [2] = a64op_gpx(16);
    s.out     [3] = a64op_gpx(17);
    s.in_bump [0] = a64op_gpx(20);
    s.in_bump [1] = a64op_gpx(21);
    s.in_bump [2] = a64op_gpx(22);
    s.in_bump [3] = a64op_gpx(23);
    s.out_bump[0] = a64op_gpx(24);
    s.out_bump[1] = a64op_gpx(25);
    s.out_bump[2] = a64op_gpx(26);
    s.out_bump[3] = a64op_gpx(27);

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
