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
typedef struct SwsAArch64Immediate {
    uint32_t     val;
    SwsPixelType movi;
    RasmOp       src_op;
    RasmOp       op;
} SwsAArch64Immediate;

typedef struct SwsAArch64ConstVec {
    uint8_t val[16];
    RasmOp op;
} SwsAArch64ConstVec;

typedef struct SwsAArch64ConstVecElem {
    uint32_t val;
    unsigned elem;
    RasmOp op;
} SwsAArch64ConstVecElem;

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
    RasmOp impl_priv;
    RasmNode *load_cont_node;
    SwsAArch64OpRegs regs;

    /* JIT-related variables. */
    SwsAArch64Immediate imm[16];    // TODO 16
    int                 imm_count;
    SwsAArch64ConstVec  data[16];   // TODO 16
    int                 data_count;

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

    /* TODO */
    int block_size;
    SwsContext *sws;
} SwsAArch64Context;

/* Looping when s->use_vh is set. */
#define LOOP_VH(s, mask, idx) if (s->use_vh) LOOP(mask, idx)
#define LOOP_MASK_VH(s, p, idx) if (s->use_vh) LOOP_MASK(p, idx)
#define LOOP_MASK_BWD_VH(s, p, idx) if (s->use_vh) LOOP_MASK_BWD(p, idx)

#endif /* SWSCALE_AARCH64_OPS_ASMGEN_H */
