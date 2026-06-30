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

#include <string.h>

#include "../ops_chain.h"

#include "rasm.h"
#include "ops_impl.h"

typedef struct SwsAArch64UsedRegs {
    uint32_t used;
    uint32_t clobbered;
} SwsAArch64UsedRegs;

/*********************************************************************/
/* Immediate values: scalar constants broadcast to all vector lanes,  *
 * pre-loaded into v28..v31 before the inner loop.                    */

#define SWS_AARCH64_MAX_IMM    4
#define SWS_AARCH64_REGID_VIMM 28

/* Data pool: 128-bit constant vectors pre-loaded into v8..v15 before
 * the inner loop, loaded via adr + ldr from a pool after the function. */

#define SWS_AARCH64_MAX_DATA_VECS 8
#define SWS_AARCH64_REGID_VDATA   8

/* meta field packing: (small_value << 16) | (repeat_len << 8) | len
 *   len:        element size in bytes (1, 2, or 4)
 *   repeat_len: byte-level repeat unit (1 = u8, 2 = u16, 4 = u32)
 *   small_value: 1 if the repeat unit fits in 8 bits (movi-encodable) */
typedef struct SwsImm {
    uint32_t val;  /* full 32-bit broadcast pattern */
    uint32_t meta;
} SwsImm;

/*********************************************************************/
typedef struct SwsAArch64Context {
    /* TOOD */
    int block_size;
    SwsContext *sws;

    RasmNode *prologue;
    RasmNode *pre_loop;
    RasmNode *loop;
    RasmNode *epilogue;
    RasmNode *const_data;

    RasmContext *rctx;

    SwsAArch64UsedRegs gprs;
    SwsAArch64UsedRegs vecs;

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

    /* Vector registers. Two banks (low and high) are used. */
    RasmOp vl[ 4];
    RasmOp vh[ 4];
    RasmOp vt[12];

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

    /* Immediate values pre-loaded into v28..v31 before the inner loop. */
    SwsImm imm [SWS_AARCH64_MAX_IMM];
    int    n_imm;
    RasmOp vimm[SWS_AARCH64_MAX_IMM];

    /* 128-bit constant vectors pre-loaded into v8..v15 from a data pool. */
    uint32_t data[SWS_AARCH64_MAX_DATA_VECS * 4];
    int      n_data;
    RasmOp   vdata[SWS_AARCH64_MAX_DATA_VECS];
    int      data_label;
} SwsAArch64Context;

/*********************************************************************/
/* Per-op pre-allocated register assignments, filled during the setup
 * pass and consumed during the RASM-emission pass.                   */
typedef struct SwsAArch64OpRegs {
    union {
        struct { RasmOp shift_vec; RasmOp bitmask; } read_bit;
        struct { RasmOp shift_vec; } write_bit;
        struct { RasmOp nibble_mask; } read_nibble;
        struct { RasmOp mask[4]; } unpack;
        struct { RasmOp data_vec; } clear;
        struct { RasmOp data_vec; } min;
        struct { RasmOp data_vec; } max;
        struct { RasmOp vec; } scale;
        struct { RasmOp coeff[4]; int num_vregs; } linear;
    };
} SwsAArch64OpRegs;

/*********************************************************************/
static int jit_gpr(SwsAArch64Context *s, int r)
{
    if (r < 0) {
        if (!s->gprs.used)
            return -1;
        r = ff_ctz(~s->gprs.used);
    } else if (s->gprs.used & (1 << r)) {
        return -1;
    }
    s->gprs.used |= (1 << r);
    s->gprs.clobbered |= (1 << r);
    return r;
}

static RasmOp jit_gpw(SwsAArch64Context *s, int r)
{
    r = jit_gpr(s, r);
    if (r < 0)
        return rasm_op_none();
    return a64op_gpw(r);
}

static RasmOp jit_gpx(SwsAArch64Context *s, int r)
{
    r = jit_gpr(s, r);
    if (r < 0)
        return rasm_op_none();
    return a64op_gpx(r);
}

static void jit_free_gpr(SwsAArch64Context *s, RasmOp op)
{
    int r = a64op_gpr_n(op);
    s->gprs.used &= ~(1 << r);
}

/*********************************************************************/
/* Immediate collection helpers.
 * Called exclusively during the setup pass (aarch64_setup), deduplicating
 * by value and allocating slots in v28..v31 for later use in asmgen_op_*. */

static int jit_push_imm32(SwsAArch64Context *s, uint32_t val, int len)
{
    for (int i = 0; i < s->n_imm; i++) {
        if (s->imm[i].val == val)
            return i;
    }

    union {
        uint32_t u32;
        uint16_t u16[2];
        uint8_t  u8[4];
    } u;
    u.u32 = val;

    int repeat_len;
    int small_value;
    if (u.u16[0] != u.u16[1]) {
        repeat_len  = 4;
        small_value = (u.u32 < 0x100);
    } else if (u.u8[0] != u.u8[1]) {
        repeat_len  = 2;
        small_value = (u.u16[0] < 0x100);
    } else {
        repeat_len  = 1;
        small_value = 1;
    }

    int idx = s->n_imm++;
    av_assert0(idx < SWS_AARCH64_MAX_IMM);
    s->imm[idx].val  = val;
    s->imm[idx].meta = ((uint32_t) small_value << 16)
                     | ((uint32_t) repeat_len  <<  8)
                     | ((uint32_t) len);
    s->vimm[idx] = a64op_vecq(SWS_AARCH64_REGID_VIMM + idx);
    return idx;
}

static int jit_push_imm16(SwsAArch64Context *s, uint16_t val, int len)
{
    uint32_t v = val | ((uint32_t) val << 16);
    return jit_push_imm32(s, v, len);
}

static int jit_push_imm8(SwsAArch64Context *s, uint8_t val, int len)
{
    uint16_t v = val | ((uint16_t) val << 8);
    return jit_push_imm16(s, v, len);
}

static int jit_push_imm32_op(SwsAArch64Context *s, SwsPixelType type, uint32_t val)
{
    if (type == SWS_PIXEL_U8)  return jit_push_imm8 (s, (uint8_t)  val, 1);
    if (type == SWS_PIXEL_U16) return jit_push_imm16(s, (uint16_t) val, 2);
    return jit_push_imm32(s, val, 4);
}

/* Push a 128-bit constant vector (given as 4 uint32 words) into the data
 * pool, deduplicating by value.  Returns the slot index (0-based). */
static int jit_push_data(SwsAArch64Context *s, const uint32_t words[4])
{
    for (int i = 0; i < s->n_data; i++) {
        if (!memcmp(&s->data[i * 4], words, 16))
            return i;
    }
    int idx = s->n_data++;
    av_assert0(idx < SWS_AARCH64_MAX_DATA_VECS);
    memcpy(&s->data[idx * 4], words, 16);
    s->vdata[idx] = a64op_vecq(SWS_AARCH64_REGID_VDATA + idx);
    return idx;
}

/* Emit load instructions for all collected immediates (v28..v31) and
 * 128-bit data pool vectors (v8..v15) before the inner loop.
 * Uses tmp0 as a scratch GPR for values that cannot be encoded by movi,
 * and as the base pointer for adr + ldr of the data pool. */
static void load_constants(SwsAArch64Context *s)
{
    RasmContext *r = s->rctx;

    if (s->n_imm) {
        rasm_add_comment(r, "immediates");
        RasmOp tmp = a64op_w(s->tmp0);

        /* First load large values into the scratch GPR, then movi small
         * values into vectors, then dup the scratch values into vectors.
         * Separating the passes allows the CPU to overlap integer and
         * vector instruction execution. */
        for (int i = 0; i < s->n_imm; i++) {
            int small_value = (int) (s->imm[i].meta >> 16);
            int repeat_len  = (int) ((s->imm[i].meta >> 8) & 0xff);
            if (!small_value && repeat_len != 1) {
                switch (repeat_len) {
                case 2: i_mov(r, tmp, IMM((int32_t) (s->imm[i].val & 0xffff))); break;
                case 4: i_mov(r, tmp, IMM((int32_t)  s->imm[i].val          )); break;
                }
            }
        }
        for (int i = 0; i < s->n_imm; i++) {
            int small_value = (int) (s->imm[i].meta >> 16);
            int repeat_len  = (int) ((s->imm[i].meta >> 8) & 0xff);
            if (small_value) {
                uint32_t byte_val = s->imm[i].val & 0xff;
                switch (repeat_len) {
                case 1: i_movi(r, v_16b(s->vimm[i]), IMM(byte_val)); break;
                case 2: i_movi(r, v_8h (s->vimm[i]), IMM(byte_val)); break;
                case 4: i_movi(r, v_4s (s->vimm[i]), IMM(byte_val)); break;
                }
            }
        }
        for (int i = 0; i < s->n_imm; i++) {
            int small_value = (int) (s->imm[i].meta >> 16);
            int repeat_len  = (int) ((s->imm[i].meta >> 8) & 0xff);
            int len         = (int)  (s->imm[i].meta & 0xff);
            if (!small_value && repeat_len != 1) {
                switch (len) {
                case 2: i_dup(r, v_8h(s->vimm[i]), tmp); break;
                case 4: i_dup(r, v_4s(s->vimm[i]), tmp); break;
                }
            }
        }
    }

    if (s->n_data) {
        rasm_add_comment(r, "data pool");
        RasmOp ptr = s->tmp0;
        s->data_label = rasm_new_label(r, "ldata");
        i_adr(r, ptr, rasm_op_label(s->data_label));
        for (int i = 0; i < s->n_data; i++)
            i_ldr(r, s->vdata[i], a64op_off(ptr, (int16_t) (i * 16)));
    }
}

