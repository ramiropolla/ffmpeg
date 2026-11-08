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

#include "ops_asmgen.h"

/*********************************************************************/
/* Helpers functions. */

/* Looping when s->use_vh is set. */
#define LOOP_VH(s, mask, idx) if (s->use_vh) LOOP(mask, idx)
#define LOOP_MASK_VH(s, p, idx) if (s->use_vh) LOOP_MASK(p, idx)
#define LOOP_MASK_BWD_VH(s, p, idx) if (s->use_vh) LOOP_MASK_BWD(p, idx)
/* Looping over the third and fourth banks (32-bit types, block_size 16). */
#define LOOP_MASK_B23(s, p, b, idx) for (int b = 2; b < s->nbanks; b++) LOOP_MASK(p, idx)

/* Inline rasm comments. */
#define CMT(comment)   rasm_annotate(r, comment)
#define CMTF(fmt, ...) rasm_annotatef(r, (char[128]){0}, 128, fmt, __VA_ARGS__)

/* Reshape input/output vector registers for current SwsOp. */
static void reshape_io_vectors(SwsAArch64OpRegs *regs, int el_count, int el_size)
{
    for (int i = 0; i < 4; i++) {
        regs->sl[i] = a64op_make_vec(a64op_vec_n(regs->sl[i]), el_count, el_size);
        regs->sh[i] = a64op_make_vec(a64op_vec_n(regs->sh[i]), el_count, el_size);
        regs->dl[i] = a64op_make_vec(a64op_vec_n(regs->dl[i]), el_count, el_size);
        regs->dh[i] = a64op_make_vec(a64op_vec_n(regs->dh[i]), el_count, el_size);
        regs->s2[i] = a64op_make_vec(a64op_vec_n(regs->s2[i]), el_count, el_size);
        regs->s3[i] = a64op_make_vec(a64op_vec_n(regs->s3[i]), el_count, el_size);
        regs->d2[i] = a64op_make_vec(a64op_vec_n(regs->d2[i]), el_count, el_size);
        regs->d3[i] = a64op_make_vec(a64op_vec_n(regs->d3[i]), el_count, el_size);
    }
}

/* Reshape temp vector registers for current SwsOp. */
static void reshape_temp_vectors(SwsAArch64OpRegs *regs, int el_count, int el_size)
{
    for (int i = 0; i < FF_ARRAY_ELEMS(regs->vt); i++)
        regs->vt[i] = a64op_make_vec(a64op_vec_n(regs->vt[i]), el_count, el_size);
}

/* Reshape const vector registers for current SwsOp. */
static void reshape_const_vectors(SwsAArch64OpRegs *regs, int el_count, int el_size)
{
    for (int i = 0; i < FF_ARRAY_ELEMS(regs->vk); i++)
        regs->vk[i] = a64op_make_vec(a64op_vec_n(regs->vk[i]), el_count, el_size);
}

/*********************************************************************/
/* Function frame */

static unsigned clobbered_frame_size(unsigned n)
{
    return ((n + 1) >> 1) * 16;
}

static void asmgen_prologue(SwsAArch64Context *s, const RasmOp *regs, unsigned n)
{
    RasmContext *r = s->rctx;
    RasmOp sp = a64op_sp();
    unsigned frame_size = clobbered_frame_size(n);
    RasmOp sp_pre = a64op_pre(sp, -frame_size);

    rasm_add_comment(r, "prologue");
    if (n == 0) {
        /* no-op */
    } else if (n == 1) {
        i_str(r, regs[0], sp_pre);
    } else {
        i_stp(r, regs[0], regs[1], sp_pre);
        for (unsigned i = 2; i + 1 < n; i += 2)
            i_stp(r, regs[i],     regs[i + 1], a64op_off(sp, i * sizeof(uint64_t)));
        if (n & 1)
            i_str(r, regs[n - 1],              a64op_off(sp, (n - 1) * sizeof(uint64_t)));
    }
}

static void asmgen_epilogue(SwsAArch64Context *s, const RasmOp *regs, unsigned n)
{
    RasmContext *r = s->rctx;
    RasmOp sp = a64op_sp();
    unsigned frame_size = clobbered_frame_size(n);
    RasmOp sp_post = a64op_post(sp, frame_size);

    rasm_add_comment(r, "epilogue");
    if (n == 0) {
        /* no-op */
    } else if (n == 1) {
        i_ldr(r, regs[0], sp_post);
    } else {
        if (n & 1)
            i_ldr(r, regs[n - 1],              a64op_off(sp, (n - 1) * sizeof(uint64_t)));
        for (unsigned i = (n & ~1u) - 2; i >= 2; i -= 2)
            i_ldp(r, regs[i],     regs[i + 1], a64op_off(sp, i * sizeof(uint64_t)));
        i_ldp(r, regs[0], regs[1], sp_post);
    }
}

/*********************************************************************/
/* Callee-saved registers (r19-r28, fp, and lr). */
#define MAX_SAVED_REGS 12

static void clobber_gpr(RasmOp regs[MAX_SAVED_REGS], unsigned *count,
                        RasmOp gpr)
{
    const int n = a64op_gpr_n(gpr);
    if (n >= 19 && n <= 30)
        regs[(*count)++] = gpr;
}

static unsigned clobbered_gprs(const SwsAArch64Context *s,
                               SwsCompMask imask, SwsCompMask omask,
                               RasmOp regs[MAX_SAVED_REGS])
{
    unsigned count = 0;
    clobber_gpr(regs, &count, a64op_lr());
    LOOP(imask, i) {
        clobber_gpr(regs, &count, s->in[i]);
        clobber_gpr(regs, &count, s->in_bump[i]);
    }
    LOOP(omask, i) {
        clobber_gpr(regs, &count, s->out[i]);
        clobber_gpr(regs, &count, s->out_bump[i]);
    }
    return count;
}

static void asmgen_process(SwsAArch64Context *s, SwsCompMask imask, SwsCompMask omask)
{
    RasmContext *r = s->rctx;

    /**
     * The process function for aarch64 works similarly to the x86 backend.
     * The description in x86/ops_include.asm mostly holds as well here.
     */

    /* Function prologue */
    RasmOp saved_regs[MAX_SAVED_REGS];
    unsigned nsaved = clobbered_gprs(s, imask, omask, saved_regs);
    if (nsaved)
        asmgen_prologue(s, saved_regs, nsaved);

    /* Banks 2 and 3 of 32-bit kernels with block_size 16 use v8-v15. */
    RasmOp saved_vregs[8];
    for (int i = 0; i < 8; i++)
        saved_vregs[i] = v_d(a64op_vec(8 + i));
    asmgen_prologue(s, saved_vregs, 8);

    /* Load values from exec. */
    RasmOp exec_in[4];
    RasmOp exec_in_bump[4];
    RasmOp exec_out[4];
    RasmOp exec_out_bump[4];
    LOOP(imask, i) { exec_in      [i] = a64op_off(s->exec, offsetof_exec_in       + (i * sizeof(uint8_t *))); }
    LOOP(imask, i) { exec_in_bump [i] = a64op_off(s->exec, offsetof_exec_in_bump  + (i * sizeof(uint8_t *))); }
    LOOP(omask, i) { exec_out     [i] = a64op_off(s->exec, offsetof_exec_out      + (i * sizeof(uint8_t *))); }
    LOOP(omask, i) { exec_out_bump[i] = a64op_off(s->exec, offsetof_exec_out_bump + (i * sizeof(uint8_t *))); }
    LOOP(imask, i) { i_ldr(r, s->in[i],       exec_in [i]);         CMTF("in[%u] = exec->in[%u];", i, i); }
    LOOP(omask, i) { i_ldr(r, s->out[i],      exec_out[i]);         CMTF("out[%u] = exec->out[%u];", i, i); }
    LOOP(imask, i) { i_ldr(r, s->in_bump[i],  exec_in_bump[i]);     CMTF("in_bump[%u] = exec->in_bump[%u];", i, i); }
    LOOP(omask, i) { i_ldr(r, s->out_bump[i], exec_out_bump[i]);    CMTF("out_bump[%u] = exec->out_bump[%u];", i, i); }

    /* Setup. */
    s->setup = rasm_get_current_node(r);

    int first_row  = rasm_new_label(r, NULL);
    int next_row   = rasm_new_label(r, NULL);
    int next_block = rasm_new_label(r, NULL);

    /* Jump to first row (skips padding). */
    i_b  (r, rasm_op_label(first_row));     CMT("goto first_row;");

    /* Perform padding, preparing for next row. */
    rasm_add_label(r, next_row);            CMT("next_row:");
    LOOP(imask, i) { i_add(r, s->in[i],  s->in[i],  s->in_bump[i]);  CMTF("in[%u] += in_bump[%u];", i, i); }
    LOOP(omask, i) { i_add(r, s->out[i], s->out[i], s->out_bump[i]); CMTF("out[%u] += out_bump[%u];", i, i); }

    /* First row (reset x). */
    rasm_add_label(r, first_row);           CMT("first_row:");
    i_mov(r, s->bx, s->bx_start);           CMT("bx = bx_start;");

    /* Main loop. */
    rasm_add_label(r, next_block);          CMT("next_block:");
    s->loop = rasm_get_current_node(r);

    /* Perform horizontal loop. */
    i_add(r, s->bx, s->bx, IMM(1));         CMT("bx += 1;");
    i_cmp(r, s->bx, s->bx_end);             CMT("if (bx != bx_end)");
    i_bne(r, next_block);                   CMT("    goto next_block;");

    /* Perform vertical loop. */
    i_add(r, s->y, s->y, IMM(1));           CMT("y += 1;");
    i_cmp(r, s->y, s->y_end);               CMT("if (y != y_end)");
    i_bne(r, next_row);                     CMT("    goto next_row;");

    /* Function epilogue */
    asmgen_epilogue(s, saved_vregs, 8);
    if (nsaved)
        asmgen_epilogue(s, saved_regs, nsaved);

    i_ret(r);
}

