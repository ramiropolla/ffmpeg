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

    AArch64Op vl[4];
    AArch64Op vh[4];
    AArch64Op in[4];
    AArch64Op out[4];
    AArch64Op in_padding[4];
    AArch64Op out_padding[4];

    AArch64Op op0_func;
    AArch64Op op1_impl;
    AArch64Op next_func;

    size_t el_size;
    size_t el_count;
    size_t vec_size;
    bool use_vh;

    // HACK
    FILE *fp_cond;
} SwsAArch64Context;

/*********************************************************************/
static const SwsAArch64OpImplParams impl_params[] = {
#include "ops_entries.c"
    { .op = AARCH64_SWS_OP_NONE }
};

/*********************************************************************/
// TODO dump offsets to entries file somehow
static const size_t offsetof_exec_in       = 16;
static const size_t offsetof_exec_out      = 32;
static const size_t offsetof_exec_in_bump  = 48;
static const size_t offsetof_exec_out_bump = 64;
static const size_t offsetof_impl_cont     =  0;
static const size_t sizeof_impl            = 32;

/*********************************************************************/
static void aarch64_asmgen_load_cont(SwsAArch64Context *s)
{
    AArch64Context *a = s->actx;
    a64insn_ldr(a, s->next_func, a64op_off(s->impl, offsetof_impl_cont));
}

static void aarch64_asmgen_continue(SwsAArch64Context *s)
{
    AArch64Context *a = s->actx;
    a64insn_add(a, s->impl, s->impl, a64op_imm(sizeof_impl));
    a64insn_br (a, s->next_func);
}

/*********************************************************************/
#define LOOP_MASK(s, p, idx)                \
    for (int idx = 0; idx < 4; idx++)       \
        if ((p->mask) & (1 << (idx << 2)))

#define LOOP_MASK_VH(s, p, idx)             \
    for (int idx = 0; idx < 4; idx++)       \
        if (s->use_vh && (p->mask) & (1 << (idx << 2)))

static void aarch64_asmgen_process(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;

    char func_name[256];
    sws_aarch64_op_impl_func_name(func_name, sizeof(func_name), p);

    int func_id = aarch64_new_label(a, func_name);
    aarch64_add_func(a, func_id, true);

    // TODO prologue
    aarch64_add_comment(a, "prologue");

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

    a64insn_mov(a, s->impl, s->op1_impl);
    a64insn_br (a, s->op0_func);

    aarch64_add_endfunc(a);
}

static void aarch64_asmgen_process_return(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
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
    AArch64Op sp = a64op_sp();

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

    aarch64_add_comment(a, "reset bx");
    a64insn_mov(a, s->bx, s->bx_start);

    aarch64_add_label(a, loop);
    a64insn_br (a, s->op0_func);

    aarch64_add_label(a, end);
    // TODO epilogue
    aarch64_add_comment(a, "epilogue");
    a64insn_ret(a);
    aarch64_add_endfunc(a);
}

static void aarch64_asmgen_op_read(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;

    switch (p->rw) {
    case AARCH64_SWS_OP_RW_BIT:
        // TODO
        break;
    case AARCH64_SWS_OP_RW_NIBBLE:
        // TODO
        break;
    case AARCH64_SWS_OP_RW_PACKED:
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
            if (!s->use_vh)
                break;
            switch (p->mask) {
            case 0x0011: a64insn_ld2(a, a64op_veclist2(s->vh[0], s->vh[1]),                     a64op_post(s->in[0], s->vec_size * 2)); break;
            case 0x0111: a64insn_ld3(a, a64op_veclist3(s->vh[0], s->vh[1], s->vh[2]),           a64op_post(s->in[0], s->vec_size * 3)); break;
            case 0x1111: a64insn_ld4(a, a64op_veclist4(s->vh[0], s->vh[1], s->vh[2], s->vh[3]), a64op_post(s->in[0], s->vec_size * 4)); break;
            }
        }
        break;
    case AARCH64_SWS_OP_RW_PLANAR:
        LOOP_MASK(s, p, i) {
            switch ((s->use_vh ? 0x100 : 0) | s->vec_size) {
            case 0x008: a64insn_ldr(a, a64op_d(s->vl[i]),                    a64op_post(s->in[i], s->vec_size * 1)); break;
            case 0x010: a64insn_ldr(a, a64op_q(s->vl[i]),                    a64op_post(s->in[i], s->vec_size * 1)); break;
            case 0x108: a64insn_ldp(a, a64op_d(s->vl[i]), a64op_d(s->vh[i]), a64op_post(s->in[i], s->vec_size * 2)); break;
            case 0x110: a64insn_ldp(a, a64op_q(s->vl[i]), a64op_q(s->vh[i]), a64op_post(s->in[i], s->vec_size * 2)); break;
            }
        }
        break;
    }
}

static void aarch64_asmgen_op_write(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
}

