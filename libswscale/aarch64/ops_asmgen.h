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

/* Opaque here: ops_static.c is a standalone host tool that must not
 * depend on internal FFmpeg headers, so it never includes
 * ../ops_chain.h and only ever sees this forward declaration (fine --
 * it only stores/passes a pointer, JIT-only code dereferences it).
 * Wherever ../ops_chain.h *is* included first (ops_jit.c), this
 * redeclares the same typedef name against the by-then-complete type,
 * which C allows. */
typedef struct SwsOpChain SwsOpChain;

/*********************************************************************/
typedef struct SwsAArch64OpRegs {
    RasmOp sl[ 4]; /* input vector registers (low bank) */
    RasmOp sh[ 4]; /* input vector registers (high bank) */
    RasmOp dl[ 4]; /* output vector registers (low bank) */
    RasmOp dh[ 4]; /* output vector registers (high bank) */
    RasmOp vt[12]; /* temp vector registers */
    RasmOp vk[ 4]; /* constant data (may be gprs) */

    /* SWS_UOP_LINEAR/SWS_UOP_LINEAR_FMA: one resolved source operand
     * per non-zero entry of the op's 4x5 coefficient matrix (column 4
     * is the offset term, columns 0-3 are the input terms -- matches
     * SwsOp.lin.m[4][5] and par.lin.zero/one's SWS_MASK(i, j)
     * indexing). CPS (asmgen_setup_linear(), ops_static.c) points each
     * entry at a by-element lane of vk[]; JIT
     * (aarch64_jit_setup_constants(), ops_jit.c) points each entry at
     * its own dedicated, chain-wide deduplicated broadcast register.
     * linear_pass() (ops_asmgen.c) only ever reads these -- it doesn't
     * need to know which. */
    RasmOp lin[4][5];
} SwsAArch64OpRegs;

/*********************************************************************/
/* Immediate values: scalar constants broadcast to all vector lanes,
 * pre-loaded before the inner loop. No fixed register range -- each
 * one's home register is auto-picked (a64reg_vec(rs, -1)) *after* the
 * whole-chain value-flow ("banks") pass has finished for every op, so
 * it only ever claims a register value-flow genuinely isn't using,
 * instead of unconditionally reserving a fixed range regardless of how
 * many constants a given chain actually needs. Just a bound on array
 * size + a sanity assert now, not a register-numbering scheme.
 * SWS_UOP_LINEAR/SWS_UOP_LINEAR_FMA does NOT push its coefficients
 * through here -- see SWS_AARCH64_MAX_LIN_COEFF below for why a
 * dedicated-per-value broadcast register would be far too expensive
 * for those. */

#define SWS_AARCH64_MAX_IMM    5

/* Data pool: 128-bit constant vectors, same dynamic-placement story as
 * immediates above, loaded via adr + ldr from a pool after the
 * function. */

#define SWS_AARCH64_MAX_DATA_VECS 8

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

/* CLEAR: a masked component's value is compile-time-constant and
 * doesn't depend on loop position, but (unlike every other constant-
 * consuming uop) its home register is whatever the *consumer* already
 * expects -- often inside another op's contiguous register block (e.g.
 * WRITE_PACKED's st2/st3/st4) -- so it can't be redirected to an
 * auto-picked register the way jit_push_imm()/jit_push_v128() do.
 * Each masked component's already-coalesced target register and value
 * are recorded here instead, and loaded directly, once, in
 * load_constants(). */
#define SWS_AARCH64_MAX_CLEAR_HOIST 8

typedef struct SwsAArch64ClearHoist {
    RasmOp   target;
    uint32_t val;
} SwsAArch64ClearHoist;

/* SWS_UOP_LINEAR/SWS_UOP_LINEAR_FMA coefficients (JIT only): packs
 * arbitrary 32-bit values -- virtually never movi/mov-eligible, being
 * raw float bit patterns -- four per 16-byte data-pool entry/physical
 * register, read back by-element, exactly how CPS
 * (asmgen_setup_linear(), ops_static.c) already delivers coefficients.
 * Dedups by exact 32-bit value across the whole chain like
 * jit_push_imm(), but without paying a full dedicated register (and
 * data-pool slot) per single distinct value the way jit_push_imm()'s
 * always-broadcast, data-pool-fallback path does -- one LINEAR op
 * alone can have up to 4*5 = 20 distinct values, four times what
 * jit_push_imm() could ever afford to give each its own register. */
#define SWS_AARCH64_MAX_LIN_COEFF 20

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

    /* DITHER's runtime matrix pointer -- CPS (asmgen_setup_dither(),
     * ops_static.c) aliases this to tmp0, freshly loaded from
     * impl->priv every call; JIT (ops_jit.c) points it at a genuinely
     * persistent GPR, loaded once before the loop. Shared by
     * asmgen_op_dither() (ops_asmgen.c) either way. */
    RasmOp dither_src_ptr;

    /* CPS-related variables. */
    RasmOp op0_func;
    RasmOp op1_impl;
    RasmOp cont;
    RasmOp impl_priv;
    RasmNode *load_cont_node;
    SwsAArch64OpRegs regs;

    /* JIT-related variables. */
    /* Immediates. */
    SwsAArch64Immediate imm[SWS_AARCH64_MAX_IMM];
    int                 imm_count;

    SwsAArch64ConstVec  data[SWS_AARCH64_MAX_DATA_VECS];
    int                 data_count;

    SwsAArch64ClearHoist clear_hoist[SWS_AARCH64_MAX_CLEAR_HOIST];
    int                  clear_hoist_count;

    uint32_t lin_coeff[SWS_AARCH64_MAX_LIN_COEFF];
    RasmOp   lin_coeff_vec[(SWS_AARCH64_MAX_LIN_COEFF + 3) / 4];
    int      lin_coeff_count;

    /* DITHER (JIT only): the matrix pointer is too large to inline as
     * compile-time data, so its bytes are pushed into the data pool
     * like any jit_push_v128() vector via jit_push_dither_ptr(), which
     * records the pool index here so load_constants() can also read it
     * back as a scalar GPR (dither_src_ptr above), not just the vector
     * every other data-pool entry gets -- -1 when no DITHER op needs
     * it. `chain` owns the underlying malloc'd buffer from compile time
     * until the compiled function itself is torn down
     * (ff_sws_op_chain_free_cb(), see aarch64_jit_compile()) -- unused/
     * NULL for CPS. Only one pointer is tracked -- a chain with two
     * DITHER ops needing two live matrix pointers simultaneously isn't
     * supported (not needed by anything this targets today). */
    SwsOpChain *chain;
    int         dither_data_idx;

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

    /* TOOD */
    int block_size;
    SwsContext *sws;
} SwsAArch64Context;

/* Looping when s->use_vh is set. */
#define LOOP_VH(s, mask, idx) if (s->use_vh) LOOP(mask, idx)
#define LOOP_MASK_VH(s, p, idx) if (s->use_vh) LOOP_MASK(p, idx)
#define LOOP_MASK_BWD_VH(s, p, idx) if (s->use_vh) LOOP_MASK_BWD(p, idx)

#endif /* SWSCALE_AARCH64_OPS_ASMGEN_H */