/*********************************************************************/
/* gather raw pixels from planes */
/* SWS_UOP_READ_BIT */
/* SWS_UOP_READ_NIBBLE */
/* SWS_UOP_READ_PACKED */
/* SWS_UOP_READ_PLANAR */

static void asmgen_op_read_bit(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                               SwsAArch64OpRegs *regs)
{
    RasmContext *r = s->rctx;
    AArch64VecViews dl[1]       = { a64op_vec_views(regs->dl[0]) };
    AArch64VecViews shift_vec   = a64op_vec_views(regs->vk[0]);
    AArch64VecViews bitmask_vec = a64op_vec_views(regs->vk[1]);

    AArch64VecViews vtmp = a64op_vec_views(regs->vt[0]);
    RasmOp wtmp          = a64op_w(s->tmp0);

    /* Note that shift_vec has negative values, so that using it with
     * ushl actually performs a right shift. */
    if (p->block_size == 16) {
        i_ldrh(r, wtmp,        a64op_post(s->in[0], 2));    CMT("uint16_t tmp = *in[0]++;");
        i_dup (r, dl[0].b8,    wtmp);                       CMT("vl[0].lo = broadcast(tmp);");
        i_lsr (r, wtmp,        wtmp, IMM(8));               CMT("tmp >>= 8;");
        i_dup (r, vtmp.b8,     wtmp);                       CMT("vtmp.lo = broadcast(tmp);");
        i_ins (r, dl[0].de[1], vtmp.de[0]);                 CMT("vl[0].hi = vtmp.lo;");
        i_ushl(r, dl[0].b16,   dl[0].b16, shift_vec.b16);   CMT("vl[0] <<= shift_vec;");
        i_and (r, dl[0].b16,   dl[0].b16, bitmask_vec.b16); CMT("vl[0] &= bitmask_vec;");
    } else {
        i_ldrb(r, wtmp,        a64op_post(s->in[0], 1));    CMT("uint8_t tmp = *in[0]++;");
        i_dup (r, dl[0].b8,    wtmp);                       CMT("vl[0].lo = broadcast(tmp);");
        i_ushl(r, dl[0].b8,    dl[0].b8,  shift_vec.b8);    CMT("vl[0] <<= shift_vec;");
        i_and (r, dl[0].b8,    dl[0].b8,  bitmask_vec.b8);  CMT("vl[0] &= bitmask_vec;");
    }
}

static void asmgen_op_read_nibble(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                                  SwsAArch64OpRegs *regs)
{
    RasmContext *r = s->rctx;
    AArch64VecViews dl[1]       = { a64op_vec_views(regs->dl[0]) };
    AArch64VecViews nibble_mask = a64op_vec_views(regs->vk[0]);

    AArch64VecViews vtmp  = a64op_vec_views(regs->vt[0]);

    if (p->block_size == 8) {
        i_ldr (r, dl[0].s,   a64op_post(s->in[0], 4));  CMT("vl[0] = *in[0]++;");
        i_ushr(r, vtmp.b8,   dl[0].b8, IMM(4));         CMT("vtmp.lo = vl[0] >> 4;");
        i_and (r, dl[0].b8,  dl[0].b8, nibble_mask.b8); CMT("vl[0].lo &= nibble_mask;");
        i_zip1(r, dl[0].b8,  vtmp.b8,  dl[0].b8);       CMT("interleave");
    } else {
        i_ldr (r, dl[0].d,   a64op_post(s->in[0], 8));  CMT("vl[0] = *in[0]++;");
        i_ushr(r, vtmp.b8,   dl[0].b8, IMM(4));         CMT("vtmp.lo = vl[0] >> 4;");
        i_and (r, dl[0].b8,  dl[0].b8, nibble_mask.b8); CMT("vl[0].lo &= nibble_mask;");
        i_zip1(r, dl[0].b16, vtmp.b16, dl[0].b16);      CMT("interleave");
    }
}

static void asmgen_op_read_packed_n(SwsAArch64Context *s, const SwsAArch64OpImplParams *p, RasmOp *vx)
{
    RasmContext *r = s->rctx;

    switch (p->mask) {
    case SWS_COMP_ELEMS(2): i_ld2(r, vv_2(vx[0], vx[1]),               a64op_post(s->in[0], s->vec_size * 2)); break;
    case SWS_COMP_ELEMS(3): i_ld3(r, vv_3(vx[0], vx[1], vx[2]),        a64op_post(s->in[0], s->vec_size * 3)); break;
    case SWS_COMP_ELEMS(4): i_ld4(r, vv_4(vx[0], vx[1], vx[2], vx[3]), a64op_post(s->in[0], s->vec_size * 4)); break;
    }
}

static void asmgen_op_read_packed(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                                  SwsAArch64OpRegs *regs)
{
    av_assert0(p->mask != 0x0001);
    asmgen_op_read_packed_n(s, p, regs->dl);
    if (s->use_vh)
        asmgen_op_read_packed_n(s, p, regs->dh);
}

static void asmgen_op_read_planar(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                                  SwsAArch64OpRegs *regs)
{
    RasmContext *r = s->rctx;
    AArch64VecViews dl[4] = A64OP_VEC_VIEWS4(regs->dl);
    AArch64VecViews dh[4] = A64OP_VEC_VIEWS4(regs->dh);

    LOOP_MASK(p, i) {
        if (s->nbanks == 4) {
            i_ldp(r, dl[i].q, dh[i].q, a64op_post(s->in[i], 32));
            i_ldp(r, v_q(regs->d2[i]), v_q(regs->d3[i]), a64op_post(s->in[i], 32));
            continue;
        }
        switch ((s->use_vh ? 0x100 : 0) | s->vec_size) {
        case 0x008: i_ldr(r, dl[i].d,          a64op_post(s->in[i], s->vec_size * 1)); break;
        case 0x010: i_ldr(r, dl[i].q,          a64op_post(s->in[i], s->vec_size * 1)); break;
        case 0x108: i_ldp(r, dl[i].d, dh[i].d, a64op_post(s->in[i], s->vec_size * 2)); break;
        case 0x110: i_ldp(r, dl[i].q, dh[i].q, a64op_post(s->in[i], s->vec_size * 2)); break;
        }
    }
}

/*********************************************************************/
/* write raw pixels to planes */
/* SWS_UOP_WRITE_BIT */
/* SWS_UOP_WRITE_NIBBLE */
/* SWS_UOP_WRITE_PACKED */
/* SWS_UOP_WRITE_PLANAR */