static void aarch64_asmgen_op_swap_bytes(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
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

static void aarch64_asmgen_op_swizzle(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
}

static void aarch64_asmgen_op_pack(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
}

static void aarch64_asmgen_op_lshift(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    LOOP_MASK   (s, p, i) a64insn_shl(a, s->vl[i], s->vl[i], a64op_imm(p->shift));
    LOOP_MASK_VH(s, p, i) a64insn_shl(a, s->vh[i], s->vh[i], a64op_imm(p->shift));
}

static void aarch64_asmgen_op_rshift(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
    LOOP_MASK   (s, p, i) a64insn_ushr(a, s->vl[i], s->vl[i], a64op_imm(p->shift));
    LOOP_MASK_VH(s, p, i) a64insn_ushr(a, s->vh[i], s->vh[i], a64op_imm(p->shift));
}

static void aarch64_asmgen_op_clear(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
}

static void aarch64_asmgen_op_convert(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
}

static void aarch64_asmgen_op_minmax(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
}

static void aarch64_asmgen_op_scale(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
}

static void aarch64_asmgen_op_linear(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
}

static void aarch64_asmgen_op_dither(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;
}

static void aarch64_asmgen(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    AArch64Context *a = s->actx;

    switch (p->op) {
    case AARCH64_SWS_OP_PROCESS:
        aarch64_asmgen_process(s, p);
        return;
    case AARCH64_SWS_OP_PROCESS_RETURN:
        aarch64_asmgen_process_return(s, p);
        return;
    }

    char func_name[256];
    sws_aarch64_op_impl_func_name(func_name, sizeof(func_name), p);

    char cond_str[256];
    sws_aarch64_op_impl_cond_str(cond_str, sizeof(cond_str), p, "p->");
    fprintf(s->fp_cond, "    if (%s) return %s;\n", cond_str, func_name);

    int func_id = aarch64_new_label(a, func_name);
    aarch64_add_func(a, func_id, true);
    aarch64_asmgen_load_cont(s);

    size_t el_size = sws_aarch64_pixel_size(p->type);
    size_t total_size = p->block_size * el_size;
    s->vec_size = FFMIN(total_size, 16);

    s->use_vh = (s->vec_size != total_size);

    s->el_size = el_size;
    s->el_count = s->vec_size / el_size;

    s->vl[0] = a64op_make_vec(0, s->el_count, el_size);
    s->vl[1] = a64op_make_vec(1, s->el_count, el_size);
    s->vl[2] = a64op_make_vec(2, s->el_count, el_size);
    s->vl[3] = a64op_make_vec(3, s->el_count, el_size);
    s->vh[0] = a64op_make_vec(4, s->el_count, el_size);
    s->vh[1] = a64op_make_vec(5, s->el_count, el_size);
    s->vh[2] = a64op_make_vec(6, s->el_count, el_size);
    s->vh[3] = a64op_make_vec(7, s->el_count, el_size);

    switch (p->op) {
    case AARCH64_SWS_OP_READ:
        aarch64_asmgen_op_read(s, p);
        break;
    case AARCH64_SWS_OP_WRITE:
        aarch64_asmgen_op_write(s, p);
        break;
    case AARCH64_SWS_OP_SWAP_BYTES:
        aarch64_asmgen_op_swap_bytes(s, p);
        break;
    case AARCH64_SWS_OP_SWIZZLE:
        aarch64_asmgen_op_swizzle(s, p);
        break;
    case AARCH64_SWS_OP_UNPACK:
    case AARCH64_SWS_OP_PACK:
        aarch64_asmgen_op_pack(s, p);
        break;
    case AARCH64_SWS_OP_LSHIFT:
        aarch64_asmgen_op_lshift(s, p);
        break;
    case AARCH64_SWS_OP_RSHIFT:
        aarch64_asmgen_op_rshift(s, p);
        break;
    case AARCH64_SWS_OP_CLEAR:
        aarch64_asmgen_op_clear(s, p);
        break;
    case AARCH64_SWS_OP_CONVERT:
    case AARCH64_SWS_OP_EXPAND:
        aarch64_asmgen_op_convert(s, p);
        break;
    case AARCH64_SWS_OP_MIN:
    case AARCH64_SWS_OP_MAX:
        aarch64_asmgen_op_minmax(s, p);
        break;
    case AARCH64_SWS_OP_SCALE:
        aarch64_asmgen_op_scale(s, p);
        break;
    case AARCH64_SWS_OP_LINEAR:
        aarch64_asmgen_op_linear(s, p);
        break;
    case AARCH64_SWS_OP_DITHER:
        aarch64_asmgen_op_dither(s, p);
        break;
    }

    aarch64_asmgen_continue(s);
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
int main(int argc, char *argv[])
{
    int ret;

#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    AArch64Context *actx = aarch64_alloc();
    if (!actx)
        return AVERROR(ENOMEM);

    SwsAArch64Context s = { 0 };
    s.actx = actx;

    FILE *fp_cond = fopen("ops_cond.c", "w");
    s.fp_cond = fp_cond;

    /* Process function arguments */
    s.exec     = a64op_gpx(0); // const SwsOpExec *exec
    s.impl     = a64op_gpx(1); // const void *priv
    s.bx_start = a64op_gpw(2); // int bx_start
    s.y_start  = a64op_gpw(3); // int y_start
    s.bx_end   = a64op_gpw(4); // int bx_end
    s.y_end    = a64op_gpw(5); // int y_end

    s.bx       = a64op_gpw(6);
    s.y        = s.y_start;

    s.tmp0     = a64op_gpx(7);
    s.tmp1     = a64op_gpx(8);

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

    s.op0_func  = a64op_gpx(28);
    s.op1_impl  = a64op_gpx(29);
    s.next_func = a64op_gpx(30);

    const SwsAArch64OpImplParams *params = impl_params;
    while (params->op)
        aarch64_asmgen(&s, params++);

    printf("#include \"libavutil/aarch64/asm.S\"\n");
    printf("\n");

    aarch64_print(s.actx, stdout);

    ret = 0;

error:
    aarch64_free(&s.actx);
    fclose(fp_cond);

    return ret;
}
