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
    /**
     * Input/output vector registers of the third and fourth banks, only
     * used by 32-bit kernels with block_size 16.
     */
    RasmOp s2[ 4];
    RasmOp s3[ 4];
    RasmOp d2[ 4];
    RasmOp d3[ 4];
    RasmOp vt[12]; /* temp vector registers */
    RasmOp vk[ 4]; /* constant data (may be gprs) */

    /* Op-specific registers. */
    union {
        RasmOp dither_ptr;
        struct {
            RasmOp linear_vcoeff[4][5];
            RasmOp linear_ptr; /* points to the packed coefficients */
            /* inputs of banks 2-3 to save (non-interleaved, 4 banks) */
            SwsCompMask linear_save_mask;
        };
    };
} SwsAArch64OpRegs;

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
    RasmOp op0_impl;
    RasmOp cont;
    RasmNode *load_cont_node;
    SwsAArch64OpRegs regs;

    /* Read/Write data pointers and padding. */
    RasmOp in[4];
    RasmOp out[4];
    RasmOp in_bump[4];
    RasmOp out_bump[4];

    /* Process function. */
    RasmNode *setup;
    RasmNode *loop;

    /* Vector register dimensions. */
    size_t el_size;
    size_t el_count;
    size_t vec_size;
    bool use_vh;
    int nbanks; /* number of vector banks in use (1, 2 or 4) */
} SwsAArch64Context;

/* Vector register bank `b` of the input or output registers. */
static inline RasmOp *bank_src(SwsAArch64OpRegs *regs, int b)
{
    RasmOp *banks[4] = { regs->sl, regs->sh, regs->s2, regs->s3 };
    return banks[b];
}

static inline RasmOp *bank_dst(SwsAArch64OpRegs *regs, int b)
{
    RasmOp *banks[4] = { regs->dl, regs->dh, regs->d2, regs->d3 };
    return banks[b];
}

#endif /* SWSCALE_AARCH64_OPS_ASMGEN_H */