static void asmgen_op_write_bit(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                                SwsAArch64OpRegs *regs)
{
    RasmContext *r = s->rctx;
    AArch64VecViews sl[1]     = { a64op_vec_views(regs->sl[0]) };
    AArch64VecViews shift_vec = a64op_vec_views(regs->vk[0]);

    AArch64VecViews vtmp0 = a64op_vec_views(regs->vt[0]);
    AArch64VecViews vtmp1 = a64op_vec_views(regs->vt[1]);

    if (p->block_size == 8) {
        i_ushl(r, sl[0].b8,    sl[0].b8,   shift_vec.b8);   CMT("vl[0] <<= shift_vec;");
        i_addv(r, vtmp0.b,     sl[0].b8);                   CMT("vtmp0[0] = add_across(vl[0].lo);");
        i_str (r, vtmp0.b,     a64op_post(s->out[0], 1));   CMT("*out[0]++ = vtmp0;");
    } else {
        i_ushl(r, sl[0].b16,   sl[0].b16,  shift_vec.b16);  CMT("vl[0] <<= shift_vec;");
        i_addv(r, vtmp0.b,     sl[0].b8);                   CMT("vtmp0[0] = add_across(vl[0].lo);");
        i_ins (r, vtmp1.de[0], sl[0].de[1]);                CMT("vtmp1.lo = vl[0].hi;");
        i_addv(r, vtmp1.b,     vtmp1.b8);                   CMT("vtmp1[0] = add_across(vtmp1);");
        i_ins (r, vtmp0.be[1], vtmp1.be[0]);                CMT("vtmp0[1] = vtmp1[0];");
        i_str (r, vtmp0.h,     a64op_post(s->out[0], 2));   CMT("*out[0]++ = vtmp0;");
    }
}

static void asmgen_op_write_nibble(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                                   SwsAArch64OpRegs *regs)
{
    RasmContext *r = s->rctx;
    AArch64VecViews sl[4] = A64OP_VEC_VIEWS4(regs->sl);
    AArch64VecViews vtmp0 = a64op_vec_views(regs->vt[0]);
    AArch64VecViews vtmp1 = a64op_vec_views(regs->vt[1]);

    if (p->block_size == 8) {
        i_shl (r, vtmp0.h4,  sl[0].h4,  IMM(4));
        i_ushr(r, vtmp1.h4,  sl[0].h4,  IMM(8));
        i_orr (r, sl[0].b8,  vtmp0.b8,  vtmp1.b8);
        i_xtn (r, vtmp0.b8,  sl[0].h8);
        i_str (r, vtmp0.s,   a64op_post(s->out[0], 4));
    } else {
        i_shl (r, vtmp0.h8,  sl[0].h8,  IMM(4));
        i_ushr(r, vtmp1.h8,  sl[0].h8,  IMM(8));
        i_orr (r, sl[0].b16, vtmp0.b16, vtmp1.b16);
        i_xtn (r, vtmp0.b8,  sl[0].h8);
        i_str (r, vtmp0.d,   a64op_post(s->out[0], 8));
    }
}

static void asmgen_op_write_packed_n(SwsAArch64Context *s, const SwsAArch64OpImplParams *p, RasmOp *vx)
{
    RasmContext *r = s->rctx;

    switch (p->mask) {
    case SWS_COMP_ELEMS(2): i_st2(r, vv_2(vx[0], vx[1]),               a64op_post(s->out[0], s->vec_size * 2)); break;
    case SWS_COMP_ELEMS(3): i_st3(r, vv_3(vx[0], vx[1], vx[2]),        a64op_post(s->out[0], s->vec_size * 3)); break;
    case SWS_COMP_ELEMS(4): i_st4(r, vv_4(vx[0], vx[1], vx[2], vx[3]), a64op_post(s->out[0], s->vec_size * 4)); break;
    }
}

static void asmgen_op_write_packed(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                                   SwsAArch64OpRegs *regs)
{
    av_assert0(p->mask != 0x0001);
    asmgen_op_write_packed_n(s, p, regs->sl);
    if (s->use_vh)
        asmgen_op_write_packed_n(s, p, regs->sh);
}

static void asmgen_op_write_planar(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                                   SwsAArch64OpRegs *regs)
{
    RasmContext *r = s->rctx;
    AArch64VecViews sl[4] = A64OP_VEC_VIEWS4(regs->sl);
    AArch64VecViews sh[4] = A64OP_VEC_VIEWS4(regs->sh);

    LOOP_MASK(p, i) {
        if (s->nbanks == 4) {
            i_stp(r, sl[i].q, sh[i].q, a64op_post(s->out[i], 32));
            i_stp(r, v_q(regs->s2[i]), v_q(regs->s3[i]), a64op_post(s->out[i], 32));
            continue;
        }
        switch ((s->use_vh ? 0x100 : 0) | s->vec_size) {
        case 0x008: i_str(r, sl[i].d,          a64op_post(s->out[i], s->vec_size * 1)); break;
        case 0x010: i_str(r, sl[i].q,          a64op_post(s->out[i], s->vec_size * 1)); break;
        case 0x108: i_stp(r, sl[i].d, sh[i].d, a64op_post(s->out[i], s->vec_size * 2)); break;
        case 0x110: i_stp(r, sl[i].q, sh[i].q, a64op_post(s->out[i], s->vec_size * 2)); break;
        }
    }
}

/*********************************************************************/
/* swap byte order (for differing endianness) */
/* SWS_UOP_SWAP_BYTES */

static void asmgen_op_swap_bytes(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                                 SwsAArch64OpRegs *regs)
{
    RasmContext *r = s->rctx;
    AArch64VecViews sl[4] = A64OP_VEC_VIEWS4(regs->sl);
    AArch64VecViews sh[4] = A64OP_VEC_VIEWS4(regs->sh);
    AArch64VecViews dl[4] = A64OP_VEC_VIEWS4(regs->dl);
    AArch64VecViews dh[4] = A64OP_VEC_VIEWS4(regs->dh);

    switch (ff_sws_pixel_type_size(p->type)) {
    case sizeof(uint16_t):
        LOOP_MASK      (p, i) i_rev16(r, dl[i].b16, sl[i].b16);
        LOOP_MASK_VH(s, p, i) i_rev16(r, dh[i].b16, sh[i].b16);
        break;
    case sizeof(uint32_t):
        LOOP_MASK      (p, i) i_rev32(r, dl[i].b16, sl[i].b16);
        LOOP_MASK_VH(s, p, i) i_rev32(r, dh[i].b16, sh[i].b16);
        LOOP_MASK_B23(s, p, b, i) i_rev32(r, v_16b(bank_dst(regs, b)[i]), v_16b(bank_src(regs, b)[i]));
        break;
    }
}

/*********************************************************************/
/* rearrange channel order, or duplicate channels */
/* SWS_UOP_PERMUTE */
/* SWS_UOP_COPY */

static const char *print_swizzle_v(char buf[8], int8_t n, int bank)
{
    if (n == -1)
        snprintf(buf, sizeof(char[8]), "vtmp%c", "lh23"[bank]);
    else
        snprintf(buf, sizeof(char[8]), "v%c[%u]", "lh23"[bank], n);
    return buf;
}
#define PRINT_SWIZZLE_V(n, bank) print_swizzle_v((char[8]){ 0 }, n, bank)

static RasmOp swizzle_a64op(SwsAArch64OpRegs *regs, int8_t n, int bank, bool dst)
{
    if (n == -1)
        return regs->vt[bank];
    return dst ? bank_dst(regs, bank)[n] : bank_src(regs, bank)[n];
}

static void swizzle_emit(SwsAArch64Context *s, SwsAArch64OpRegs *regs,
                         int8_t dst, int8_t src)
{
    RasmContext *r = s->rctx;
    for (int b = 0; b < s->nbanks; b++) {
        RasmOp src_op = swizzle_a64op(regs, src, b, false);
        RasmOp dst_op = swizzle_a64op(regs, dst, b, true);
        i_mov(r, dst_op, src_op); CMTF("%s = %s;", PRINT_SWIZZLE_V(dst, b), PRINT_SWIZZLE_V(src, b));
    }
}

static void asmgen_op_move(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                           SwsAArch64OpRegs *regs)
{
    for (int i = 0; i < p->par.move.num_moves; i++)
        swizzle_emit(s, regs, p->par.move.dst[i], p->par.move.src[i]);
}

/*********************************************************************/
/* split tightly packed data into components */
/* SWS_UOP_UNPACK */

