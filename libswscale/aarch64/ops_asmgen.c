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
#define LOOP_MASK(s, p, idx)                \
    for (int idx = 0; idx < 4; idx++)       \
        if ((p->mask) & (1 << (idx << 2)))

#define LOOP_MASK_VH(s, p, idx)             \
    for (int idx = 0; idx < 4; idx++)       \
        if (s->use_vh && (p->mask) & (1 << (idx << 2)))

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
    AArch64Op next_func;

    AArch64Op vl[4];
    AArch64Op vh[4];
    AArch64Op vt[4];

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
static const SwsAArch64OpImplParams impl_params[] = {
#include "ops_entries.c"
    { .op = AARCH64_SWS_OP_NONE }
};

/*********************************************************************/
/* Save registers x19-x28, along with x29 (fp) and x30 (lr). */
#define MAX_SAVED_REGS 12

static unsigned clobbered_frame_size(unsigned n)
{
    return ((n + 1) >> 1) << 4;
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
        a64insn_str(a, regs[0], sp_pre);
    } else {
        a64insn_stp(a, regs[0], regs[1], sp_pre);
        for (unsigned i = 2; i + 1 < n; i += 2)
            a64insn_stp(a, regs[i], regs[i + 1], a64op_off(sp, i * sizeof(uint64_t)));
        if (n & 1)
            a64insn_str(a, regs[n - 1], a64op_off(sp, (n - 1) * sizeof(uint64_t)));
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
        a64insn_ldr(a, regs[0], sp_post);
    } else {
        if (n & 1)
            a64insn_ldr(a, regs[n - 1], a64op_off(sp, (n - 1) * sizeof(uint64_t)));
        for (unsigned i = (n & ~1u) - 2; i >= 2; i -= 2)
            a64insn_ldp(a, regs[i], regs[i + 1], a64op_off(sp, i * sizeof(uint64_t)));
        a64insn_ldp(a, regs[0], regs[1], sp_post);
    }
}

