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

/*********************************************************************/
typedef struct SwsAArch64OpRegs {
    RasmOp sl[4]; /* input vector registers (low bank) */
    RasmOp sh[4]; /* input vector registers (high bank) */
    RasmOp dl[4]; /* output vector registers (low bank) */
    RasmOp dh[4]; /* output vector registers (high bank) */
    RasmOp vt[8]; /* temp vector registers */
    RasmOp vk[4]; /* constant data (may be gprs) */
} SwsAArch64OpRegs;

/*********************************************************************/
/* Immediate values: scalar constants broadcast to all vector lanes,  *
 * pre-loaded into v27..v31 before the inner loop.                    */

#define SWS_AARCH64_MAX_IMM    5
#define SWS_AARCH64_REGID_VIMM 27

/* Data pool: 128-bit constant vectors pre-loaded into v8..v15 before
 * the inner loop, loaded via adr + ldr from a pool after the function. */

#define SWS_AARCH64_MAX_DATA_VECS 8
#define SWS_AARCH64_REGID_VDATA   8

typedef struct SwsAArch64Immediate {
    uint32_t val;
    SwsPixelType type;
    // uint8_t repeat_len;
    // uint8_t small_value;
    RasmOp op;
} SwsAArch64Immediate;

typedef struct SwsAArch64RegState {
    uint32_t used;
    uint32_t clobbered;
} SwsAArch64RegState;

typedef struct SwsAArch64ConstVec {
    uint8_t val[16];
    RasmOp op;
    // TODO .8b
} SwsAArch64ConstVec;

/*********************************************************************/
typedef struct SwsAArch64Context {
    RasmContext *rctx;

    /* Process function. */
    AArch64Frame frame;
    AArch64Frame vframe;
    RasmNode *setup;
    RasmNode *loop;

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
    RasmOp impl_priv;
    SwsAArch64OpRegs regs;

    /* JIT-related variables. */
    /* Immediates. */
    SwsAArch64Immediate imm[SWS_AARCH64_MAX_IMM];
    int                 imm_count;

    SwsAArch64ConstVec  data[SWS_AARCH64_MAX_DATA_VECS];
    int                 data_count;

    /* Read/Write data pointers and padding. */
    RasmOp in[4];
    RasmOp out[4];
    RasmOp in_bump[4];
    RasmOp out_bump[4];

    /* Vector register dimensions. */
    size_t el_size;
    size_t el_count;
    size_t vec_size;
    bool use_vh;

    /* TOOD */
    int block_size;
    SwsContext *sws;
} SwsAArch64Context;

/* Looping when s->use_vh is set. */
#define LOOP_VH(s, mask, idx) if (s->use_vh) LOOP(mask, idx)
#define LOOP_MASK_VH(s, p, idx) if (s->use_vh) LOOP_MASK(p, idx)
#define LOOP_MASK_BWD_VH(s, p, idx) if (s->use_vh) LOOP_MASK_BWD(p, idx)

#endif /* SWSCALE_AARCH64_OPS_ASMGEN_H */