static void asmgen_op_unpack(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                             SwsAArch64OpRegs *regs)
{
    RasmContext *r = s->rctx;
    RasmOp *vmask = regs->vk;

    const int offsets[4] = {
        p->par.pack.pattern[3] + p->par.pack.pattern[2] + p->par.pack.pattern[1],
        p->par.pack.pattern[3] + p->par.pack.pattern[2],
        p->par.pack.pattern[3],
        0
    };

    /**
     * Loop backwards to avoid clobbering component 0. The component at
     * offset 0 is masked directly from component 0, and the topmost field
     * doesn't need masking if it extends up to the most significant bit.
     */
    SwsCompMask and_mask = 0;
    for (int b = 0; b < s->nbanks; b++) {
        RasmOp *sx = bank_src(regs, b);
        RasmOp *dx = bank_dst(regs, b);
        LOOP_MASK_BWD(p, i) {
            if (offsets[i]) {
                i_ushr  (r, dx[i], sx[0], IMM(offsets[i])); CMTF("v%c[%u] >>= %u;", "lh23"[b], i, offsets[i]);
                if (unpack_needs_mask(p, i))
                    and_mask |= SWS_COMP(i);
            } else if (unpack_needs_mask(p, i)) {
                i_and16b(r, dx[i], sx[0], vmask[i]);        CMTF("v%c[%u] = v%c[0] & 0x%x;", "lh23"[b], i, "lh23"[b], (1u << p->par.pack.pattern[i]) - 1);
            } else if (i) {
                i_mov16b(r, dx[i], sx[0]);                  CMTF("v%c[%u] = v%c[0];", "lh23"[b], i, "lh23"[b]);
            }
        }
    }

    /* Apply remaining masks. */
    for (int b = 0; b < s->nbanks; b++) {
        RasmOp *dx = bank_dst(regs, b);
        LOOP(and_mask, i) { i_and16b(r, dx[i], dx[i], vmask[i]); CMTF("v%c[%u] &= 0x%x;", "lh23"[b], i, (1u << p->par.pack.pattern[i]) - 1); }
    }
}

/*********************************************************************/
/* compress components into tightly packed data */
/* SWS_UOP_PACK */

static void asmgen_op_pack(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                           SwsAArch64OpRegs *regs)
{
    RasmContext *r = s->rctx;

    /**
     * Combine the components from the most significant one downwards, by
     * inserting the accumulated value above the next component with sli
     * (Horner's scheme): acc = (acc << (offset[prev] - offset[i])) | x[i].
     * This assumes that each component fits inside its field. The result
     * ends up in the register of the last component, so move it (or shift
     * it, if the last component is not at offset 0) to component 0.
     */
    int prev = -1;
    LOOP_MASK(p, i) {
        if (prev >= 0) {
            const int shift = pack_offset(p, prev) - pack_offset(p, i);
            for (int b = 0; b < s->nbanks; b++) {
                RasmOp *sx = bank_src(regs, b);
                RasmOp *dx = bank_dst(regs, b);
                i_sli(r, dx[i], sx[prev], IMM(shift)); CMTF("v%c[%u] |= v%c[%u] << %u;", "lh23"[b], i, "lh23"[b], prev, shift);
                sx[i] = dx[i];
            }
        }
        prev = i;
    }
    const int last_offset = pack_offset(p, prev);
    for (int b = 0; b < s->nbanks; b++) {
        RasmOp *sx = bank_src(regs, b);
        RasmOp *dx = bank_dst(regs, b);
        const char c = "lh23"[b];
        if (last_offset) {
            i_shl   (r, dx[0], sx[prev], IMM(last_offset)); CMTF("v%c[0] = v%c[%u] << %u;", c, c, prev, last_offset);
        } else if (prev > 0) {
            i_mov16b(r, dx[0], sx[prev]);                   CMTF("v%c[0] = v%c[%u];", c, c, prev);
        }
    }
}

/*********************************************************************/
/* logical left shift of raw pixel values */
/* SWS_UOP_LSHIFT */

static void asmgen_op_lshift(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                             SwsAArch64OpRegs *regs)
{
    uint8_t shift = p->par.shift.amount;
    RasmContext *r = s->rctx;
    RasmOp *sl = regs->sl;
    RasmOp *sh = regs->sh;
    RasmOp *dl = regs->dl;
    RasmOp *dh = regs->dh;

    LOOP_MASK      (p, i) { i_shl(r, dl[i], sl[i], IMM(shift)); CMTF("vl[%u] <<= %u;", i, shift); }
    LOOP_MASK_VH(s, p, i) { i_shl(r, dh[i], sh[i], IMM(shift)); CMTF("vh[%u] <<= %u;", i, shift); }
}

/*********************************************************************/
/* right shift of raw pixel values */
/* SWS_UOP_RSHIFT */

static void asmgen_op_rshift(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                             SwsAArch64OpRegs *regs)
{
    uint8_t shift = p->par.shift.amount;
    RasmContext *r = s->rctx;
    RasmOp *sl = regs->sl;
    RasmOp *sh = regs->sh;
    RasmOp *dl = regs->dl;
    RasmOp *dh = regs->dh;

    LOOP_MASK      (p, i) { i_ushr(r, dl[i], sl[i], IMM(shift)); CMTF("vl[%u] >>= %u;", i, shift); }
    LOOP_MASK_VH(s, p, i) { i_ushr(r, dh[i], sh[i], IMM(shift)); CMTF("vh[%u] >>= %u;", i, shift); }
}

/*********************************************************************/
/* clear pixel values */
/* SWS_UOP_CLEAR */

static void emit_clear(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                       RasmOp *vx, int i, const char *vx_str)
{
    RasmContext *r = s->rctx;
    if (p->par.clear.zero & SWS_COMP(i)) {
        i_movi(r, vx[i], IMM(0));                   CMTF("%s[%u] = 0;", vx_str, i);
    } else if (p->par.clear.one & SWS_COMP(i)) {
        if (p->block_size * ff_sws_pixel_type_size(p->type) == 8) {
            i_movi(r, v_8b (vx[i]), IMM(0xff));
        } else {
            i_movi(r, v_16b(vx[i]), IMM(0xff));
        }
        CMTF("%s[%u] = UINT_MAX;", vx_str, i);
    }
    /* Other values have already been loaded by asmgen_setup_clear(). */
}

static void asmgen_op_clear(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                            SwsAArch64OpRegs *regs)
{
    RasmOp *dl = regs->dl;
    RasmOp *dh = regs->dh;

    LOOP_MASK      (p, i) { emit_clear(s, p, dl, i, "vl"); }
    LOOP_MASK_VH(s, p, i) { emit_clear(s, p, dh, i, "vh"); }
    LOOP_MASK_B23(s, p, b, i) { emit_clear(s, p, bank_dst(regs, b), i, b == 2 ? "v2" : "v3"); }
}

/*********************************************************************/
/* convert (cast) between formats */
/* SWS_UOP_TO_U8 */
/* SWS_UOP_TO_U16 */
/* SWS_UOP_TO_U32 */
/* SWS_UOP_TO_F32 */