/*********************************************************************/
static int aarch64_jit_setup_linear(const SwsAArch64OpImplParams *p,
                                    const SwsOp *op, SwsImplResult *res)
{
    /**
     * Compute number of full vector registers needed to pack all non-zero
     * coefficients.
     */
    const int num_vregs = linear_num_vregs(p);
    av_assert0(num_vregs <= 4);
    float *coeffs = av_malloc(num_vregs * 4 * sizeof(float));
    if (!coeffs)
        return AVERROR(ENOMEM);

    /**
     * Copy non-zero coefficients, packed in sequential order, offset first.
     * The same order must be followed in asmgen_op_linear().
     */
    int i_coeff = 0;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 5; j++) {
            const int jj = (j == 0) ? 4 : (j - 1);
            if (!(p->par.lin.zero & SWS_MASK(i, jj)))
                coeffs[i_coeff++] = (float) op->lin.m[i][jj].num / op->lin.m[i][jj].den;
        }
    }

    res->priv.ptr = coeffs;
    res->free = ff_op_priv_free;

    return 0;
}

/*********************************************************************/
static int aarch64_jit_setup_dither(const SwsAArch64OpImplParams *p,
                                    const SwsOp *op, SwsImplResult *res)
{
    /**
     * The input dither matrix is (1 << size_log2)² pixels large. It is
     * periodic, so the x and y offsets should be masked to fit inside
     * (1 << size_log2).
     * The width of the matrix is assumed to be at least 8, which matches
     * the maximum block_size for aarch64 asmgen when f32 operations
     * (i.e., dithering) are used. This guarantees that the x offset is
     * aligned and that reading block_size elements does not extend past
     * the end of the row. The x offset doesn't change between components,
     * so it is only required to be masked once.
     * The y offset, on the other hand, may change per component, and
     * would therefore need to be masked for every y_offset value. To
     * simplify the execution, we over-allocate the number of rows of
     * the output dither matrix by the largest y_offset value. This way,
     * we only need to mask y offset once, and can safely increment the
     * dither matrix pointer by fixed offsets for every y_offset change.
     */

    /* Find the largest y_offset value. */
    const int size = 1 << op->dither.size_log2;
    const int8_t *off = op->dither.y_offset;
    int max_offset = 0;
    for (int i = 0; i < 4; i++) {
        if (off[i] >= 0)
            max_offset = FFMAX(max_offset, off[i] & (size - 1));
    }

    /* Allocate (size + max_offset) rows to allow over-reading the matrix. */
    const int stride = size * sizeof(float);
    const int num_rows = size + max_offset;
    float *matrix = av_malloc(num_rows * stride);
    if (!matrix)
        return AVERROR(ENOMEM);

    for (int i = 0; i < size * size; i++)
        matrix[i] = (float) op->dither.matrix[i].num / op->dither.matrix[i].den;

    memcpy(&matrix[size * size], matrix, max_offset * stride);

    res->priv.ptr = matrix;
    res->free = ff_op_priv_free;

    return 0;
}

/*********************************************************************/
static int aarch64_jit_setup(const SwsOpList *ops, int block_size, int n,
                             const SwsAArch64OpImplParams *p, SwsImplResult *out)
{
    SwsOp *op = &ops->ops[n];
    switch (op->op) {
    case SWS_OP_CLEAR:
        ff_sws_setup_clear(&(const SwsImplParams) { .op = op }, out);
        break;
    case SWS_OP_MIN:
    case SWS_OP_MAX:
        ff_sws_setup_clamp(&(const SwsImplParams) { .op = op }, out);
        break;
    case SWS_OP_SCALE:
        ff_sws_setup_scale(&(const SwsImplParams) { .op = op }, out);
        break;
    case SWS_OP_LINEAR:
        return aarch64_jit_setup_linear(p, op, out);
    case SWS_OP_DITHER:
        return aarch64_jit_setup_dither(p, op, out);
    }
    return 0;
}

/*********************************************************************/
/* Helpers functions. */

/* Looping when s->use_vh is set. */
#define LOOP_VH(s, mask, idx) if (s->use_vh) LOOP(mask, idx)
#define LOOP_MASK_VH(s, p, idx) if (s->use_vh) LOOP_MASK(p, idx)
#define LOOP_MASK_BWD_VH(s, p, idx) if (s->use_vh) LOOP_MASK_BWD(p, idx)

/* Inline rasm comments. */
#define CMT(comment)   rasm_annotate(r, comment)
#define CMTF(fmt, ...) rasm_annotatef(r, (char[128]){0}, 128, fmt, __VA_ARGS__)

/* Reshape all vector registers for current SwsOp. */
static void reshape_all_vectors(SwsAArch64Context *s, int el_count, int el_size)
{
    s->vl[ 0] = a64op_make_vec( 0, el_count, el_size);
    s->vl[ 1] = a64op_make_vec( 1, el_count, el_size);
    s->vl[ 2] = a64op_make_vec( 2, el_count, el_size);
    s->vl[ 3] = a64op_make_vec( 3, el_count, el_size);
    s->vh[ 0] = a64op_make_vec( 4, el_count, el_size);
    s->vh[ 1] = a64op_make_vec( 5, el_count, el_size);
    s->vh[ 2] = a64op_make_vec( 6, el_count, el_size);
    s->vh[ 3] = a64op_make_vec( 7, el_count, el_size);
    s->vt[ 0] = a64op_make_vec(16, el_count, el_size);
    s->vt[ 1] = a64op_make_vec(17, el_count, el_size);
    s->vt[ 2] = a64op_make_vec(18, el_count, el_size);
    s->vt[ 3] = a64op_make_vec(19, el_count, el_size);
    s->vt[ 4] = a64op_make_vec(20, el_count, el_size);
    s->vt[ 5] = a64op_make_vec(21, el_count, el_size);
    s->vt[ 6] = a64op_make_vec(22, el_count, el_size);
    s->vt[ 7] = a64op_make_vec(23, el_count, el_size);
    s->vt[ 8] = a64op_make_vec(24, el_count, el_size);
    s->vt[ 9] = a64op_make_vec(25, el_count, el_size);
    s->vt[10] = a64op_make_vec(26, el_count, el_size);
    s->vt[11] = a64op_make_vec(27, el_count, el_size);
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
                               SwsCompMask mask,
                               RasmOp regs[MAX_SAVED_REGS])
{
    unsigned count = 0;
    clobber_gpr(regs, &count, a64op_lr());
    LOOP(mask, i) {
        clobber_gpr(regs, &count, s->in[i]);
        clobber_gpr(regs, &count, s->out[i]);
        clobber_gpr(regs, &count, s->in_bump[i]);
        clobber_gpr(regs, &count, s->out_bump[i]);
    }
    return count;
}

static int aarch64_jit_process(SwsAArch64Context *s, const SwsAArch64OpImplParams *pin, const SwsAArch64OpImplParams *pout)
{
    SwsCompMask imask = pin->mask;
    SwsCompMask omask = pout->mask;

    RasmContext *r = s->rctx;
    char func_name[128];
    char buf[64];

    /**
     * TODO comment
     * The process function for aarch64 works similarly to the x86 backend.
     * The description in x86/ops_include.asm mostly holds as well here.
     */

    snprintf(func_name, sizeof(func_name), "ff_sws_process_%04x_%04x_neon", nibble_mask(pin->mask), nibble_mask(pout->mask));

    rasm_func_begin(r, func_name, true, false);

    /* SwsOpFunc arguments. */
    s->exec      = jit_gpx(s, 0); // const SwsOpExec *exec
    s->impl      = jit_gpx(s, 1); // const void *priv
    s->bx_start  = jit_gpw(s, 2); // int bx_start
    s->y_start   = jit_gpw(s, 3); // int y_start
    s->bx_end    = jit_gpw(s, 4); // int bx_end
    s->y_end     = jit_gpw(s, 5); // int y_end

    /* local variables */
    s->bx        = jit_gpw(s, 6); // int bx
    s->y         = s->y_start;    /* Reused from SwsOpFunc argument. */

    /* Function prologue */
    s->prologue = rasm_get_current_node(r);

    /* Load values from exec. */
    rasm_add_comment(r, "init");
    LOOP(imask, i) {
        rasm_annotate_nextf(r, buf, sizeof(buf), "in[%u] = exec->in[%u];", i, i);
        s->in[i] = jit_gpx(s, -1);
        i_ldr(r, s->in[i],       a64op_off(s->exec, offsetof_exec_in       + (i * sizeof(uint8_t *))));
    }
    LOOP(omask, i) {
        rasm_annotate_nextf(r, buf, sizeof(buf), "out[%u] = exec->out[%u];", i, i);
        s->out[i] = jit_gpx(s, -1);
        i_ldr(r, s->out[i],      a64op_off(s->exec, offsetof_exec_out      + (i * sizeof(uint8_t *))));
    }
    LOOP(imask, i) {
        rasm_annotate_nextf(r, buf, sizeof(buf), "in_bump[%u] = exec->in_bump[%u];", i, i);
        s->in_bump[i] = jit_gpx(s, -1);
        i_ldr(r, s->in_bump[i],  a64op_off(s->exec, offsetof_exec_in_bump  + (i * sizeof(ptrdiff_t))));
    }
    LOOP(omask, i) {
        rasm_annotate_nextf(r, buf, sizeof(buf), "out_bump[%u] = exec->out_bump[%u];", i, i);
        s->out_bump[i] = jit_gpx(s, -1);
        i_ldr(r, s->out_bump[i], a64op_off(s->exec, offsetof_exec_out_bump + (i * sizeof(ptrdiff_t))));
    }

    s->tmp0 = s->exec;
    s->tmp1 = s->impl;

    load_constants(s);

    s->pre_loop = rasm_get_current_node(r);

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

    /* TODO Reset impl and call first kernel. */
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
    s->epilogue = rasm_get_current_node(r);

    i_ret(r);

    s->const_data = rasm_get_current_node(r);

    return 0;
}