/*********************************************************************/
static unsigned clobbered_saved_gprs(const SwsAArch64Context *s,
                                     const SwsAArch64OpImplParams *p,
                                     AArch64Op regs[MAX_SAVED_REGS])
{
    unsigned n = 0;
    regs[n++] = s->op1_impl;
    LOOP_MASK(s, p, i) {
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

    int func_id = aarch64_new_label(a, func_name);
    aarch64_add_func(a, func_id, true);

    /* Function prologue */
    AArch64Op saved_regs[MAX_SAVED_REGS];
    unsigned nsaved = clobbered_saved_gprs(s, p, saved_regs);
    if (nsaved) {
        aarch64_add_comment(a, "prologue");
        asmgen_prologue(s, saved_regs, nsaved);
    }

    a64insn_ldr(a, s->op0_func, a64op_off(s->impl, offsetof_impl_cont));
    a64insn_add(a, s->op1_impl, s->impl, a64op_imm(sizeof_impl));

    aarch64_add_comment(a, "exec->in");
    LOOP_MASK(s, p, i) a64insn_ldr(a, s->in[i],          a64op_off(s->exec, offsetof_exec_in       + (i * sizeof(uint8_t *))));
    aarch64_add_comment(a, "exec->out");
    LOOP_MASK(s, p, i) a64insn_ldr(a, s->out[i],         a64op_off(s->exec, offsetof_exec_out      + (i * sizeof(uint8_t *))));
    aarch64_add_comment(a, "exec->in_bump");
    LOOP_MASK(s, p, i) a64insn_ldr(a, s->in_padding[i],  a64op_off(s->exec, offsetof_exec_in_bump  + (i * sizeof(ptrdiff_t))));
    aarch64_add_comment(a, "exec->out_bump");
    LOOP_MASK(s, p, i) a64insn_ldr(a, s->out_padding[i], a64op_off(s->exec, offsetof_exec_out_bump + (i * sizeof(ptrdiff_t))));

    aarch64_add_comment(a, "bx = bx_start;");
    a64insn_mov(a, s->bx, s->bx_start);

    a64insn_mov(a, s->impl, s->op1_impl);
    a64insn_br (a, s->op0_func);

    aarch64_add_endfunc(a);
}

static void asmgen_process_return(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;

    char func_name[256];
    sws_aarch64_op_impl_func_name(func_name, sizeof(func_name), p);

    int func_id = aarch64_new_label(a, func_name);
    aarch64_add_func(a, func_id, true);

    char buf[256];
    // TODO aarch64_new_local_label, aarch64_new_local_labelf
    int loop = aarch64_new_labelf(a, buf, sizeof(buf), ".Lprocess_return_%04X_loop", nswap16(p->mask));
    int end  = aarch64_new_labelf(a, buf, sizeof(buf), ".Lprocess_return_%04X_end",  nswap16(p->mask));

    aarch64_add_comment(a, "reset impl");
    a64insn_mov(a, s->impl, s->op1_impl);

    aarch64_add_comment(a, "horizontal loop");
    a64insn_add(a, s->bx, s->bx, a64op_imm(1));
    a64insn_cmp(a, s->bx, s->bx_end);
    a64insn_bne(a, loop);

    aarch64_add_comment(a, "vertical loop");
    a64insn_add(a, s->y, s->y, a64op_imm(1));
    a64insn_cmp(a, s->y, s->y_end);
    a64insn_beq(a, end);

    aarch64_add_comment(a, "padding");
    LOOP_MASK(s, p, i) a64insn_add(a, s->in[i],  s->in[i],  s->in_padding[i]);
    LOOP_MASK(s, p, i) a64insn_add(a, s->out[i], s->out[i], s->out_padding[i]);

    aarch64_add_comment(a, "bx = bx_start;");
    a64insn_mov(a, s->bx, s->bx_start);

    aarch64_add_label(a, loop);
    a64insn_br (a, s->op0_func);

    aarch64_add_label(a, end);

    /* Function epilogue */
    AArch64Op saved_regs[MAX_SAVED_REGS];
    unsigned nsaved = clobbered_saved_gprs(s, p, saved_regs);
    if (nsaved) {
        aarch64_add_comment(a, "epilogue");
        asmgen_epilogue(s, saved_regs, nsaved);
    }

    a64insn_ret(a);
    aarch64_add_endfunc(a);
}

/*********************************************************************/
static void asmgen_op_read_bit(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    // TODO
}

static void asmgen_op_read_nibble(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    // TODO
}

static void asmgen_op_read_packed(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    if (p->mask == 0x0001) {
        switch ((s->use_vh ? 0x100 : 0) | s->vec_size) {
        case 0x008: a64insn_ldr(a, a64op_d(s->vl[0]),                    a64op_post(s->in[0], s->vec_size * 1)); break;
        case 0x010: a64insn_ldr(a, a64op_q(s->vl[0]),                    a64op_post(s->in[0], s->vec_size * 1)); break;
        case 0x108: a64insn_ldp(a, a64op_d(s->vl[0]), a64op_d(s->vh[0]), a64op_post(s->in[0], s->vec_size * 2)); break;
        case 0x110: a64insn_ldp(a, a64op_q(s->vl[0]), a64op_q(s->vh[0]), a64op_post(s->in[0], s->vec_size * 2)); break;
        }
    } else {
        switch (p->mask) {
        case 0x0011: a64insn_ld2(a, a64op_veclist2(s->vl[0], s->vl[1]),                     a64op_post(s->in[0], s->vec_size * 2)); break;
        case 0x0111: a64insn_ld3(a, a64op_veclist3(s->vl[0], s->vl[1], s->vl[2]),           a64op_post(s->in[0], s->vec_size * 3)); break;
        case 0x1111: a64insn_ld4(a, a64op_veclist4(s->vl[0], s->vl[1], s->vl[2], s->vl[3]), a64op_post(s->in[0], s->vec_size * 4)); break;
        }
        if (s->use_vh) {
            switch (p->mask) {
            case 0x0011: a64insn_ld2(a, a64op_veclist2(s->vh[0], s->vh[1]),                     a64op_post(s->in[0], s->vec_size * 2)); break;
            case 0x0111: a64insn_ld3(a, a64op_veclist3(s->vh[0], s->vh[1], s->vh[2]),           a64op_post(s->in[0], s->vec_size * 3)); break;
            case 0x1111: a64insn_ld4(a, a64op_veclist4(s->vh[0], s->vh[1], s->vh[2], s->vh[3]), a64op_post(s->in[0], s->vec_size * 4)); break;
            }
        }
    }
}

static void asmgen_op_read_planar(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    LOOP_MASK(s, p, i) {
        switch ((s->use_vh ? 0x100 : 0) | s->vec_size) {
        case 0x008: a64insn_ldr(a, a64op_d(s->vl[i]),                    a64op_post(s->in[i], s->vec_size * 1)); break;
        case 0x010: a64insn_ldr(a, a64op_q(s->vl[i]),                    a64op_post(s->in[i], s->vec_size * 1)); break;
        case 0x108: a64insn_ldp(a, a64op_d(s->vl[i]), a64op_d(s->vh[i]), a64op_post(s->in[i], s->vec_size * 2)); break;
        case 0x110: a64insn_ldp(a, a64op_q(s->vl[i]), a64op_q(s->vh[i]), a64op_post(s->in[i], s->vec_size * 2)); break;
        }
    }
}

static void asmgen_op_write_bit(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    // TODO
}

static void asmgen_op_write_nibble(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    // TODO
}

static void asmgen_op_write_packed(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    if (p->mask == 0x0001) {
        switch ((s->use_vh ? 0x100 : 0) | s->vec_size) {
        case 0x008: a64insn_str(a, a64op_d(s->vl[0]),                    a64op_post(s->out[0], s->vec_size * 1)); break;
        case 0x010: a64insn_str(a, a64op_q(s->vl[0]),                    a64op_post(s->out[0], s->vec_size * 1)); break;
        case 0x108: a64insn_stp(a, a64op_d(s->vl[0]), a64op_d(s->vh[0]), a64op_post(s->out[0], s->vec_size * 2)); break;
        case 0x110: a64insn_stp(a, a64op_q(s->vl[0]), a64op_q(s->vh[0]), a64op_post(s->out[0], s->vec_size * 2)); break;
        }
    } else {
        switch (p->mask) {
        case 0x0011: a64insn_st2(a, a64op_veclist2(s->vl[0], s->vl[1]),                     a64op_post(s->out[0], s->vec_size * 2)); break;
        case 0x0111: a64insn_st3(a, a64op_veclist3(s->vl[0], s->vl[1], s->vl[2]),           a64op_post(s->out[0], s->vec_size * 3)); break;
        case 0x1111: a64insn_st4(a, a64op_veclist4(s->vl[0], s->vl[1], s->vl[2], s->vl[3]), a64op_post(s->out[0], s->vec_size * 4)); break;
        }
        if (s->use_vh) {
            switch (p->mask) {
            case 0x0011: a64insn_st2(a, a64op_veclist2(s->vh[0], s->vh[1]),                     a64op_post(s->out[0], s->vec_size * 2)); break;
            case 0x0111: a64insn_st3(a, a64op_veclist3(s->vh[0], s->vh[1], s->vh[2]),           a64op_post(s->out[0], s->vec_size * 3)); break;
            case 0x1111: a64insn_st4(a, a64op_veclist4(s->vh[0], s->vh[1], s->vh[2], s->vh[3]), a64op_post(s->out[0], s->vec_size * 4)); break;
            }
        }
    }
}

static void asmgen_op_write_planar(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    LOOP_MASK(s, p, i) {
        switch ((s->use_vh ? 0x100 : 0) | s->vec_size) {
        case 0x008: a64insn_str(a, a64op_d(s->vl[i]),                    a64op_post(s->out[i], s->vec_size * 1)); break;
        case 0x010: a64insn_str(a, a64op_q(s->vl[i]),                    a64op_post(s->out[i], s->vec_size * 1)); break;
        case 0x108: a64insn_stp(a, a64op_d(s->vl[i]), a64op_d(s->vh[i]), a64op_post(s->out[i], s->vec_size * 2)); break;
        case 0x110: a64insn_stp(a, a64op_q(s->vl[i]), a64op_q(s->vh[i]), a64op_post(s->out[i], s->vec_size * 2)); break;
        }
    }
}

static void asmgen_op_swap_bytes(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    switch (sws_aarch64_pixel_size(p->type)) {
    case sizeof(uint16_t):
        LOOP_MASK   (s, p, i) a64insn_rev16(a, a64op_16b(s->vl[i]), a64op_16b(s->vl[i]));
        LOOP_MASK_VH(s, p, i) a64insn_rev16(a, a64op_16b(s->vh[i]), a64op_16b(s->vh[i]));
        break;
    case sizeof(uint32_t):
        LOOP_MASK   (s, p, i) a64insn_rev32(a, a64op_16b(s->vl[i]), a64op_16b(s->vl[i]));
        LOOP_MASK_VH(s, p, i) a64insn_rev32(a, a64op_16b(s->vh[i]), a64op_16b(s->vh[i]));
        break;
    }
}

static void asmgen_op_swizzle(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;

    /* Compute used vectors (src and dst) */
    unsigned src_used[4] = { 0 };
    bool done[4] = { true, true, true, true };
    LOOP_MASK(s, p, dst) {
        unsigned src = (p->swizzle >> (dst << 2)) & 0xf;
        src_used[src]++;
        done[dst] = false;
    }

    /* Max ops count is 2 swaps of 3 ops each */
    const int max_op_count = 6;
    uint8_t ops[max_op_count];
    int prev_ops_count = 0;
    int ops_count = 0;

    int unobstructed_copies = -1;
    int swap_and_rotate = -1;

    /* Unobstructed copies */
    do {
        prev_ops_count = ops_count;
        for (int dst = 0; dst < 4; dst++) {
            if (!done[dst] && !src_used[dst]) {
                unsigned src = (p->swizzle >> (dst << 2)) & 0xf;
                if (unobstructed_copies < 0)
                    unobstructed_copies = ops_count;
                ops[ops_count++] = ((src) << 4) | (dst);
                src_used[src]--;
                done[dst] = true;
            }
        }
    } while (ops_count > prev_ops_count);

    /* Swap and rotate */
    const unsigned tmpv = 0xf;
    for (int orig_dst = 0; orig_dst < 4; orig_dst++) {
        if (done[orig_dst])
            continue;

        if (swap_and_rotate < 0)
            swap_and_rotate = ops_count;

        ops[ops_count++] = ((orig_dst) << 4) | tmpv;

        unsigned dst = orig_dst;
        unsigned src = (p->swizzle >> (dst << 2)) & 0xf;
        while (src != orig_dst) {
            ops[ops_count++] = ((src) << 4) | (dst);
            done[dst] = true;
            dst = src;
            src = (p->swizzle >> (dst << 2)) & 0xf;
        }

        ops[ops_count++] = (tmpv << 4) | (dst);
        done[dst] = true;
    }

    /* Emit operations */
    for (int i = 0; i < ops_count; i++) {
        uint8_t op = ops[i];
        uint8_t src = op >> 4;
        uint8_t dst = op & 0xf;
        AArch64Op src_op[2] = {
            (src == tmpv) ? s->vt[0] : s->vl[src],
            (src == tmpv) ? s->vt[1] : s->vh[src],
        };
        AArch64Op dst_op[2] = {
            (dst == tmpv) ? s->vt[0] : s->vl[dst],
            (dst == tmpv) ? s->vt[1] : s->vh[dst],
        };
        if (i == unobstructed_copies)
            aarch64_add_comment(a, "Unobstructed copies");
        else if (i == swap_and_rotate)
            aarch64_add_comment(a, "Swap and rotate");
        a64insn_mov(a, dst_op[0], src_op[0]);
        if (s->use_vh)
            a64insn_mov(a, dst_op[1], src_op[1]);
    }
}

static void asmgen_op_unpack(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    // TODO
}

static void asmgen_op_pack(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    // TODO
}

static void asmgen_op_lshift(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    LOOP_MASK   (s, p, i) a64insn_shl(a, s->vl[i], s->vl[i], a64op_imm(p->shift));
    LOOP_MASK_VH(s, p, i) a64insn_shl(a, s->vh[i], s->vh[i], a64op_imm(p->shift));
}

static void asmgen_op_rshift(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    LOOP_MASK   (s, p, i) a64insn_ushr(a, s->vl[i], s->vl[i], a64op_imm(p->shift));
    LOOP_MASK_VH(s, p, i) a64insn_ushr(a, s->vh[i], s->vh[i], a64op_imm(p->shift));
}

static void asmgen_op_clear(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    // TODO
}

static void asmgen_op_convert(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    size_t src_el_size = s->el_size;
    size_t dst_el_size = sws_aarch64_pixel_size(p->to_type);

#if 0
    // TODO this function assumes block_size is either 8 or 16 and that we're
    // always using at most two full vregs:
    /* Use at most two full vregs during the widest precision section */
    int block_size = (ff_sws_op_list_max_size(ops) == 4) ? 8 : 16;
    // There will be no block_size 8 that does not use 2 full vregs.
#endif

    if (p->type == AARCH64_PIXEL_F32) {
        // TODO types are already 4s
        LOOP_MASK(s, p, i) a64insn_fcvtzu(a, s->vl[i], s->vl[i]);
        LOOP_MASK(s, p, i) a64insn_fcvtzu(a, s->vh[i], s->vh[i]);
        // TODO u32 is still 4s
    }
    if (s->block_size == 8) {
        if (src_el_size == 1 && dst_el_size > src_el_size) {
            LOOP_MASK(s, p, i) a64insn_uxtl (a, s->vl[i].h8(), src_vl[i].b8());
            src_el_size = 2;
        } else if (src_el_size == 4 && dst_el_size < src_el_size) {
            LOOP_MASK(s, p, i) a64insn_xtn  (a, s->vl[i].h4(), src_vl[i].s4());
            LOOP_MASK(s, p, i) a64insn_xtn  (a, s->vh[i].h4(), src_vh[i].s4());
            LOOP_MASK(s, p, i) a64insn_ins  (a, s->vl[i].d(1), s->vh[i].d(0));
            src_el_size = 2;
        }
        if (src_el_size == 2 && dst_el_size == 4) {
            LOOP_MASK(s, p, i) a64insn_uxtl2(a, s->vh[i].s4(), src_vl[i].h8());
            LOOP_MASK(s, p, i) a64insn_uxtl (a, s->vl[i].s4(), src_vl[i].h4());
            src_el_size = 4;
        } else if (src_el_size == 2 && dst_el_size == 1) {
            LOOP_MASK(s, p, i) a64insn_xtn  (a, s->vl[i].b8(), src_vl[i].h8());
            src_el_size = 1;
        }
    } else /* if (s->block_size == 16) */ {
        if (src_el_size == 1 && dst_el_size == 2) {
            LOOP_MASK(s, p, i) a64insn_uxtl2(a, s->vh[i].h8(), src_vl[i].b16());
            LOOP_MASK(s, p, i) a64insn_uxtl (a, s->vl[i].h8(), src_vl[i].b8());
        } else if (src_el_size == 2 && dst_el_size == 1) {
            LOOP_MASK(s, p, i) a64insn_xtn  (a, s->vl[i].b8(), src_vl[i].h8());
            LOOP_MASK(s, p, i) a64insn_xtn  (a, s->vh[i].b8(), src_vh[i].h8());
            LOOP_MASK(s, p, i) a64insn_ins  (a, s->vl[i].d(1), s->vh[i].d(0));
        }
    }
    // [...]
        LOOP_MASK_VH(s, p, i) s->vh[i] = a64op_4s(s->vh[i]);
        LOOP_MASK   (s, p, i) s->vl[i] = a64op_4s(s->vl[i]);
    if (p->type_to == AARCH64_PIXEL_F32) {
        // TODO types are already 4s because of the previous conversion to u32
        LOOP_MASK(s, p, i) a64insn_ucvtf(a, s->vl[i], s->vl[i]);
        LOOP_MASK(s, p, i) a64insn_ucvtf(a, s->vh[i], s->vh[i]);
    }
}

static void asmgen_op_expand(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;

    size_t src_el_size = s->el_size;
    size_t dst_el_size = sws_aarch64_pixel_size(p->to_type);
    size_t dst_total_size = p->block_size * dst_el_size;
    size_t dst_vec_size = FFMIN(dst_total_size, 16);

    if (!s->use_vh)
        s->use_vh = (dst_vec_size != dst_total_size);

    if (src_el_size == 1) {
        // TODO add comment for 16b for zip1/zip2 from u8
        LOOP_MASK_VH(s, p, i) s->vh[i] = a64op_16b(s->vh[i]);
        LOOP_MASK   (s, p, i) s->vl[i] = a64op_16b(s->vl[i]);
        LOOP_MASK_VH(s, p, i) a64insn_zip2(a, s->vh[i], s->vl[i], s->vl[i]);
        LOOP_MASK   (s, p, i) a64insn_zip1(a, s->vl[i], s->vl[i], s->vl[i]);
    }
    if (dst_el_size == 4) {
        // TODO add comment for 8h for zip1/zip2 from u16 (is this even correct? we don't test this)
        LOOP_MASK_VH(s, p, i) s->vh[i] = a64op_8h(s->vh[i]);
        LOOP_MASK   (s, p, i) s->vl[i] = a64op_8h(s->vl[i]);
        LOOP_MASK_VH(s, p, i) a64insn_zip2(a, s->vh[i], s->vl[i], s->vl[i]);
        LOOP_MASK   (s, p, i) a64insn_zip1(a, s->vl[i], s->vl[i], s->vl[i]);
    }
}

static void asmgen_op_min(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    // TODO
}

static void asmgen_op_max(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    // TODO
}

static void asmgen_op_scale(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    // TODO
}

static void asmgen_op_linear(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    // TODO
}

static void asmgen_op_dither(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    // TODO
}

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
    }

    char func_name[256];
    sws_aarch64_op_impl_func_name(func_name, sizeof(func_name), p);

    int func_id = aarch64_new_label(a, func_name);
    aarch64_add_func(a, func_id, true);

    a64insn_ldr(a, s->next_func, a64op_off(s->impl, offsetof_impl_cont));
    aarch64_annotate(a, "next_func = impl->cont;");

    size_t el_size = sws_aarch64_pixel_size(p->type);
    size_t total_size = p->block_size * el_size;
    s->vec_size = FFMIN(total_size, 16);

    s->use_vh = (s->vec_size != total_size);

    s->el_size = el_size;
    s->el_count = s->vec_size / el_size;

    s->vl[0] = a64op_make_vec( 0, s->el_count, el_size);
    s->vl[1] = a64op_make_vec( 1, s->el_count, el_size);
    s->vl[2] = a64op_make_vec( 2, s->el_count, el_size);
    s->vl[3] = a64op_make_vec( 3, s->el_count, el_size);
    s->vh[0] = a64op_make_vec( 4, s->el_count, el_size);
    s->vh[1] = a64op_make_vec( 5, s->el_count, el_size);
    s->vh[2] = a64op_make_vec( 6, s->el_count, el_size);
    s->vh[3] = a64op_make_vec( 7, s->el_count, el_size);
    s->vt[0] = a64op_make_vec(16, s->el_count, el_size);
    s->vt[1] = a64op_make_vec(17, s->el_count, el_size);
    s->vt[2] = a64op_make_vec(18, s->el_count, el_size);
    s->vt[3] = a64op_make_vec(19, s->el_count, el_size);

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
    }

    a64insn_add(a, s->impl, s->impl, a64op_imm(sizeof_impl));
    aarch64_annotate(a, "impl++;");
    a64insn_br (a, s->next_func);
    aarch64_annotate(a, "goto next_func;");

    aarch64_add_endfunc(a);
}

#if 0
AArch64Context *aarch64_alloc(void);

void aarch64_free(AArch64Context **p_actx);

int aarch64_add_insn(AArch64Context *actx, AArch64InsnId id,
                     AArch64Op op0, AArch64Op op1, AArch64Op op2, AArch64Op op3);

int aarch64_add_comment(AArch64Context *actx, const char *comment);

int aarch64_new_label(AArch64Context *actx, const char *name);
#if 0
int aarch64_new_labelf(AArch64Context *actx, char *s, size_t n, const char *fmt, ...) av_printf_format(4, 5);
#endif
int aarch64_add_label(AArch64Context *actx, int id);
int aarch64_add_func(AArch64Context *actx, int id, bool export);

int aarch64_add_endfunc(AArch64Context *actx);

void aarch64_annotate(AArch64Context *actx, const char *comment);

int aarch64_print(AArch64Context *actx, FILE *fp);
#endif

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
    s.next_func = s.exec;

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
    int lookup = 0;

#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-lookup"))
            lookup = 1;
    }

    return lookup ? lookup_gen() : asmgen();
}