static void asmgen_op_convert(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                              SwsAArch64OpRegs *regs)
{
    RasmContext *r = s->rctx;
    AArch64VecViews sl[4] = A64OP_VEC_VIEWS4(regs->sl);
    AArch64VecViews sh[4] = A64OP_VEC_VIEWS4(regs->sh);
    AArch64VecViews dl[4] = A64OP_VEC_VIEWS4(regs->dl);
    AArch64VecViews dh[4] = A64OP_VEC_VIEWS4(regs->dh);

    /**
     * Since each instruction in the convert operation needs specific
     * element types, it is simpler to use arrangement specifiers for
     * each operand instead of reshaping all vectors.
     */

    size_t src_el_size = s->el_size;
    SwsPixelType to_type;
    switch (p->uop) {
    case SWS_UOP_TO_U8:  to_type = SWS_PIXEL_U8;  break;
    case SWS_UOP_TO_U16: to_type = SWS_PIXEL_U16; break;
    case SWS_UOP_TO_U32: to_type = SWS_PIXEL_U32; break;
    case SWS_UOP_TO_F32: to_type = SWS_PIXEL_F32; break;
    case SWS_UOP_SAT_U8:  to_type = SWS_PIXEL_U8;  break;
    case SWS_UOP_SAT_U16: to_type = SWS_PIXEL_U16; break;
    default:
        av_assert0(!"Invalid uop!");
        break;
    }
    size_t dst_el_size = ff_sws_pixel_type_size(to_type);

    /**
     * Saturating conversions from unsigned integers use the saturating
     * narrowing instructions (the lower bound is implicit).
     */
    const bool sat = (p->uop == SWS_UOP_SAT_U8 || p->uop == SWS_UOP_SAT_U16);

    /**
     * Saturating conversions from f32 convert to fixed-point, with the
     * integer part in the top bits of each u32 lane: fcvtzu saturates at 0
     * and exactly at the upper bound of the destination type, and narrowing
     * just keeps the top halves (uzp2), which executes on more pipelines
     * than uqxtn on some cores.
     */
    const bool satfx = sat && p->type == SWS_PIXEL_F32;
    const RasmOp fbits = IMM(satfx ? 32 - 8 * dst_el_size : 0);

    /**
     * Zero-extension is done by interleaving with a zero vector (zip1/zip2),
     * since uxtl/uxtl2 (ushll) only execute on one pipeline on some cores.
     */
    AArch64VecViews vzero = a64op_vec_views(regs->vt[11]);
    bool have_zero = false;
#define NEED_ZERO() do {                                                    \
        if (!have_zero) {                                                   \
            i_movi(r, vzero.d2, IMM(0));                                    \
            have_zero = true;                                               \
        }                                                                   \
    } while (0)

    /**
     * This function assumes block_size is either 8 or 16, and that
     * we're always using the most amount of vector registers possible.
     * Therefore, u32 always uses the high vector bank, and also banks 2
     * and 3 with block_size 16.
     */
    if (p->type == SWS_PIXEL_F32) {
        rasm_add_comment(r, "f32 -> u32");
        if (satfx) {
            LOOP_MASK(p, i) i_fcvtzu_fx(r, dl[i].s4, sl[i].s4, fbits);
            LOOP_MASK(p, i) i_fcvtzu_fx(r, dh[i].s4, sh[i].s4, fbits);
        } else {
            LOOP_MASK(p, i) i_fcvtzu(r, dl[i].s4, sl[i].s4);
            LOOP_MASK(p, i) i_fcvtzu(r, dh[i].s4, sh[i].s4);
        }
        memcpy(sl, dl, sizeof(sl));
        memcpy(sh, dh, sizeof(sh));
    }

    /* 32-bit elements with block_size 16 use four vector banks. */
    AArch64VecViews d2[4] = A64OP_VEC_VIEWS4(regs->d2);
    AArch64VecViews d3[4] = A64OP_VEC_VIEWS4(regs->d3);
    const bool x4 = p->block_size == 16 && (src_el_size == 4 || dst_el_size == 4);
    if (x4 && src_el_size == 4 && dst_el_size == 4) {
        /* f32 <-> u32 */
        if (p->type == SWS_PIXEL_F32) {
            rasm_add_comment(r, "f32 -> u32 (banks 2-3)");
            LOOP_MASK(p, i) i_fcvtzu(r, d2[i].s4, d2[i].s4);
            LOOP_MASK(p, i) i_fcvtzu(r, d3[i].s4, d3[i].s4);
        } else if (to_type == SWS_PIXEL_F32) {
            rasm_add_comment(r, "u32 -> f32");
            LOOP_MASK(p, i) i_ucvtf(r, dl[i].s4, sl[i].s4);
            LOOP_MASK(p, i) i_ucvtf(r, dh[i].s4, sh[i].s4);
            LOOP_MASK(p, i) i_ucvtf(r, d2[i].s4, d2[i].s4);
            LOOP_MASK(p, i) i_ucvtf(r, d3[i].s4, d3[i].s4);
        }
        return;
    } else if (x4 && src_el_size == 4) {
        if (satfx) {
            rasm_add_comment(r, "f32 -> u32 (banks 2-3)");
            LOOP_MASK(p, i) i_fcvtzu_fx(r, d2[i].s4, d2[i].s4, fbits);
            LOOP_MASK(p, i) i_fcvtzu_fx(r, d3[i].s4, d3[i].s4, fbits);
        } else if (p->type == SWS_PIXEL_F32) {
            rasm_add_comment(r, "f32 -> u32 (banks 2-3)");
            LOOP_MASK(p, i) i_fcvtzu(r, d2[i].s4, d2[i].s4);
            LOOP_MASK(p, i) i_fcvtzu(r, d3[i].s4, d3[i].s4);
        }
        rasm_add_comment(r, "u32 -> u16");
        if (satfx) {
            LOOP_MASK(p, i) i_uzp2  (r, dl[i].h8, sl[i].h8, sh[i].h8);
            LOOP_MASK(p, i) i_uzp2  (r, dh[i].h8, d2[i].h8, d3[i].h8);
        } else if (sat) {
            LOOP_MASK(p, i) i_uqxtn (r, dl[i].h4, sl[i].s4);
            LOOP_MASK(p, i) i_uqxtn2(r, dl[i].h8, sh[i].s4);
            LOOP_MASK(p, i) i_uqxtn (r, dh[i].h4, d2[i].s4);
            LOOP_MASK(p, i) i_uqxtn2(r, dh[i].h8, d3[i].s4);
        } else {
            LOOP_MASK(p, i) i_uzp1  (r, dl[i].h8, sl[i].h8, sh[i].h8);
            LOOP_MASK(p, i) i_uzp1  (r, dh[i].h8, d2[i].h8, d3[i].h8);
        }
        memcpy(sl, dl, sizeof(sl));
        memcpy(sh, dh, sizeof(sh));
        src_el_size = 2;
        /* u16 -> u8 is handled below. */
    } else if (x4) {
        av_assert0(dst_el_size == 4 && src_el_size < 4);
        /**
         * Widen banks 3 to 0 (bank 0 last, since it shares the registers of
         * the input), and convert each bank to f32 after widening the next
         * one, so that both can execute in parallel.
         */
        const bool tbl = convert_u8_tbl(p);
        AArch64VecViews *db[4] = { dl, dh, d2, d3 };
        rasm_add_comment(r, tbl ? "u8 -> u32 -> f32" : "u16 -> u32 -> f32");
        if (!tbl)
            NEED_ZERO();
        for (int b = 3; b >= -1; b--) {
            if (b >= 0) {
                AArch64VecViews *d = db[b];
                if (tbl) {
                    LOOP_MASK(p, i) i_tbl(r, d[i].b16, vv_1(sl[i].b16), v_16b(regs->vk[b]));
                } else {
                    AArch64VecViews *src = (b >= 2) ? sh : sl;
                    if (b & 1) {
                        LOOP_MASK(p, i) i_zip2(r, d[i].h8, src[i].h8, vzero.h8);
                    } else {
                        LOOP_MASK(p, i) i_zip1(r, d[i].h8, src[i].h8, vzero.h8);
                    }
                }
            }
            if (to_type == SWS_PIXEL_F32 && b < 3) {
                AArch64VecViews *d = db[b + 1];
                LOOP_MASK(p, i) i_ucvtf(r, d[i].s4, d[i].s4);
            }
        }
        return;
    }

    if (convert_u8_tbl(p)) {
        rasm_add_comment(r, "u8 -> u32");
        RasmOp idx_lo = v_16b(regs->vk[0]);
        RasmOp idx_hi = v_16b(regs->vk[1]);
        LOOP_MASK(p, i) i_tbl(r, dh[i].b16, vv_1(sl[i].b16), idx_hi);
        LOOP_MASK(p, i) i_tbl(r, dl[i].b16, vv_1(sl[i].b16), idx_lo);
        memcpy(sl, dl, sizeof(sl));
        memcpy(sh, dh, sizeof(sh));
        src_el_size = 4;
    } else if (p->block_size == 8) {
        if (src_el_size == 1 && dst_el_size > src_el_size) {
            rasm_add_comment(r, "u8 -> u16");
            NEED_ZERO();
            LOOP_MASK(p, i) i_zip1 (r, dl[i].b16,   sl[i].b16,   vzero.b16);
            memcpy(sl, dl, sizeof(sl));
            src_el_size = 2;
        } else if (src_el_size == 4 && dst_el_size < src_el_size) {
            rasm_add_comment(r, "u32 -> u16");
            if (satfx) {
                LOOP_MASK(p, i) i_uzp2  (r, dl[i].h8, sl[i].h8, sh[i].h8);
            } else if (sat) {
                LOOP_MASK(p, i) i_uqxtn (r, dl[i].h4, sl[i].s4);
                LOOP_MASK(p, i) i_uqxtn2(r, dl[i].h8, sh[i].s4);
            } else {
                /* uzp1 narrows both halves at once (even elements = low halves). */
                LOOP_MASK(p, i) i_uzp1  (r, dl[i].h8, sl[i].h8, sh[i].h8);
            }
            memcpy(sl, dl, sizeof(sl));
            memcpy(sh, dh, sizeof(sh));
            src_el_size = 2;
        }
        if (src_el_size == 2 && dst_el_size == 4) {
            rasm_add_comment(r, "u16 -> u32");
            NEED_ZERO();
            LOOP_MASK(p, i) i_zip2 (r, dh[i].h8,    sl[i].h8,    vzero.h8);
            LOOP_MASK(p, i) i_zip1 (r, dl[i].h8,    sl[i].h8,    vzero.h8);
            memcpy(sl, dl, sizeof(sl));
            memcpy(sh, dh, sizeof(sh));
            src_el_size = 4;
        } else if (src_el_size == 2 && dst_el_size == 1) {
            rasm_add_comment(r, "u16 -> u8");
            if (satfx) {
                /* Only the low half of the result is used. */
                LOOP_MASK(p, i) i_uzp2 (r, dl[i].b16, sl[i].b16, sl[i].b16);
            } else if (sat) {
                LOOP_MASK(p, i) i_uqxtn(r, dl[i].b8, sl[i].h8);
            } else {
                LOOP_MASK(p, i) i_xtn  (r, dl[i].b8, sl[i].h8);
            }
            memcpy(sl, dl, sizeof(sl));
            src_el_size = 1;
        }
    } else /* if (p->block_size == 16) */ {
        if (src_el_size == 1 && dst_el_size == 2) {
            rasm_add_comment(r, "u8 -> u16");
            NEED_ZERO();
            LOOP_MASK(p, i) i_zip2 (r, dh[i].b16,   sl[i].b16,   vzero.b16);
            LOOP_MASK(p, i) i_zip1 (r, dl[i].b16,   sl[i].b16,   vzero.b16);
            memcpy(sl, dl, sizeof(sl));
            memcpy(sh, dh, sizeof(sh));
        } else if (src_el_size == 2 && dst_el_size == 1) {
            rasm_add_comment(r, "u16 -> u8");
            if (satfx) {
                LOOP_MASK(p, i) i_uzp2  (r, dl[i].b16, sl[i].b16, sh[i].b16);
            } else if (sat) {
                LOOP_MASK(p, i) i_uqxtn (r, dl[i].b8,  sl[i].h8);
                LOOP_MASK(p, i) i_uqxtn2(r, dl[i].b16, sh[i].h8);
            } else {
                LOOP_MASK(p, i) i_uzp1  (r, dl[i].b16, sl[i].b16, sh[i].b16);
            }
            memcpy(sl, dl, sizeof(sl));
        }
    }