/*********************************************************************/
/* gather raw pixels from planes */
/* SWS_UOP_READ_BIT */
/* SWS_UOP_READ_NIBBLE */
/* SWS_UOP_READ_PACKED */
/* SWS_UOP_READ_PLANAR */

static void asmgen_op_read_bit(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                               const SwsAArch64OpRegs *regs)
{
    RasmContext *r = s->rctx;
    RasmOp wtmp = a64op_w(s->tmp0);
    AArch64VecViews vl[1];
    AArch64VecViews vtmp;
    AArch64VecViews shift_vec;

    a64op_vec_views(regs->read_bit.shift_vec, &shift_vec);
    a64op_vec_views(s->vl[0], &vl[0]);
    a64op_vec_views(s->vt[1], &vtmp);

    /* Note that shift_vec has negative values, so that using it with
     * ushl actually performs a right shift. */
    if (p->block_size == 16) {
        i_ldrh(r, wtmp,                        a64op_post(s->in[0], 2));    CMT("uint16_t tmp = *in[0]++;");
        i_dup (r, vl[0].b8,                    wtmp);                       CMT("vl[0].lo = broadcast(tmp);");
        i_lsr (r, wtmp,                        wtmp, IMM(8));               CMT("tmp >>= 8;");
        i_dup (r, vtmp.b8,                     wtmp);                       CMT("vtmp.lo = broadcast(tmp);");
        i_ins (r, vl[0].de[1],                 vtmp.de[0]);                 CMT("vl[0].hi = vtmp.lo;");
        i_ushl(r, vl[0].b16,                   vl[0].b16, shift_vec.b16);   CMT("vl[0] <<= shift_vec;");
        i_and (r, vl[0].b16,                   vl[0].b16, regs->read_bit.bitmask); CMT("vl[0] &= bitmask_vec;");
    } else {
        i_ldrb(r, wtmp,                        a64op_post(s->in[0], 1));    CMT("uint8_t tmp = *in[0]++;");
        i_dup (r, vl[0].b8,                    wtmp);                       CMT("vl[0].lo = broadcast(tmp);");
        i_ushl(r, vl[0].b8,                    vl[0].b8,  shift_vec.b8);    CMT("vl[0] <<= shift_vec;");
        i_and (r, vl[0].b8,                    vl[0].b8,  regs->read_bit.bitmask); CMT("vl[0] &= bitmask_vec;");
    }
}

static void asmgen_op_read_nibble(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                                  const SwsAArch64OpRegs *regs)
{
    RasmContext *r = s->rctx;
    RasmOp nibble_mask = v_8b(regs->read_nibble.nibble_mask);
    AArch64VecViews vl[1];
    AArch64VecViews vtmp;

    a64op_vec_views(s->vl[0], &vl[0]);
    a64op_vec_views(s->vt[0], &vtmp);

    if (p->block_size == 8) {
        i_ldr (r, vl[0].s,   a64op_post(s->in[0], 4));  CMT("vl[0] = *in[0]++;");
        i_ushr(r, vtmp.b8,   vl[0].b8, IMM(4));         CMT("vtmp.lo = vl[0] >> 4;");
        i_and (r, vl[0].b8,  vl[0].b8, nibble_mask);    CMT("vl[0].lo &= nibble_mask;");
        i_zip1(r, vl[0].b8,  vtmp.b8,  vl[0].b8);       CMT("interleave");
    } else {
        i_ldr (r, vl[0].d,   a64op_post(s->in[0], 8));  CMT("vl[0] = *in[0]++;");
        i_ushr(r, vtmp.b8,   vl[0].b8, IMM(4));         CMT("vtmp.lo = vl[0] >> 4;");
        i_and (r, vl[0].b8,  vl[0].b8, nibble_mask);    CMT("vl[0].lo &= nibble_mask;");
        i_zip1(r, vl[0].b16, vtmp.b16, vl[0].b16);      CMT("interleave");
    }
}

static void asmgen_op_read_packed_n(SwsAArch64Context *s, const SwsAArch64OpImplParams *p, RasmOp *vx)
{
    RasmContext *r = s->rctx;

    switch (p->mask) {
    case SWS_COMP_MASK(1, 1, 0, 0): i_ld2(r, vv_2(vx[0], vx[1]),               a64op_post(s->in[0], s->vec_size * 2)); break;
    case SWS_COMP_MASK(1, 1, 1, 0): i_ld3(r, vv_3(vx[0], vx[1], vx[2]),        a64op_post(s->in[0], s->vec_size * 3)); break;
    case SWS_COMP_MASK(1, 1, 1, 1): i_ld4(r, vv_4(vx[0], vx[1], vx[2], vx[3]), a64op_post(s->in[0], s->vec_size * 4)); break;
    }
}

static void asmgen_op_read_packed(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    av_assert0(p->mask != 0x0001);
    asmgen_op_read_packed_n(s, p, s->vl);
    if (s->use_vh)
        asmgen_op_read_packed_n(s, p, s->vh);
}

