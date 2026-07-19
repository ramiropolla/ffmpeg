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

#ifndef SWSCALE_AARCH64_OPS_ASMGEN_H
#define SWSCALE_AARCH64_OPS_ASMGEN_H

#include "rasm.h"
#include "ops_impl.h"

/*********************************************************************/
typedef struct SwsAArch64OpRegs {
    RasmOp sl[ 4]; /* input vector registers (low bank) */
    RasmOp sh[ 4]; /* input vector registers (high bank) */
    RasmOp dl[ 4]; /* output vector registers (low bank) */
    RasmOp dh[ 4]; /* output vector registers (high bank) */
    RasmOp vt[12]; /* temp vector registers */
    RasmOp vk[ 4]; /* constant data (may be gprs) */

    /* Op-specific registers. */
    union {
        RasmOp dither_ptr;
        RasmOp linear_vcoeff[4][5];
    };
} SwsAArch64OpRegs;

/*********************************************************************/
typedef union SwsAArch64Vector {
    uint8_t  u8 [16];
    uint16_t u16[ 8];
    uint32_t u32[ 4];
    float    f32[ 4];
    uint64_t u64[ 2];
} SwsAArch64Vector;

typedef struct SwsAArch64ConstVec {
    SwsAArch64Vector vec;
    RasmOp op;
    int    op_idx;  /* index of last 32-bit element used. */
} SwsAArch64ConstVec;

/*********************************************************************/
typedef struct SwsAArch64Context {
    RasmContext *rctx;

    /* SwsOpFunc arguments. */
    RasmOp exec;
    RasmOp impl;
    RasmOp bx_start;
    RasmOp y_start;
    RasmOp bx_end;
    RasmOp y_end;

    /* Loop iterator variables. */
    RasmOp bx;
    RasmOp y;

    /* Scratch registers. */
    RasmOp tmp0;
    RasmOp tmp1;

    /* CPS-related variables. */
    RasmOp op0_func;
    RasmOp op1_impl;
    RasmOp cont;
    RasmNode *load_cont_node;
    SwsAArch64OpRegs regs;

    /* JIT-related variables. */
    SwsAArch64ConstVec data[16];
    int data_count;

    /* Read/Write data pointers and padding. */
    RasmOp in[4];
    RasmOp out[4];
    RasmOp in_bump[4];
    RasmOp out_bump[4];

    /* Process function. */
    AArch64RegState regstate;
    RasmNode *setup;
    RasmNode *loop;

    /* Vector register dimensions. */
    size_t el_size;
    size_t el_count;
    size_t vec_size;
    bool use_vh;
} SwsAArch64Context;

/* Looping when s->use_vh is set. */
#define LOOP_VH(s, mask, idx) if (s->use_vh) LOOP(mask, idx)
#define LOOP_MASK_VH(s, p, idx) if (s->use_vh) LOOP_MASK(p, idx)
#define LOOP_MASK_BWD_VH(s, p, idx) if (s->use_vh) LOOP_MASK_BWD(p, idx)

/* Emit the main per-block process loop. */
void ff_sws_aarch64_asmgen_process(SwsAArch64Context *s, SwsCompMask imask, SwsCompMask omask);

/* Set up vector register dimensions for p and reshape regs accordingly. */
void ff_sws_aarch64_asmgen_setup_vecs(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                                      SwsAArch64OpRegs *regs);

/* Emit the uop kernel selected by p->uop. */
void ff_sws_aarch64_asmgen_op(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                              SwsAArch64OpRegs *regs);

#endif /* SWSCALE_AARCH64_OPS_ASMGEN_H */