#undef NEED_ZERO

    /* See comment above for high vector bank usage for u32. */
    if (to_type == SWS_PIXEL_F32) {
        rasm_add_comment(r, "u32 -> f32");
        LOOP_MASK(p, i) i_ucvtf(r, dl[i].s4, sl[i].s4);
        LOOP_MASK(p, i) i_ucvtf(r, dh[i].s4, sh[i].s4);
    }
}

/*********************************************************************/
/* expand integers to the full range */
/* SWS_UOP_EXPAND_PAIR */
/* SWS_UOP_EXPAND_QUAD */

static void asmgen_op_expand(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                             SwsAArch64OpRegs *regs)
{
    RasmContext *r = s->rctx;
    RasmOp *sl = regs->sl;
    RasmOp *dl = regs->dl;
    RasmOp *dh = regs->dh;

    size_t src_el_size = s->el_size;
    SwsPixelType to_type;
    switch (p->uop) {
    case SWS_UOP_EXPAND_PAIR: to_type = SWS_PIXEL_U16; break;
    case SWS_UOP_EXPAND_QUAD: to_type = SWS_PIXEL_U32; break;
    default:
        av_assert0(!"Invalid uop!");
        break;
    }
    size_t dst_el_size = ff_sws_pixel_type_size(to_type);
    size_t dst_total_size = p->block_size * dst_el_size;
    size_t dst_vec_size = FFMIN(dst_total_size, 16);

    if (!s->use_vh)
        s->use_vh = (dst_vec_size != dst_total_size);

    if (src_el_size == 1) {
        rasm_add_comment(r, "u8 -> u16");
        reshape_io_vectors(regs, 16, 1);
        LOOP_MASK_VH(s, p, i) i_zip2(r, dh[i], sl[i], sl[i]);
        LOOP_MASK      (p, i) i_zip1(r, dl[i], sl[i], sl[i]);
        sl = dl;
    }
    if (dst_el_size == 4) {
        rasm_add_comment(r, "u16 -> u32");
        reshape_io_vectors(regs, 8, 2);
        LOOP_MASK_VH(s, p, i) i_zip2(r, dh[i], sl[i], sl[i]);
        LOOP_MASK      (p, i) i_zip1(r, dl[i], sl[i], sl[i]);
    }
}

/*********************************************************************/
/* numeric minimum */
/* SWS_UOP_MIN */

static void asmgen_op_min(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                          SwsAArch64OpRegs *regs)
{
    RasmContext *r = s->rctx;
    RasmOp *sl = regs->sl;
    RasmOp *sh = regs->sh;
    RasmOp *dl = regs->dl;
    RasmOp *dh = regs->dh;
    RasmOp *vk = regs->vk;

    if (p->type == SWS_PIXEL_F32) {
        LOOP_MASK      (p, i) { i_fmin(r, dl[i], sl[i], vk[i]);     CMTF("vl[%u] = min(vl[%u], vmin%u);", i, i, i); }
        LOOP_MASK_VH(s, p, i) { i_fmin(r, dh[i], sh[i], vk[i]);     CMTF("vh[%u] = min(vh[%u], vmin%u);", i, i, i); }
        LOOP_MASK_B23(s, p, b, i) {
            i_fmin(r, bank_dst(regs, b)[i], bank_src(regs, b)[i], vk[i]);
            CMTF("v%c[%u] = min(v%c[%u], vmin%u);", "lh23"[b], i, "lh23"[b], i, i);
        }
    } else {
        LOOP_MASK      (p, i) { i_umin(r, dl[i], sl[i], vk[i]);     CMTF("vl[%u] = min(vl[%u], vmin%u);", i, i, i); }
        LOOP_MASK_VH(s, p, i) { i_umin(r, dh[i], sh[i], vk[i]);     CMTF("vh[%u] = min(vh[%u], vmin%u);", i, i, i); }
    }
}

/*********************************************************************/
/* numeric maximum */
/* SWS_UOP_MAX */

static void asmgen_op_max(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                          SwsAArch64OpRegs *regs)
{
    RasmContext *r = s->rctx;
    RasmOp *sl = regs->sl;
    RasmOp *sh = regs->sh;
    RasmOp *dl = regs->dl;
    RasmOp *dh = regs->dh;
    RasmOp *vk = regs->vk;

    if (p->type == SWS_PIXEL_F32) {
        LOOP_MASK      (p, i) { i_fmax(r, dl[i], sl[i], vk[i]);     CMTF("vl[%u] = max(vl[%u], vmax%u);", i, i, i); }
        LOOP_MASK_VH(s, p, i) { i_fmax(r, dh[i], sh[i], vk[i]);     CMTF("vh[%u] = max(vh[%u], vmax%u);", i, i, i); }
        LOOP_MASK_B23(s, p, b, i) {
            i_fmax(r, bank_dst(regs, b)[i], bank_src(regs, b)[i], vk[i]);
            CMTF("v%c[%u] = max(v%c[%u], vmax%u);", "lh23"[b], i, "lh23"[b], i, i);
        }
    } else {
        LOOP_MASK      (p, i) { i_umax(r, dl[i], sl[i], vk[i]);     CMTF("vl[%u] = max(vl[%u], vmax%u);", i, i, i); }
        LOOP_MASK_VH(s, p, i) { i_umax(r, dh[i], sh[i], vk[i]);     CMTF("vh[%u] = max(vh[%u], vmax%u);", i, i, i); }
    }
}

/*********************************************************************/
/* multiplication by scalar */
/* SWS_UOP_SCALE */