static void asmgen_op_read_planar(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    RasmContext *r = s->rctx;
    AArch64VecViews vl[4];
    AArch64VecViews vh[4];

    for (int i = 0; i < 4; i++) {
        a64op_vec_views(s->vl[i], &vl[i]);
        a64op_vec_views(s->vh[i], &vh[i]);
    }

    LOOP_MASK(p, i) {
        switch ((s->use_vh ? 0x100 : 0) | s->vec_size) {
        case 0x008: i_ldr(r, vl[i].d,          a64op_post(s->in[i], s->vec_size * 1)); break;
        case 0x010: i_ldr(r, vl[i].q,          a64op_post(s->in[i], s->vec_size * 1)); break;
        case 0x108: i_ldp(r, vl[i].d, vh[i].d, a64op_post(s->in[i], s->vec_size * 2)); break;
        case 0x110: i_ldp(r, vl[i].q, vh[i].q, a64op_post(s->in[i], s->vec_size * 2)); break;
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
                                const SwsAArch64OpRegs *regs)
{
    RasmContext *r = s->rctx;
    AArch64VecViews vl[1];
    AArch64VecViews shift_vec;
    AArch64VecViews vtmp0;
    AArch64VecViews vtmp1;

    a64op_vec_views(s->vl[0], &vl[0]);
    a64op_vec_views(regs->write_bit.shift_vec, &shift_vec);
    a64op_vec_views(s->vt[1], &vtmp0);
    a64op_vec_views(s->vt[2], &vtmp1);

    if (p->block_size == 8) {
        i_ushl(r, vl[0].b8,    vl[0].b8,   shift_vec.b8);   CMT("vl[0] <<= shift_vec;");
        i_addv(r, vtmp0.b,     vl[0].b8);                   CMT("vtmp0[0] = add_across(vl[0].lo);");
        i_str (r, vtmp0.b,     a64op_post(s->out[0], 1));   CMT("*out[0]++ = vtmp0;");
    } else {
        i_ushl(r, vl[0].b16,   vl[0].b16,  shift_vec.b16);  CMT("vl[0] <<= shift_vec;");
        i_addv(r, vtmp0.b,     vl[0].b8);                   CMT("vtmp0[0] = add_across(vl[0].lo);");
        i_ins (r, vtmp1.de[0], vl[0].de[1]);                CMT("vtmp1.lo = vl[0].hi;");
        i_addv(r, vtmp1.b,     vtmp1.b8);                   CMT("vtmp1[0] = add_across(vtmp1);");
        i_ins (r, vtmp0.be[1], vtmp1.be[0]);                CMT("vtmp0[1] = vtmp1[0];");
        i_str (r, vtmp0.h,     a64op_post(s->out[0], 2));   CMT("*out[0]++ = vtmp0;");
    }
}

static void asmgen_op_write_nibble(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    RasmContext *r = s->rctx;
    AArch64VecViews vl[4];
    AArch64VecViews vtmp0;
    AArch64VecViews vtmp1;

    for (int i = 0; i < 4; i++)
        a64op_vec_views(s->vl[i], &vl[i]);
    a64op_vec_views(s->vt[0], &vtmp0);
    a64op_vec_views(s->vt[1], &vtmp1);

    if (p->block_size == 8) {
        i_shl (r, vtmp0.h4,  vl[0].h4,  IMM(4));
        i_ushr(r, vtmp1.h4,  vl[0].h4,  IMM(8));
        i_orr (r, vl[0].b8,  vtmp0.b8,  vtmp1.b8);
        i_xtn (r, vtmp0.b8,  vl[0].h8);
        i_str (r, vtmp0.s,   a64op_post(s->out[0], 4));
    } else {
        i_shl (r, vtmp0.h8,  vl[0].h8,  IMM(4));
        i_ushr(r, vtmp1.h8,  vl[0].h8,  IMM(8));
        i_orr (r, vl[0].b16, vtmp0.b16, vtmp1.b16);
        i_xtn (r, vtmp0.b8,  vl[0].h8);
        i_str (r, vtmp0.d,   a64op_post(s->out[0], 8));
    }
}

static void asmgen_op_write_packed_n(SwsAArch64Context *s, const SwsAArch64OpImplParams *p, RasmOp *vx)
{
    RasmContext *r = s->rctx;

    switch (p->mask) {
    case SWS_COMP_MASK(1, 1, 0, 0): i_st2(r, vv_2(vx[0], vx[1]),               a64op_post(s->out[0], s->vec_size * 2)); break;
    case SWS_COMP_MASK(1, 1, 1, 0): i_st3(r, vv_3(vx[0], vx[1], vx[2]),        a64op_post(s->out[0], s->vec_size * 3)); break;
    case SWS_COMP_MASK(1, 1, 1, 1): i_st4(r, vv_4(vx[0], vx[1], vx[2], vx[3]), a64op_post(s->out[0], s->vec_size * 4)); break;
    }
}

static void asmgen_op_write_packed(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    av_assert0(p->mask != 0x0001);
    asmgen_op_write_packed_n(s, p, s->vl);
    if (s->use_vh)
        asmgen_op_write_packed_n(s, p, s->vh);
}

static void asmgen_op_write_planar(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    RasmContext *r = s->rctx;
    AArch64VecViews vl[4];
    AArch64VecViews vh[4];

    for (int i = 0; i < 4; i++) {
        a64op_vec_views(s->vl[i], &vl[i]);
        a64op_vec_views(s->vh[i], &vh[i]);
    }

    LOOP_MASK(p, i) {
        switch ((s->use_vh ? 0x100 : 0) | s->vec_size) {
        case 0x008: i_str(r, vl[i].d,          a64op_post(s->out[i], s->vec_size * 1)); break;
        case 0x010: i_str(r, vl[i].q,          a64op_post(s->out[i], s->vec_size * 1)); break;
        case 0x108: i_stp(r, vl[i].d, vh[i].d, a64op_post(s->out[i], s->vec_size * 2)); break;
        case 0x110: i_stp(r, vl[i].q, vh[i].q, a64op_post(s->out[i], s->vec_size * 2)); break;
        }
    }
}

/*********************************************************************/
/* swap byte order (for differing endianness) */
/* SWS_UOP_SWAP_BYTES */

static void asmgen_op_swap_bytes(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    RasmContext *r = s->rctx;
    AArch64VecViews vl[4];
    AArch64VecViews vh[4];

    for (int i = 0; i < 4; i++) {
        a64op_vec_views(s->vl[i], &vl[i]);
        a64op_vec_views(s->vh[i], &vh[i]);
    }

    switch (ff_sws_pixel_type_size(p->type)) {
    case sizeof(uint16_t):
        LOOP_MASK      (p, i) i_rev16(r, vl[i].b16, vl[i].b16);
        LOOP_MASK_VH(s, p, i) i_rev16(r, vh[i].b16, vh[i].b16);
        break;
    case sizeof(uint32_t):
        LOOP_MASK      (p, i) i_rev32(r, vl[i].b16, vl[i].b16);
        LOOP_MASK_VH(s, p, i) i_rev32(r, vh[i].b16, vh[i].b16);
        break;
    }
}

/*********************************************************************/
/* rearrange channel order, or duplicate channels */
/* SWS_UOP_SWIZZLE */

static const char *print_swizzle_v(char buf[8], int8_t n, uint8_t vh)
{
    if (n == -1)
        snprintf(buf, sizeof(char[8]), "vtmp%c", vh ? 'h' : 'l');
    else
        snprintf(buf, sizeof(char[8]), "v%c[%u]", vh ? 'h' : 'l', n);
    return buf;
}
#define PRINT_SWIZZLE_V(n, vh) print_swizzle_v((char[8]){ 0 }, n, vh)

static RasmOp swizzle_a64op(SwsAArch64Context *s, int8_t n, uint8_t vh)
{
    if (n == -1)
        return s->vt[vh];
    return vh ? s->vh[n] : s->vl[n];
}

static void swizzle_emit(SwsAArch64Context *s, int8_t dst, int8_t src)
{
    RasmContext *r = s->rctx;
    RasmOp src_op[2] = { swizzle_a64op(s, src, 0), swizzle_a64op(s, src, 1) };
    RasmOp dst_op[2] = { swizzle_a64op(s, dst, 0), swizzle_a64op(s, dst, 1) };

    i_mov    (r, dst_op[0], src_op[0]); CMTF("%s = %s;", PRINT_SWIZZLE_V(dst, 0), PRINT_SWIZZLE_V(src, 0));
    if (s->use_vh) {
        i_mov(r, dst_op[1], src_op[1]); CMTF("%s = %s;", PRINT_SWIZZLE_V(dst, 1), PRINT_SWIZZLE_V(src, 1));
    }
}

static void asmgen_op_move(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    for (int i = 0; i < p->par.move.num_moves; i++)
        swizzle_emit(s, p->par.move.dst[i], p->par.move.src[i]);
}

/*********************************************************************/
/* split tightly packed data into components */
/* SWS_UOP_UNPACK */

static void asmgen_op_unpack(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                             const SwsAArch64OpRegs *regs)
{
    RasmContext *r = s->rctx;
    RasmOp *vl = s->vl;
    RasmOp *vh = s->vh;

    const int offsets[4] = {
        p->par.pack.pattern[3] + p->par.pack.pattern[2] + p->par.pack.pattern[1],
        p->par.pack.pattern[3] + p->par.pack.pattern[2],
        p->par.pack.pattern[3],
        0
    };

    /* Loop backwards to avoid clobbering component 0. */
    LOOP_MASK_BWD      (p, i) {
        if (offsets[i]) {
            i_ushr  (r, vl[i], vl[0], IMM(offsets[i])); CMTF("vl[%u] >>= %u;", i, offsets[i]);
        } else if (i) {
            i_mov16b(r, vl[i], vl[0]);                  CMTF("vl[%u] = vl[0];", i);
        }
    }
    LOOP_MASK_BWD_VH(s, p, i) {
        if (offsets[i]) {
            i_ushr  (r, vh[i], vh[0], IMM(offsets[i])); CMTF("vh[%u] >>= %u;", i, offsets[i]);
        } else if (i) {
            i_mov16b(r, vh[i], vh[0]);                  CMTF("vh[%u] = vh[0];", i);
        }
    }

    /* Apply masks. */
    reshape_all_vectors(s, 16, 1);
    LOOP_MASK_BWD(p, i) {
        i_and(r, vl[i], vl[i], regs->unpack.mask[i]);
        CMTF("vl[%u] &= 0x%x;", i, (1u << p->par.pack.pattern[i]) - 1);
    }
    LOOP_MASK_BWD_VH(s, p, i) {
        i_and(r, vh[i], vh[i], regs->unpack.mask[i]);
        CMTF("vh[%u] &= 0x%x;", i, (1u << p->par.pack.pattern[i]) - 1);
    }
}

/*********************************************************************/
/* compress components into tightly packed data */
/* SWS_UOP_PACK */

static void asmgen_op_pack(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    RasmContext *r = s->rctx;
    RasmOp *vl = s->vl;
    RasmOp *vh = s->vh;

    const int offsets[4] = {
        p->par.pack.pattern[3] + p->par.pack.pattern[2] + p->par.pack.pattern[1],
        p->par.pack.pattern[3] + p->par.pack.pattern[2],
        p->par.pack.pattern[3],
        0
    };
    SwsCompMask offset_mask = 0;
    LOOP_MASK(p, i) {
        if (offsets[i])
            offset_mask |= SWS_COMP(i);
    }

    /* Perform left shift. */
    LOOP      (offset_mask, i) { i_shl(r, vl[i], vl[i], IMM(offsets[i])); CMTF("vl[%u] <<= %u;", i, offsets[i]); }
    LOOP_VH(s, offset_mask, i) { i_shl(r, vh[i], vh[i], IMM(offsets[i])); CMTF("vh[%u] <<= %u;", i, offsets[i]); }

    /* Combine components. */
    reshape_all_vectors(s, 16, 1);
    LOOP_MASK      (p, i) {
        if (i != 0) {
            i_orr    (r, vl[0], vl[0], vl[i]); CMTF("vl[0] |= vl[%u];", i);
            if (s->use_vh) {
                i_orr(r, vh[0], vh[0], vh[i]); CMTF("vh[0] |= vh[%u];", i);
            }
        }
    }
}

/*********************************************************************/
/* logical left shift of raw pixel values */
/* SWS_UOP_LSHIFT */

static void asmgen_op_lshift(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    uint8_t shift = p->par.shift.amount;
    RasmContext *r = s->rctx;
    RasmOp *vl = s->vl;
    RasmOp *vh = s->vh;

    LOOP_MASK      (p, i) { i_shl(r, vl[i], vl[i], IMM(shift)); CMTF("vl[%u] <<= %u;", i, shift); }
    LOOP_MASK_VH(s, p, i) { i_shl(r, vh[i], vh[i], IMM(shift)); CMTF("vh[%u] <<= %u;", i, shift); }
}

/*********************************************************************/
/* right shift of raw pixel values */
/* SWS_UOP_RSHIFT */

static void asmgen_op_rshift(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    uint8_t shift = p->par.shift.amount;
    RasmContext *r = s->rctx;
    RasmOp *vl = s->vl;
    RasmOp *vh = s->vh;

    LOOP_MASK      (p, i) { i_ushr(r, vl[i], vl[i], IMM(shift)); CMTF("vl[%u] >>= %u;", i, shift); }
    LOOP_MASK_VH(s, p, i) { i_ushr(r, vh[i], vh[i], IMM(shift)); CMTF("vh[%u] >>= %u;", i, shift); }
}

/*********************************************************************/
/* clear pixel values */
/* SWS_UOP_CLEAR */

static void emit_clear(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                       RasmOp clear_vec,
                       RasmOp *vx, int i, const char *vx_str)
{
    RasmContext *r = s->rctx;
    if (SWS_COMP_TEST(p->par.clear.zero, i)) {
        i_movi(r, vx[i], IMM(0));                   CMTF("%s[%u] = 0;", vx_str, i);
    } else if (SWS_COMP_TEST(p->par.clear.one, i)) {
        if (p->block_size * ff_sws_pixel_type_size(p->type) == 8) {
            i_movi(r, v_8b (vx[i]), IMM(0xff));
        } else {
            i_movi(r, v_16b(vx[i]), IMM(0xff));
        }
        CMTF("%s[%u] = UINT_MAX;", vx_str, i);
    } else {
        i_dup (r, vx[i], a64op_elem(clear_vec, i)); CMTF("%s[%u] = broadcast(clear_vec[%u]);", vx_str, i, i);
    }
}

static void asmgen_op_clear(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                            const SwsAArch64OpRegs *regs)
{
    LOOP_MASK      (p, i) { emit_clear(s, p, regs->clear.data_vec, s->vl, i, "vl"); }
    LOOP_MASK_VH(s, p, i) { emit_clear(s, p, regs->clear.data_vec, s->vh, i, "vh"); }
}

/*********************************************************************/
/* convert (cast) between formats */
/* SWS_UOP_TO_U8 */
/* SWS_UOP_TO_U16 */
/* SWS_UOP_TO_U32 */
/* SWS_UOP_TO_F32 */

static void asmgen_op_convert(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    RasmContext *r = s->rctx;
    AArch64VecViews vl[4];
    AArch64VecViews vh[4];

    /**
     * Since each instruction in the convert operation needs specific
     * element types, it is simpler to use arrangement specifiers for
     * each operand instead of reshaping all vectors.
     */

    for (int i = 0; i < 4; i++) {
        a64op_vec_views(s->vl[i], &vl[i]);
        a64op_vec_views(s->vh[i], &vh[i]);
    }

    size_t src_el_size = s->el_size;
    SwsPixelType to_type;
    switch (p->uop) {
    case SWS_UOP_TO_U8:  to_type = SWS_PIXEL_U8;  break;
    case SWS_UOP_TO_U16: to_type = SWS_PIXEL_U16; break;
    case SWS_UOP_TO_U32: to_type = SWS_PIXEL_U32; break;
    case SWS_UOP_TO_F32: to_type = SWS_PIXEL_F32; break;
    default:
        av_assert0(!"Invalid uop!");
        break;
    }
    size_t dst_el_size = ff_sws_pixel_type_size(to_type);

    /**
     * This function assumes block_size is either 8 or 16, and that
     * we're always using the most amount of vector registers possible.
     * Therefore, u32 always uses the high vector bank.
     */
    if (p->type == SWS_PIXEL_F32) {
        rasm_add_comment(r, "f32 -> u32");
        LOOP_MASK(p, i) i_fcvtzu(r, vl[i].s4, vl[i].s4);
        LOOP_MASK(p, i) i_fcvtzu(r, vh[i].s4, vh[i].s4);
    }

    if (p->block_size == 8) {
        if (src_el_size == 1 && dst_el_size > src_el_size) {
            rasm_add_comment(r, "u8 -> u16");
            LOOP_MASK(p, i) i_uxtl (r, vl[i].h8,    vl[i].b8);
            src_el_size = 2;
        } else if (src_el_size == 4 && dst_el_size < src_el_size) {
            rasm_add_comment(r, "u32 -> u16");
            LOOP_MASK(p, i) i_xtn  (r, vl[i].h4,    vl[i].s4);
            LOOP_MASK(p, i) i_xtn  (r, vh[i].h4,    vh[i].s4);
            LOOP_MASK(p, i) i_ins  (r, vl[i].de[1], vh[i].de[0]);
            src_el_size = 2;
        }
        if (src_el_size == 2 && dst_el_size == 4) {
            rasm_add_comment(r, "u16 -> u32");
            LOOP_MASK(p, i) i_uxtl2(r, vh[i].s4,    vl[i].h8);
            LOOP_MASK(p, i) i_uxtl (r, vl[i].s4,    vl[i].h4);
            src_el_size = 4;
        } else if (src_el_size == 2 && dst_el_size == 1) {
            rasm_add_comment(r, "u16 -> u8");
            LOOP_MASK(p, i) i_xtn  (r, vl[i].b8,    vl[i].h8);
            src_el_size = 1;
        }
    } else /* if (p->block_size == 16) */ {
        if (src_el_size == 1 && dst_el_size == 2) {
            rasm_add_comment(r, "u8 -> u16");
            LOOP_MASK(p, i) i_uxtl2(r, vh[i].h8,    vl[i].b16);
            LOOP_MASK(p, i) i_uxtl (r, vl[i].h8,    vl[i].b8);
        } else if (src_el_size == 2 && dst_el_size == 1) {
            rasm_add_comment(r, "u16 -> u8");
            LOOP_MASK(p, i) i_xtn  (r, vl[i].b8,    vl[i].h8);
            LOOP_MASK(p, i) i_xtn  (r, vh[i].b8,    vh[i].h8);
            LOOP_MASK(p, i) i_ins  (r, vl[i].de[1], vh[i].de[0]);
        }
    }

    /* See comment above for high vector bank usage for u32. */
    if (to_type == SWS_PIXEL_F32) {
        rasm_add_comment(r, "u32 -> f32");
        LOOP_MASK(p, i) i_ucvtf(r, vl[i].s4, vl[i].s4);
        LOOP_MASK(p, i) i_ucvtf(r, vh[i].s4, vh[i].s4);
    }
}

/*********************************************************************/
/* expand integers to the full range */
/* SWS_UOP_EXPAND_PAIR */
/* SWS_UOP_EXPAND_QUAD */

static void asmgen_op_expand(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    RasmContext *r = s->rctx;
    RasmOp *vl = s->vl;
    RasmOp *vh = s->vh;

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
        reshape_all_vectors(s, 16, 1);
        LOOP_MASK_VH(s, p, i) i_zip2(r, vh[i], vl[i], vl[i]);
        LOOP_MASK      (p, i) i_zip1(r, vl[i], vl[i], vl[i]);
    }
    if (dst_el_size == 4) {
        rasm_add_comment(r, "u16 -> u32");
        reshape_all_vectors(s, 8, 2);
        LOOP_MASK_VH(s, p, i) i_zip2(r, vh[i], vl[i], vl[i]);
        LOOP_MASK      (p, i) i_zip1(r, vl[i], vl[i], vl[i]);
    }
}

/*********************************************************************/
/* numeric minimum */
/* SWS_UOP_MIN */

static void asmgen_op_min(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                          const SwsAArch64OpRegs *regs)
{
    RasmContext *r = s->rctx;
    RasmOp *vl = s->vl;
    RasmOp *vh = s->vh;
    RasmOp data_vec = regs->min.data_vec;

    LOOP_MASK(p, i) {
        i_dup(r, s->vt[0], a64op_elem(data_vec, i));
        if (p->type == SWS_PIXEL_F32) {
            i_fmin(r, vl[i], vl[i], s->vt[0]); CMTF("vl[%u] = min(vl[%u], vmin%u);", i, i, i);
            if (s->use_vh)
                i_fmin(r, vh[i], vh[i], s->vt[0]);
        } else {
            i_umin(r, vl[i], vl[i], s->vt[0]); CMTF("vl[%u] = min(vl[%u], vmin%u);", i, i, i);
            if (s->use_vh)
                i_umin(r, vh[i], vh[i], s->vt[0]);
        }
    }
}

/*********************************************************************/
/* numeric maximum */
/* SWS_UOP_MAX */

static void asmgen_op_max(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                          const SwsAArch64OpRegs *regs)
{
    RasmContext *r = s->rctx;
    RasmOp *vl = s->vl;
    RasmOp *vh = s->vh;
    RasmOp data_vec = regs->max.data_vec;

    LOOP_MASK(p, i) {
        i_dup(r, s->vt[0], a64op_elem(data_vec, i));
        if (p->type == SWS_PIXEL_F32) {
            i_fmax(r, vl[i], vl[i], s->vt[0]); CMTF("vl[%u] = max(vl[%u], vmax%u);", i, i, i);
            if (s->use_vh)
                i_fmax(r, vh[i], vh[i], s->vt[0]);
        } else {
            i_umax(r, vl[i], vl[i], s->vt[0]); CMTF("vl[%u] = max(vl[%u], vmax%u);", i, i, i);
            if (s->use_vh)
                i_umax(r, vh[i], vh[i], s->vt[0]);
        }
    }
}

/*********************************************************************/
/* multiplication by scalar */
/* SWS_UOP_SCALE */

static void asmgen_op_scale(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                            const SwsAArch64OpRegs *regs)
{
    RasmContext *r = s->rctx;
    RasmOp *vl = s->vl;
    RasmOp *vh = s->vh;
    RasmOp scale_vec = a64op_make_vec(a64op_vec_n(regs->scale.vec), s->el_count, s->el_size);

    if (p->type == SWS_PIXEL_F32) {
        LOOP_MASK      (p, i) { i_fmul(r, vl[i], vl[i], scale_vec); CMTF("vl[%u] *= scale_vec;", i); }
        LOOP_MASK_VH(s, p, i) { i_fmul(r, vh[i], vh[i], scale_vec); CMTF("vh[%u] *= scale_vec;", i); }
    } else {
        LOOP_MASK      (p, i) { i_mul (r, vl[i], vl[i], scale_vec); CMTF("vl[%u] *= scale_vec;", i); }
        LOOP_MASK_VH(s, p, i) { i_mul (r, vh[i], vh[i], scale_vec); CMTF("vh[%u] *= scale_vec;", i); }
    }
}

/*********************************************************************/
/* generalized linear affine transform */
/* SWS_UOP_LINEAR */
/* SWS_UOP_LINEAR_FMA */

/**
 * Performs one pass of the linear transform over a single vector bank
 * (low or high).
 */
static void linear_pass(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                        RasmOp *vt, RasmOp *vc,
                        SwsCompMask save_mask, bool vh_pass)
{
    RasmContext *r = s->rctx;
    /**
     * The intermediate registers for fmul+fadd (for when SWS_BITEXACT
     * is set) start from temp vector 4.
     */
    RasmOp *vtmp = &vt[4];
    RasmOp *vx = vh_pass ? s->vh : s->vl;
    char cvh = vh_pass ? 'h' : 'l';

    if (vh_pass && !s->use_vh)
        return;

    /**
     * Save rows that need to be used as input after they have been already
     * written to.
     */
    RasmOp src_vx[4] = { vx[0], vx[1], vx[2], vx[3] };
    if (save_mask) {
        for (int i = 0; i < 4; i++) {
            if (save_mask & SWS_COMP(i)) {
                src_vx[i] = vt[i];
                i_mov16b(r, vt[i], vx[i]);  CMTF("vsrc[%u] = v%c[%u];", i, cvh, i);
            }
        }
    }

    /**
     * The non-zero coefficients have been packed in aarch64_jit_setup_linear()
     * in sequential order into the individual lanes of the coefficient
     * vector registers. We must follow the same order of execution here.
     */
    int i_coeff = 0;
    LOOP_MASK(p, i) {
        bool first = true;
        RasmNode *pre_mul = rasm_get_current_node(r);
        for (int j = 0; j < 5; j++) {
            bool is_offset = (j == 0);
            int src_j = is_offset ? 4 : (j - 1);
            if (p->par.lin.zero & SWS_MASK(i, src_j))
                continue;
            RasmOp vsrc = src_vx[src_j];
            uint8_t vc_i = i_coeff / 4;
            uint8_t vc_j = i_coeff & 3;
            RasmOp vcoeff = a64op_elem(vc[vc_i], vc_j);
            i_coeff++;
            if (first && is_offset) {
                i_dup (r, vx[i], vcoeff);               CMTF("v%c[%u]  = broadcast(vc[%u][%u]);", cvh, i, vc_i, vc_j);
            } else if (first && !is_offset) {
                if (p->par.lin.one & SWS_MASK(i, src_j)) {
                    i_mov16b(r, vx[i], vsrc);           CMTF("v%c[%u]  = vsrc[%u];", cvh, i, src_j);
                } else {
                    i_fmul  (r, vx[i], vsrc, vcoeff);   CMTF("v%c[%u]  = vsrc[%u] * vc[%u][%u];", cvh, i, src_j, vc_i, vc_j);
                }
            } else if (p->uop == SWS_UOP_LINEAR_FMA) {
                /**
                 * Most modern aarch64 cores have a fastpath for sequences
                 * of fmla instructions. This means that even if the coefficient
                 * is 1, it is still faster to use fmla by 1 instead of fadd.
                 */
                i_fmla(r, vx[i], vsrc, vcoeff);         CMTF("v%c[%u] += vsrc[%u] * vc[%u][%u];", cvh, i, src_j, vc_i, vc_j);
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
                    i_fmul(r, vtmp[vc_j], vsrc, vcoeff);    CMTF("vtmp[%u] = vsrc[%u] * vc[%u][%u];", vc_j, src_j, vc_i, vc_j);
                    pre_mul = rasm_set_current_node(r, pre_mul);
                    i_fadd(r, vx[i], vx[i], vtmp[vc_j]);    CMTF("v%c[%u] += vtmp[%u];", cvh, i, vc_j);
                } else {
                    i_fadd(r, vx[i], vx[i], vsrc);          CMTF("v%c[%u] += vsrc[%u];", cvh, i, vc_j);
                }
            }
            first = false;
        }
    }
}