static void asmgen_op_scale(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                            SwsAArch64OpRegs *regs)
{
    RasmContext *r = s->rctx;
    RasmOp *sl       = regs->sl;
    RasmOp *sh       = regs->sh;
    RasmOp *dl       = regs->dl;
    RasmOp *dh       = regs->dh;
    RasmOp scale_vec = regs->vk[0];

    if (p->type == SWS_PIXEL_F32) {
        LOOP_MASK      (p, i) { i_fmul(r, dl[i], sl[i], scale_vec); CMTF("vl[%u] *= scale_vec;", i); }
        LOOP_MASK_VH(s, p, i) { i_fmul(r, dh[i], sh[i], scale_vec); CMTF("vh[%u] *= scale_vec;", i); }
        LOOP_MASK_B23(s, p, b, i) {
            i_fmul(r, bank_dst(regs, b)[i], bank_src(regs, b)[i], scale_vec);
            CMTF("v%c[%u] *= scale_vec;", "lh23"[b], i);
        }
    } else {
        LOOP_MASK      (p, i) { i_mul (r, dl[i], sl[i], scale_vec); CMTF("vl[%u] *= scale_vec;", i); }
        LOOP_MASK_VH(s, p, i) { i_mul (r, dh[i], sh[i], scale_vec); CMTF("vh[%u] *= scale_vec;", i); }
    }
}

/*********************************************************************/
/* generalized linear affine transform */
/* SWS_UOP_LINEAR */
/* SWS_UOP_LINEAR_FMA */

/* Position of the pre-broadcast offset of row `row` in impl->priv. */
static int linear_offset_pos(const SwsAArch64OpImplParams *p, int row)
{
    int pos = 16 * linear_num_vregs(p);
    LOOP_MASK(p, i) {
        if (i == row)
            break;
        if (!(p->par.lin.zero & SWS_MASK_OFF(i)))
            pos += 16;
    }
    return pos;
}

static void linear_load_offset(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                               SwsAArch64OpRegs *regs, int bank, int i)
{
    RasmContext *r = s->rctx;
    const bool full = p->block_size * ff_sws_pixel_type_size(p->type) >= 16;
    RasmOp dst = bank_dst(regs, bank)[i];
    RasmOp src = a64op_off(regs->linear_ptr, linear_offset_pos(p, i));
    i_ldr(r, full ? v_q(dst) : v_d(dst), src);
    CMTF("v%c[%u]  = broadcast(offset[%u]);", "lh23"[bank], i, i);
}

static void linear_pass(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                        SwsAArch64OpRegs *regs, bool vh_pass, SwsCompMask preloaded)
{
    RasmContext *r = s->rctx;
    /**
     * The intermediate registers for fmul+fadd (for when SWS_BITEXACT
     * is set) start from temp vector 8.
     */
    RasmOp *vt = regs->vt;
    RasmOp *vtmp = &vt[8];
    RasmOp *sx = vh_pass ? regs->sh : regs->sl;
    RasmOp *dx = vh_pass ? regs->dh : regs->dl;
    char cvh = vh_pass ? 'h' : 'l';

    if (vh_pass && !s->use_vh)
        return;

    /**
     * The non-zero coefficients have been packed in aarch64_setup_linear()
     * in sequential order into the individual lanes of the coefficient
     * vector registers. We must follow the same order of execution here.
     */
    LOOP_MASK(p, i) {
        bool first = true;
        RasmNode *pre_mul = rasm_get_current_node(r);
        for (int j = 0; j < 5; j++) {
            bool is_offset = (j == 0);
            int src_j = is_offset ? 4 : (j - 1);
            if (p->par.lin.zero & SWS_MASK(i, src_j))
                continue;
            RasmOp vsrc = sx[src_j];
            RasmOp vcoeff = regs->linear_vcoeff[i][j];
            if (first && is_offset) {
                if (!(preloaded & SWS_COMP(i))) {
                    linear_load_offset(s, p, regs, vh_pass, i);
                } else {
                    /* Separates the multiplications inserted at pre_mul from the additions. */
                    rasm_add_commentf(r, (char[64]){0}, 64, "v%c[%u] = offset[%u] (preloaded)", cvh, i, i);
                }
            } else if (first && !is_offset) {
                if (p->par.lin.one & SWS_MASK(i, src_j)) {
                    i_mov16b(r, dx[i], vsrc);           CMTF("v%c[%u]  = vsrc%c[%u];", cvh, i, cvh, src_j);
                } else if (p->type == SWS_PIXEL_F32) {
                    i_fmul  (r, dx[i], vsrc, vcoeff);   CMTF("v%c[%u]  = vsrc%c[%u] * coeff[%u][%u];", cvh, i, cvh, src_j, i, src_j);
                } else {
                    i_mul   (r, dx[i], vsrc, vcoeff);   CMTF("v%c[%u]  = vsrc%c[%u] * coeff[%u][%u];", cvh, i, cvh, src_j, i, src_j);
                }
            } else if (p->type != SWS_PIXEL_F32) {
                /* Integer multiply-accumulate is always exact. */
                i_mla (r, dx[i], vsrc, vcoeff);         CMTF("v%c[%u] += vsrc%c[%u] * coeff[%u][%u];", cvh, i, cvh, src_j, i, src_j);
            } else if (p->uop == SWS_UOP_LINEAR_FMA) {
                /**
                 * Most modern aarch64 cores have a fastpath for sequences
                 * of fmla instructions. This means that even if the coefficient
                 * is 1, it is still faster to use fmla by 1 instead of fadd.
                 */
                i_fmla(r, dx[i], vsrc, vcoeff);         CMTF("v%c[%u] += vsrc%c[%u] * coeff[%u][%u];", cvh, i, cvh, src_j, i, src_j);
            } else {
                /**
                 * Split the multiply-accumulate into fmul+fadd. All
                 * multiplications are performed first into temporary
                 * registers, and only then added to the destination,
                 * to reduce the dependency chain.
                 * There is no need to perform multiplications by 1.
                 */
                if (!(p->par.lin.one & SWS_MASK(i, src_j))) {
                    pre_mul = rasm_set_current_node(r, pre_mul);
                    i_fmul(r, vtmp[src_j], vsrc, vcoeff);   CMTF("vtmp[%u] = vsrc%c[%u] * coeff[%u][%u];", src_j, cvh, src_j, i, src_j);
                    pre_mul = rasm_set_current_node(r, pre_mul);
                    i_fadd(r, dx[i], dx[i], vtmp[src_j]);   CMTF("v%c[%u] += vtmp[%u];", cvh, i, src_j);
                } else {
                    i_fadd(r, dx[i], dx[i], vsrc);          CMTF("v%c[%u] += vsrc%c[%u];", cvh, i, cvh, src_j);
                }
            }
            first = false;
        }
    }
}

/* Emit term `t` (the t-th non-zero coefficient) of row `i` for one bank. */
static void linear_term(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                        SwsAArch64OpRegs *regs, int bank, int i, int t)
{
    RasmContext *r = s->rctx;
    RasmOp *sx = bank_src(regs, bank);
    RasmOp *dx = bank_dst(regs, bank);
    char cvh = "lh23"[bank];

    int n = 0;
    for (int j = 0; j < 5; j++) {
        bool is_offset = (j == 0);
        int src_j = is_offset ? 4 : (j - 1);
        if (p->par.lin.zero & SWS_MASK(i, src_j))
            continue;
        if (n++ != t)
            continue;
        RasmOp vsrc = sx[src_j];
        RasmOp vcoeff = regs->linear_vcoeff[i][j];
        if (t == 0 && is_offset) {
            linear_load_offset(s, p, regs, bank, i);
        } else if (t == 0) {
            if (p->par.lin.one & SWS_MASK(i, src_j)) {
                i_mov16b(r, dx[i], vsrc);               CMTF("v%c[%u]  = vsrc%c[%u];", cvh, i, cvh, src_j);
            } else if (p->type == SWS_PIXEL_F32) {
                i_fmul  (r, dx[i], vsrc, vcoeff);       CMTF("v%c[%u]  = vsrc%c[%u] * coeff[%u][%u];", cvh, i, cvh, src_j, i, src_j);
            } else {
                i_mul   (r, dx[i], vsrc, vcoeff);       CMTF("v%c[%u]  = vsrc%c[%u] * coeff[%u][%u];", cvh, i, cvh, src_j, i, src_j);
            }
        } else if (p->type != SWS_PIXEL_F32) {
            i_mla (r, dx[i], vsrc, vcoeff);             CMTF("v%c[%u] += vsrc%c[%u] * coeff[%u][%u];", cvh, i, cvh, src_j, i, src_j);
        } else {
            i_fmla(r, dx[i], vsrc, vcoeff);             CMTF("v%c[%u] += vsrc%c[%u] * coeff[%u][%u];", cvh, i, cvh, src_j, i, src_j);
        }
        return;
    }
}