static void asmgen_op_linear(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                             const SwsAArch64OpRegs *regs)
{
    RasmOp *vt = s->vt;

    /* Compute mask for rows that must be saved before being overwritten. */
    SwsCompMask save_mask = 0;
    bool overwritten[4] = { false, false, false, false };
    LOOP_MASK(p, i) {
        for (int j = 0; j < 5; j++) {
            bool is_offset = (j == 0);
            int src_j = is_offset ? 4 : (j - 1);
            if (p->par.lin.zero & SWS_MASK(i, src_j))
                continue;
            if (!is_offset && overwritten[src_j])
                save_mask |= SWS_COMP(src_j);
            overwritten[i] = true;
        }
    }

    /* Perform linear passes for low and high vector banks. */
    linear_pass(s, p, vt, regs->linear.coeff, save_mask, false);
    linear_pass(s, p, vt, regs->linear.coeff, save_mask, true);
}

/*********************************************************************/
/* add dithering noise */
/* SWS_UOP_DITHER */

static void asmgen_op_dither(SwsAArch64Context *s, const SwsAArch64OpImplParams *p)
{
    RasmContext *r = s->rctx;
    RasmOp *vl = s->vl;
    RasmOp *vh = s->vh;
    RasmOp ptr = s->tmp0;
    RasmOp tmp1 = s->tmp1;
    RasmOp wtmp1 = a64op_w(tmp1);
    RasmOp dither_vl = s->vt[0];
    RasmOp dither_vh = s->vt[1];
    RasmOp bx64 = a64op_x(s->bx);
    RasmOp y64 = a64op_x(s->y);

    /**
     * For a description of the matrix buffer layout, read the comments
     * in aarch64_setup_dither() in aarch64/ops.c.
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

    i_ldr(r, ptr, a64op_off(s->impl, offsetof_impl_priv));  CMT("void *ptr = impl->priv.ptr;");

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
        i_add  (r, ptr,  ptr,  tmp1);       CMT("ptr += tmp1;");
    }

    int last_y_off = -1;
    int prev_i = 0;
    for (int sorted_i = 0; sorted_i < n_comps; sorted_i++) {
        int i = sorted[sorted_i];
        uint8_t y_off = p->par.dither.y_offset[i];
        bool do_load = (y_off != last_y_off);

        if (last_y_off < 0) {
            /* On the first run, calculate pointer inside dither_matrix. */
            RasmOp lsb   = IMM(dither_size_log2 + sizeof_float_log2);
            RasmOp width = IMM(dither_size_log2);
            /**
             * The ubfiz instruction for the y offset performs masking
             * by the dither matrix size and shifts by the stride.
             */
            if (y_off == 0) {
                i_ubfiz(r, tmp1,  y64,  lsb, width);        CMT("tmp1 = (y & (dither_size - 1)) * dither_size * sizeof(float);");
            } else {
                i_add  (r, wtmp1, s->y, IMM(y_off));        CMTF("tmp1 = y + y_off[%u];", i);
                i_ubfiz(r, tmp1,  tmp1, lsb, width);        CMT("tmp1 = (tmp1 & (dither_size - 1)) * dither_size * sizeof(float);");
            }
            i_add(r, ptr, ptr, tmp1);                       CMT("ptr += tmp1;");
        } else if (do_load) {
            /**
             * On subsequent runs, just increment the pointer.
             * The matrix is over-allocated, so we don't risk
             * overreading.
             */
            int delta = (y_off - last_y_off) * (1 << dither_size_log2) * sizeof(float);
            i_add(r, ptr, ptr, IMM(delta));                 CMTF("ptr += (y_off[%u] - y_off[%u]) * dither_size * sizeof(float);", i, prev_i);
        }

        if (do_load) {
            RasmOp dither_vlq = v_q(dither_vl);
            RasmOp dither_vhq = v_q(dither_vh);
            i_ldp (r, dither_vlq, dither_vhq, a64op_base(ptr)); CMT("{ ditherl, ditherh } = *ptr;");
        }

        i_fadd    (r, vl[i], vl[i], dither_vl);             CMTF("vl[%u] += vditherl;", i);
        if (s->use_vh) {
            i_fadd(r, vh[i], vh[i], dither_vh);             CMTF("vh[%u] += vditherh;", i);
        }

        last_y_off = y_off;
        prev_i = i;
    }
}

/*********************************************************************/
static const char op_type_names[SWS_UOP_TYPE_NB][16] = {
    [SWS_UOP_READ_BIT      ] = "read_bit",
    [SWS_UOP_READ_NIBBLE   ] = "read_nibble",
    [SWS_UOP_READ_PACKED   ] = "read_packed",
    [SWS_UOP_READ_PLANAR   ] = "read_planar",
    [SWS_UOP_WRITE_BIT     ] = "write_bit",
    [SWS_UOP_WRITE_NIBBLE  ] = "write_nibble",
    [SWS_UOP_WRITE_PACKED  ] = "write_packed",
    [SWS_UOP_WRITE_PLANAR  ] = "write_planar",
    [SWS_UOP_SWAP_BYTES    ] = "swap_bytes",
    [SWS_UOP_MOVE          ] = "move",
    [SWS_UOP_UNPACK        ] = "unpack",
    [SWS_UOP_PACK          ] = "pack",
    [SWS_UOP_LSHIFT        ] = "lshift",
    [SWS_UOP_RSHIFT        ] = "rshift",
    [SWS_UOP_CLEAR         ] = "clear",
    [SWS_UOP_TO_U8         ] = "to_u8",
    [SWS_UOP_TO_U16        ] = "to_u16",
    [SWS_UOP_TO_U32        ] = "to_u32",
    [SWS_UOP_TO_F32        ] = "to_f32",
    [SWS_UOP_EXPAND_PAIR   ] = "expand_pair",
    [SWS_UOP_EXPAND_QUAD   ] = "expand_quad",
    [SWS_UOP_MIN           ] = "min",
    [SWS_UOP_MAX           ] = "max",
    [SWS_UOP_SCALE         ] = "scale",
    [SWS_UOP_LINEAR        ] = "linear",
    [SWS_UOP_LINEAR_FMA    ] = "linear_fma",
    [SWS_UOP_DITHER        ] = "dither",
};