static void asmgen_op_linear(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                             SwsAArch64OpRegs *regs)
{
    if (linear_interleaved(p)) {
        /**
         * Emit the terms round-robin across rows and vector banks, so that
         * consecutive instructions are independent. Each row still
         * accumulates its terms in the same order.
         */
        for (int t = 0; t < 5; t++) {
            LOOP_MASK(p, i) {
                for (int b = 0; b < s->nbanks; b++)
                    linear_term(s, p, regs, b, i, t);
            }
        }
        return;
    }

    /**
     * Load the offsets of all rows whose output register is not read as an
     * input by any row up front (both banks), so that in-order cores don't
     * stall on the load right before the first multiply-accumulate.
     */
    SwsCompMask preloaded = 0;
    LOOP_MASK(p, i) {
        if (p->par.lin.zero & SWS_MASK_OFF(i))
            continue;
        bool read = false;
        LOOP_MASK(p, k) {
            for (int j = 0; j < 4; j++) {
                if (!(p->par.lin.zero & SWS_MASK(k, j)) &&
                    a64op_vec_n(regs->sl[j]) == a64op_vec_n(regs->dl[i]))
                    read = true;
            }
        }
        if (!read)
            preloaded |= SWS_COMP(i);
    }
    LOOP      (preloaded, i) linear_load_offset(s, p, regs, false, i);
    LOOP_VH(s, preloaded, i) linear_load_offset(s, p, regs, true,  i);

    /* Perform linear passes for low and high vector banks. */
    linear_pass(s, p, regs, false, preloaded);
    linear_pass(s, p, regs, true,  preloaded);
}

/*********************************************************************/
/* add dithering noise */
/* SWS_UOP_DITHER */

static void asmgen_op_dither(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                             SwsAArch64OpRegs *regs)
{
    RasmContext *r = s->rctx;
    RasmOp src_ptr = regs->dither_ptr;

    RasmOp ptr = s->tmp0;
    RasmOp tmp1 = s->tmp1;
    RasmOp wtmp1 = a64op_w(tmp1);
    RasmOp bx64 = a64op_x(s->bx);
    RasmOp y64 = a64op_x(s->y);

    /**
     * The dither matrix is (1 << size_log2)² pixels large. It is
     * periodic, so the x and y offsets should be masked to fit inside
     * (1 << size_log2). The matrix buffer is prepared by
     * translate_dither_op() in libswscale/uops.c.
     * The width of the matrix is assumed to be at least 8, which matches
     * the maximum block_size for aarch64 asmgen when f32 operations
     * (i.e., dithering) are used. This guarantees that the x offset is
     * aligned and that reading block_size elements does not extend past
     * the end of the row. The x offset doesn't change between components,
     * so it is only required to be masked once.
     * The y offset, on the other hand, may change per component, and
     * would therefore need to be masked for every y_offset value. To
     * avoid this, the matrix buffer is over-allocated by the largest
     * y_offset value, with the extra rows repeating the first rows of
     * the matrix. This way, we only need to mask the y offset once, and
     * can safely increment the dither matrix pointer by fixed offsets
     * for every y_offset change.
     */

    /**
     * Sort components by y_offset value so that we can start dithering
     * with the smallest value, and increment the pointer upwards for
     * each new offset. The dither matrix is over-allocated and may be
     * over-read at the top, but it cannot be over-read before the start
     * of the buffer. Since we only mask the y offset once, this would
     * be an issue if we tried to subtract a value larger than the
     * initial y_offset.
     */
    int sorted[4];
    int n_comps = 0;
    /* Very cheap bucket sort. */
    int max_offset = 0;
    LOOP_MASK(p, i)
        max_offset = FFMAX(max_offset, p->par.dither.y_offset[i]);
    for (int y_off = 0; y_off <= max_offset; y_off++) {
        LOOP_MASK(p, i) {
            if (p->par.dither.y_offset[i] == y_off)
                sorted[n_comps++] = i;
        }
    }

    /**
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
    const int block_size_log2   = (p->block_size == 16) ? 4 : 3;
    const int dither_size_log2  = p->par.dither.size_log2;
    const int sizeof_float_log2 = 2;
    if (dither_size_log2 != block_size_log2) {
        RasmOp lsb   = IMM(block_size_log2 + sizeof_float_log2);
        RasmOp width = IMM(dither_size_log2 - block_size_log2);
        i_ubfiz(r, tmp1, bx64, lsb, width); CMT("tmp1 = (bx & ((dither_size / block_size) - 1)) * block_size * sizeof(float);");
        i_add  (r, ptr,  src_ptr,  tmp1);   CMT("ptr += tmp1;");
        src_ptr = ptr;
    }

    /**
     * Compute the pointer to the first (smallest) y offset, then load the
     * rows of all components up front into separate registers, using
     * immediate offsets from that pointer, and only then add them. This
     * avoids pointer increments and gives in-order cores time to complete
     * the loads before they are used.
     */
    const int first_y_off = p->par.dither.y_offset[sorted[0]];
    {
        RasmOp lsb   = IMM(dither_size_log2 + sizeof_float_log2);
        RasmOp width = IMM(dither_size_log2);
        /**
         * The ubfiz instruction for the y offset performs masking
         * by the dither matrix size and shifts by the stride.
         */
        if (first_y_off == 0) {
            i_ubfiz(r, tmp1,  y64,  lsb, width);        CMT("tmp1 = (y & (dither_size - 1)) * dither_size * sizeof(float);");
        } else {
            i_add  (r, wtmp1, s->y, IMM(first_y_off));  CMTF("tmp1 = y + y_off[%u];", sorted[0]);
            i_ubfiz(r, tmp1,  tmp1, lsb, width);        CMT("tmp1 = (tmp1 & (dither_size - 1)) * dither_size * sizeof(float);");
        }
        i_add(r, ptr, src_ptr, tmp1);                   CMT("ptr += tmp1;");
    }

    const int row_size = (1 << dither_size_log2) * sizeof(float);
    av_assert0(dither_size_log2 >= block_size_log2);

    /* Distinct y offsets, in increasing order. */
    int n_loads = 0;
    int load_off[4], load_of[4];
    for (int sorted_i = 0; sorted_i < n_comps; sorted_i++) {
        int i = sorted[sorted_i];
        int y_off = p->par.dither.y_offset[i];
        if (!n_loads || load_off[n_loads - 1] != y_off) {
            load_of[n_loads] = i;
            load_off[n_loads++] = y_off;
        }
    }

    /**
     * Load the rows for all components (in up to two phases of vector
     * banks, if there are not enough temp registers), then add them.
     */
    const int nphases = (s->nbanks * n_loads > FF_ARRAY_ELEMS(regs->vt)) ? 2 : 1;
    const int phase_banks = s->nbanks / nphases;
    for (int phase = 0; phase < nphases; phase++) {
        const int b0 = phase * phase_banks;
        RasmOp vd[4][4]; /* [load][bank] */
        for (int k = 0; k < n_loads; k++) {
            int delta = (load_off[k] - first_y_off) * row_size;
            for (int b = 0; b < phase_banks; b++)
                vd[k][b0 + b] = regs->vt[phase_banks * k + b];
            if (phase_banks == 1) {
                RasmOp src = delta ? a64op_off(ptr, delta) : a64op_base(ptr);
                i_ldr(r, v_q(vd[k][b0]), src);
                CMTF("dither%d[%d] = ptr[y_off[%u] - y_off[%u]];", b0, k, load_of[k], sorted[0]);
                continue;
            }
            for (int b = b0; b < b0 + phase_banks; b += 2) {
                int off = delta + 16 * b;
                av_assert0(off <= 1008); /* ldp q immediate range */
                RasmOp src = off ? a64op_off(ptr, off) : a64op_base(ptr);
                i_ldp(r, v_q(vd[k][b]), v_q(vd[k][b + 1]), src);
                CMTF("{ dither%d[%d], dither%d[%d] } = ptr[y_off[%u] - y_off[%u]] + %d;",
                     b, k, b + 1, k, load_of[k], sorted[0], 4 * b);
            }
        }
        int k = -1;
        int last_y_off = -1;
        for (int sorted_i = 0; sorted_i < n_comps; sorted_i++) {
            int i = sorted[sorted_i];
            int y_off = p->par.dither.y_offset[i];
            if (y_off != last_y_off)
                k++;
            last_y_off = y_off;
            for (int b = b0; b < b0 + phase_banks; b++) {
                i_fadd(r, bank_dst(regs, b)[i], bank_src(regs, b)[i], vd[k][b]);
                CMTF("v%d[%u] += vdither%d[%d];", b, i, b, k);
            }
        }
    }
}