static int aarch64_jit_uop(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                           const SwsAArch64OpRegs *regs)
{
    RasmContext *r = s->rctx;

    /**
     * Set up vector register dimensions and reshape all vectors
     * accordingly.
     */
    size_t el_size = ff_sws_pixel_type_size(p->type);
    size_t total_size = p->block_size * el_size;

    s->vec_size = FFMIN(total_size, 16);
    s->use_vh = (s->vec_size != total_size);

    s->el_size = el_size;
    s->el_count = s->vec_size / el_size;
    reshape_all_vectors(s, s->el_count, el_size);

    rasm_add_commentf(r, (char[128]){0}, 128, "=> %s", op_type_names[p->uop]);

    switch (p->uop) {
    case SWS_UOP_READ_BIT:     asmgen_op_read_bit(s, p, regs);     break;
    case SWS_UOP_READ_NIBBLE:  asmgen_op_read_nibble(s, p, regs);  break;
    case SWS_UOP_READ_PACKED:  asmgen_op_read_packed(s, p);        break;
    case SWS_UOP_READ_PLANAR:  asmgen_op_read_planar(s, p);        break;
    case SWS_UOP_WRITE_BIT:    asmgen_op_write_bit(s, p, regs);    break;
    case SWS_UOP_WRITE_NIBBLE: asmgen_op_write_nibble(s, p);       break;
    case SWS_UOP_WRITE_PACKED: asmgen_op_write_packed(s, p);       break;
    case SWS_UOP_WRITE_PLANAR: asmgen_op_write_planar(s, p);       break;
    case SWS_UOP_SWAP_BYTES:   asmgen_op_swap_bytes(s, p);         break;
    case SWS_UOP_MOVE:         asmgen_op_move(s, p);               break;
    case SWS_UOP_UNPACK:       asmgen_op_unpack(s, p, regs);       break;
    case SWS_UOP_PACK:         asmgen_op_pack(s, p);               break;
    case SWS_UOP_LSHIFT:       asmgen_op_lshift(s, p);             break;
    case SWS_UOP_RSHIFT:       asmgen_op_rshift(s, p);             break;
    case SWS_UOP_CLEAR:        asmgen_op_clear(s, p, regs);        break;
    case SWS_UOP_TO_U8:        asmgen_op_convert(s, p);            break;
    case SWS_UOP_TO_U16:       asmgen_op_convert(s, p);            break;
    case SWS_UOP_TO_U32:       asmgen_op_convert(s, p);            break;
    case SWS_UOP_TO_F32:       asmgen_op_convert(s, p);            break;
    case SWS_UOP_EXPAND_PAIR:  asmgen_op_expand(s, p);             break;
    case SWS_UOP_EXPAND_QUAD:  asmgen_op_expand(s, p);             break;
    case SWS_UOP_MIN:          asmgen_op_min(s, p, regs);          break;
    case SWS_UOP_MAX:          asmgen_op_max(s, p, regs);          break;
    case SWS_UOP_SCALE:        asmgen_op_scale(s, p, regs);        break;
    case SWS_UOP_LINEAR:       asmgen_op_linear(s, p, regs);       break;
    case SWS_UOP_LINEAR_FMA:   asmgen_op_linear(s, p, regs);       break;
    case SWS_UOP_DITHER:       asmgen_op_dither(s, p);             break;
    /* TODO implement SWS_UOP_SHUFFLE */
    default:
        break;
    }

    return 0;
}

int ff_sws_jit_assemble_llvm(const char *asm_src, uint8_t **out_text, size_t *out_size);

/*********************************************************************/
/* Unified setup pass: collect all immediates and data pool entries
 * needed by one op, and fill in the pre-allocated register assignments
 * in *regs.  Called before aarch64_jit_process() so that
 * load_constants() can pre-load all constants before the inner loop. */
static int aarch64_setup(SwsAArch64Context *s, const SwsOpList *ops, int n,
                         const SwsAArch64OpImplParams *p, SwsAArch64OpRegs *regs)
{
    SwsImplResult impl_result = { 0 };

    switch (p->uop) {
    case SWS_UOP_READ_BIT: {
        int bitmask_idx = jit_push_imm8(s, 1, 1);
        regs->read_bit.bitmask = s->vimm[bitmask_idx];
#if 0
        int ret = aarch64_jit_setup(ops, s->block_size, n, p, &impl_result);
        if (ret < 0)
            return ret;
#else
        /* Negative shift values to perform right shift using ushl. */
        if (op->rw.frac == 3) {
            out->priv = (SwsOpPriv) {
                .u8 = {
                    -7, -6, -5, -4, -3, -2, -1, 0,
                    -7, -6, -5, -4, -3, -2, -1, 0,
                }
            };
        }
#endif
        int idx = jit_push_data(s, impl_result.priv.u32);
        regs->read_bit.shift_vec = s->vdata[idx];
        break;
    }
    case SWS_UOP_READ_NIBBLE: {
        int nibble_idx = jit_push_imm8(s, 0x0f, 1);
        regs->read_nibble.nibble_mask = s->vimm[nibble_idx];
        break;
    }
    case SWS_UOP_WRITE_BIT: {
#if 0
        int ret = aarch64_jit_setup(ops, s->block_size, n, p, &impl_result);
        if (ret < 0)
            return ret;
#else
        /* Shift values for ushl. */
        if (op->rw.frac == 3) {
            out->priv = (SwsOpPriv) {
                .u8 = {
                    7, 6, 5, 4, 3, 2, 1, 0,
                    7, 6, 5, 4, 3, 2, 1, 0,
                }
            };
        }
#endif
        int idx = jit_push_data(s, impl_result.priv.u32);
        regs->write_bit.shift_vec = s->vdata[idx];
        break;
    }
    case SWS_UOP_UNPACK:
        LOOP_MASK(p, i) {
            uint32_t val = (1u << p->par.pack.pattern[i]) - 1;
            int idx = jit_push_imm32_op(s, p->type, val);
            regs->unpack.mask[i] = s->vimm[idx];
        }
        break;
    case SWS_UOP_CLEAR: {
        bool need_data = false;
        LOOP_MASK(p, i) {
            if (!SWS_COMP_TEST(p->par.clear.zero, i) && !SWS_COMP_TEST(p->par.clear.one, i))
                need_data = true;
        }
        if (need_data) {
            int ret = aarch64_jit_setup(ops, s->block_size, n, p, &impl_result);
            if (ret < 0)
                return ret;
            int idx = jit_push_data(s, impl_result.priv.u32);
            int el_size = ff_sws_pixel_type_size(p->type);
            regs->clear.data_vec = a64op_make_vec(SWS_AARCH64_REGID_VDATA + idx,
                                                  16 / el_size, el_size);
        }
        break;
    }
    case SWS_UOP_MIN: {
        int ret = aarch64_jit_setup(ops, s->block_size, n, p, &impl_result);
        if (ret < 0)
            return ret;
        int idx = jit_push_data(s, impl_result.priv.u32);
        int el_size = ff_sws_pixel_type_size(p->type);
        regs->min.data_vec = a64op_make_vec(SWS_AARCH64_REGID_VDATA + idx,
                                            16 / el_size, el_size);
        break;
    }
    case SWS_UOP_MAX: {
        int ret = aarch64_jit_setup(ops, s->block_size, n, p, &impl_result);
        if (ret < 0)
            return ret;
        int idx = jit_push_data(s, impl_result.priv.u32);
        int el_size = ff_sws_pixel_type_size(p->type);
        regs->max.data_vec = a64op_make_vec(SWS_AARCH64_REGID_VDATA + idx,
                                            16 / el_size, el_size);
        break;
    }
    case SWS_UOP_SCALE: {
        int ret = aarch64_jit_setup(ops, s->block_size, n, p, &impl_result);
        if (ret < 0)
            return ret;
        int len = ff_sws_pixel_type_size(p->type);
        int idx = jit_push_imm32(s, impl_result.priv.u32[0], len);
        regs->scale.vec = s->vimm[idx];
        break;
    }
    case SWS_UOP_LINEAR:
    case SWS_UOP_LINEAR_FMA: {
        int ret = aarch64_jit_setup(ops, s->block_size, n, p, &impl_result);
        if (ret < 0)
            return ret;
        const int num_vregs = linear_num_vregs(p);
        av_assert0(num_vregs <= 4);
        regs->linear.num_vregs = num_vregs;
        const float *coeffs = (const float *) impl_result.priv.ptr;
        for (int vi = 0; vi < num_vregs; vi++) {
            const uint32_t *words = (const uint32_t *) &coeffs[vi * 4];
            int idx = jit_push_data(s, words);
            regs->linear.coeff[vi] = a64op_vec4s(SWS_AARCH64_REGID_VDATA + idx);
        }
        impl_result.free(&impl_result.priv);
        break;
    }
    default:
        break;
    }
    return 0;
}

/*********************************************************************/
static int aarch64_jit_compile(SwsContext *ctx, const SwsOpList *ops,
                               SwsCompiledOp *out)
{
    int ret;

    const int cpu_flags = av_get_cpu_flags();
    if (!(cpu_flags & AV_CPU_FLAG_NEON))
        return AVERROR(ENOTSUP);

    const int block_size = (ff_sws_op_list_max_size(ops) == 4) ? 8 : 16;

    RasmContext *r = rasm_alloc();
    if (!r)
        return AVERROR(ENOMEM);

    SwsAArch64Context s = {
        .sws        = ctx,
        .block_size = block_size,
        .rctx       = r,
    };

    /* Translate all ops into implementation parameters and setup all
     * constant data. */
    SwsAArch64OpImplParams params[SWS_MAX_OPS] = { 0 };
    SwsAArch64OpRegs regs[SWS_MAX_OPS] = { 0 };
    for (int i = 0; i < ops->num_ops; i++) {
        ret = ff_sws_aarch64_ops_translate(ctx, ops, i, block_size, &params[i]);
        if (ret < 0)
            goto error;
        ret = aarch64_setup(&s, ops, i, &params[i], &regs[i]);
        if (ret < 0)
            goto error;
    }

#if 1
    /* The Platform Register (r18) is not used. */
    jit_gpr(&s, 18);

    /**
     * The entry point of the SwsOpFunc is the `process` function. The
     * first kernel function is called from `process`, and subsequent
     * kernel functions are chained by directly branching to the next
     * operation, using a continuation-passing style design. The last
     * operation must be a write operation, which returns from the call
     * to the `process` function.
     *
     * The GPRs used by the entire call-chain are listed below.
     *
     * Function arguments are passed in r0-r5. After the parameters
     * from `exec` have been read, r0 is reused to branch to the
     * continuation functions. After the original parameters from
     * `impl` have been computed, r1 is reused as the `impl` pointer
     * for each operation.
     *
     * Loop iterators are r6 for `bx` and r3 for `y`, reused from
     * `y_start`, which doesn't need to be preserved.
     *
     * The intra-procedure-call temporary registers (r16 and r17) are
     * used as scratch registers. They may be used by call veneers and
     * PLT code inserted by the linker, so we cannot expect them to
     * persist across branches between functions.
     *
     * The Platform Register (r18) is not used.
     *
     * The read/write data pointers and padding values first use up the
     * remaining free caller-saved registers, and only then are the
     * caller-saved registers (r19-r28) used.
     */
#endif

    /* create process */
    ret = aarch64_jit_process(&s, &params[0], &params[ops->num_ops - 1]);
    if (ret < 0)
        goto error;

    /* emit data pool words after the ret instruction */
    if (s.n_data > 0) {
        rasm_set_current_node(r, s.const_data);
        rasm_add_directive(r, ".align 16");
        rasm_add_label(r, s.data_label);
        char buf[32];
        for (int i = 0; i < s.n_data * 4; i++) {
            snprintf(buf, sizeof(buf), ".word 0x%08x", s.data[i]);
            rasm_add_directive(r, buf);
        }
    }

    /* add all ops */
    rasm_set_current_node(r, s.loop);
    for (int i = 0; i < ops->num_ops; i++) {
        ret = aarch64_jit_uop(&s, &params[i], &regs[i]);
        if (ret < 0)
            goto error;
    }

    /* Function prologue */
    RasmOp saved_regs[MAX_SAVED_REGS];
    unsigned nsaved = 0;
    for (int i = 19; i <= 30; i++) {
        if (s.gprs.clobbered & (1 << i))
            saved_regs[nsaved++] = a64op_gpx(i);
    }
    if (nsaved) {
        rasm_set_current_node(r, s.prologue);
        asmgen_prologue(&s, saved_regs, nsaved);
        rasm_set_current_node(r, s.epilogue);
        asmgen_epilogue(&s, saved_regs, nsaved);
    }

    *out = (SwsCompiledOp) {
        .priv        = &s,
        .slice_align = 1,
        // .free        = ff_sws_op_chain_free_cb,
        .block_size  = block_size,
        .func        = /*process_func*/ NULL,
        .cpu_flags   = cpu_flags,
    };

    printf("gprs.used %08x\n", s.gprs.used);
    printf("[%s][%d] %s() %d\n", __FILE__, __LINE__, __func__, SWS_MAX_OPS);
    AVBPrint bp;
    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
    rasm_print(s.rctx, &bp, true);

    uint8_t *text;
    size_t text_size;
    ret = ff_sws_jit_assemble_llvm(bp.str, &text, &text_size);
    if (ret < 0) {
        printf("[%s][%d] %s() ret %d\n", __FILE__, __LINE__, __func__, ret);
    }

    fputs(bp.str, stdout);
    av_bprint_finalize(&bp, NULL);

    out->func = (SwsOpFunc) text;

error:
    if (ret < 0) {
        rasm_free(&s.rctx);
    }
    return ret;
}

/*********************************************************************/
const SwsOpBackend backend_aarch64_jit = {
    .name      = "aarch64_jit",
    .flags     = SWS_BACKEND_AARCH64_JIT,
    .compile   = aarch64_jit_compile,
    .hw_format = AV_PIX_FMT_NONE,
};
