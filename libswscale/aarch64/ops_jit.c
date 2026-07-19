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
#include "ops.h"

/*********************************************************************/
/* TODO no */
#include "ops_asmgen.c"

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

static bool vecs_are_contiguous(RasmOp *ops)
{
    for (int i = 0; i < 4; i++) {
        if (rasm_op_type(ops[i]) == RASM_OP_NONE)
            break;
        if (i && (a64op_vec_n(ops[i]) & 0x1f) != ((a64op_vec_n(ops[i - 1]) + 1) & 0x1f))
            return false;
    }
    return true;
}

static int asmgen_op_jit(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                         SwsAArch64OpRegs *regs)
{
    RasmContext *r = s->rctx;
    AArch64RegState *rs = &s->regstate;

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
    reshape_io_vectors(regs, s->el_count, el_size);
    reshape_temp_vectors(regs, s->el_count, el_size);
    reshape_const_vectors(regs, s->el_count, el_size);

    rasm_add_commentf(r, (char[128]){0}, 128, "=> %s", op_type_names[p->uop]);

    switch (p->uop) {
    case SWS_UOP_READ_BIT:     asmgen_op_read_bit(s, p, regs);     break;
    case SWS_UOP_READ_NIBBLE:  asmgen_op_read_nibble(s, p, regs);  break;
    case SWS_UOP_READ_PACKED:  asmgen_op_read_packed(s, p, regs);  break;
    case SWS_UOP_READ_PLANAR:  asmgen_op_read_planar(s, p, regs);  break;
    case SWS_UOP_WRITE_BIT:    asmgen_op_write_bit(s, p, regs);    break;
    case SWS_UOP_WRITE_NIBBLE: asmgen_op_write_nibble(s, p, regs); break;
    case SWS_UOP_WRITE_PACKED: {
        /* Count number of elems. */
        int n = 0;
        LOOP_MASK(p, i)
            n++;

        if (!vecs_are_contiguous(regs->sl)) {
            RasmOp sl[4] = { 0 };
            a64reg_contiguous_vec(rs, n, sl);
            LOOP_MASK(p, i) {
                sl[i] = a64op_make_vec(a64op_vec_n(sl[i]), s->el_count, s->el_size);
                i_mov(r, sl[i], regs->sl[i]);
                a64reg_vec_free(rs, regs->sl[i]);
                regs->sl[i] = sl[i];
            }
        }
        if (s->use_vh && !vecs_are_contiguous(regs->sh)) {
            RasmOp sh[4] = { 0 };
            a64reg_contiguous_vec(rs, n, sh);
            LOOP_MASK(p, i) {
                sh[i] = a64op_make_vec(a64op_vec_n(sh[i]), s->el_count, s->el_size);
                i_mov(r, sh[i], regs->sh[i]);
                a64reg_vec_free(rs, regs->sh[i]);
                regs->sh[i] = sh[i];
            }
        }

        asmgen_op_write_packed(s, p, regs);
        break;
    }
    case SWS_UOP_WRITE_PLANAR: asmgen_op_write_planar(s, p, regs); break;
    case SWS_UOP_SWAP_BYTES:   asmgen_op_swap_bytes(s, p, regs);   break;
    case SWS_UOP_UNPACK:       asmgen_op_unpack(s, p, regs);       break;
    case SWS_UOP_PACK:         asmgen_op_pack(s, p, regs);         break;
    case SWS_UOP_LSHIFT:       asmgen_op_lshift(s, p, regs);       break;
    case SWS_UOP_RSHIFT:       asmgen_op_rshift(s, p, regs);       break;
    case SWS_UOP_CLEAR:        asmgen_op_clear(s, p, regs);        break;
    case SWS_UOP_TO_U8:        asmgen_op_convert(s, p, regs);      break;
    case SWS_UOP_TO_U16:       asmgen_op_convert(s, p, regs);      break;
    case SWS_UOP_TO_U32:       asmgen_op_convert(s, p, regs);      break;
    case SWS_UOP_TO_F32:       asmgen_op_convert(s, p, regs);      break;
    case SWS_UOP_EXPAND_PAIR:  asmgen_op_expand(s, p, regs);       break;
    case SWS_UOP_EXPAND_QUAD:  asmgen_op_expand(s, p, regs);       break;
    case SWS_UOP_MIN:          asmgen_op_min(s, p, regs);          break;
    case SWS_UOP_MAX:          asmgen_op_max(s, p, regs);          break;
    case SWS_UOP_SCALE:        asmgen_op_scale(s, p, regs);        break;
    case SWS_UOP_LINEAR:       asmgen_op_linear(s, p, regs);       break;
    case SWS_UOP_LINEAR_FMA:   asmgen_op_linear(s, p, regs);       break;
    case SWS_UOP_DITHER:       asmgen_op_dither(s, p, regs);       break;
    /* TODO implement SWS_UOP_SHUFFLE */
    default:
        break;
    }

    return 0;
}

int ff_sws_jit_assemble_llvm(const char *asm_src, uint8_t **out_text, size_t *out_size);

/*********************************************************************/
/* Returns a RasmOp with the entire 128-bit sequence. */
static RasmOp jit_push_v128(SwsAArch64Context *s, void *val)
{
    /* Check if we already have it. */
    for (int i = 0; i < s->data_count; i++) {
        if (rasm_op_type(s->data[i].op) != AARCH64_OP_VEC)
            continue;
        if (s->data[i].op_idx == 4 && !memcmp(&s->data[i].vec, val, 16)) {
            return s->data[i].op;
        }
    }

    /* Add it to our data and create a new vector. */
    int idx = s->data_count++;
    memcpy(&s->data[idx].vec, val, 16);
    s->data[idx].op     = v_q(a64reg_unclobbered_vec(&s->regstate));
    s->data[idx].op_idx = 4;

    return s->data[idx].op;
}

/* Returns a RasmOp with the u64. */
static RasmOp jit_push_u64(SwsAArch64Context *s, uint64_t val)
{
    /* Add it to our data and create a new vector. */
    int idx = s->data_count++;
    s->data[idx].vec.u64[0] = val;
    s->data[idx].op     = a64reg_unclobbered_gpx(&s->regstate);
    s->data[idx].op_idx = 2; // TODO
    return s->data[idx].op;
}

/* Returns a RasmOp with the value broadcast to all elements. */
static RasmOp jit_push_vimm(SwsAArch64Context *s, SwsPixelType type, uint32_t val)
{
    /* Expand to u32. */
    switch (type) {
    case SWS_PIXEL_U8:  val = val | (val <<  8); av_fallthrough;
    case SWS_PIXEL_U16: val = val | (val << 16); break;
    }

    /* TODO use movi for movi-encodable immediates. */
    /* TODO use mov+dup. */

    SwsAArch64Vector vec = { .u32 = { val, val, val, val } };
    return jit_push_v128(s, &vec);
}

static RasmOp jit_push_elem(SwsAArch64Context *s, SwsPixelType type, uint32_t val)
{
    /* Check if we already have it in data. */
    for (int i = 0; i < s->data_count; i++) {
        if (rasm_op_type(s->data[i].op) != AARCH64_OP_VEC)
            continue;
        for (int j = 0; j < s->data[i].op_idx; j++) {
            if (s->data[i].vec.u32[j] == val)
                return a64op_elem(v_4s(s->data[i].op), j);
        }
    }

    /* Look for a hole. */
    for (int i = 0; i < s->data_count; i++) {
        if (rasm_op_type(s->data[i].op) != AARCH64_OP_VEC)
            continue;
        if (s->data[i].op_idx != 4) {
            int jdx = s->data[i].op_idx++;
            s->data[i].vec.u32[jdx] = val;
            return a64op_elem(v_4s(s->data[i].op), jdx);
        }
    }

    /* Add it to our data and create a new vector. */
    int idx = s->data_count++;
    s->data[idx].vec.u32[0] = val;
    s->data[idx].op     = v_q(a64reg_unclobbered_vec(&s->regstate));
    s->data[idx].op_idx = 1;

    return a64op_elem(v_4s(s->data[idx].op), 0);
}

static void load_constants(SwsAArch64Context *s)
{
    RasmContext *r = s->rctx;
    AArch64RegState *rs = &s->regstate;

    /* Emit data. */
    int ldata = rasm_const_begin(r, "ldata");
    for (int i = 0; i < s->data_count; i++)
        rasm_add_data(r, &s->data[i].vec, 4, RASM_DATA_WORD);

    /* Load data. */
    RasmNode *saved = rasm_set_current_node(r, s->setup);
    rasm_add_comment(r, "load constants");
    RasmOp ptr = a64reg_gpx(rs, -1);
    i_adr(r, ptr, rasm_op_label(ldata));
    for (int i = 0; i < s->data_count; i++) {
        RasmOp base = a64op_off(ptr, i * 16);
        i_ldr(r, s->data[i].op, base);
    }
    a64reg_gpr_free(rs, ptr);
    s->setup = rasm_set_current_node(r, saved);
}

/*********************************************************************/
static const char *print_one_reg(char buf[16], RasmOp op)
{
    switch (rasm_op_type(op)) {
    case AARCH64_OP_GPR:
        snprintf(buf, 16, "x%d", a64op_gpr_n(op));
        break;
    case AARCH64_OP_VEC:
        snprintf(buf, 16, "v%d", a64op_vec_n(op));
        break;
    default:
        snprintf(buf, 16, "__");
        break;
    }
    return buf;
}

static void print_regs(const SwsAArch64OpImplParams *p, const SwsAArch64OpRegs *regs)
{
    printf("[%-16s] { %s, %s, %s, %s } { %s, %s, %s, %s } -> { %s, %s, %s, %s } { %s, %s, %s, %s }\n",
           op_type_names[p->uop],
           print_one_reg((char[16]){0}, regs->sl[0]),
           print_one_reg((char[16]){0}, regs->sl[1]),
           print_one_reg((char[16]){0}, regs->sl[2]),
           print_one_reg((char[16]){0}, regs->sl[3]),
           print_one_reg((char[16]){0}, regs->sh[0]),
           print_one_reg((char[16]){0}, regs->sh[1]),
           print_one_reg((char[16]){0}, regs->sh[2]),
           print_one_reg((char[16]){0}, regs->sh[3]),
           print_one_reg((char[16]){0}, regs->dl[0]),
           print_one_reg((char[16]){0}, regs->dl[1]),
           print_one_reg((char[16]){0}, regs->dl[2]),
           print_one_reg((char[16]){0}, regs->dl[3]),
           print_one_reg((char[16]){0}, regs->dh[0]),
           print_one_reg((char[16]){0}, regs->dh[1]),
           print_one_reg((char[16]){0}, regs->dh[2]),
           print_one_reg((char[16]){0}, regs->dh[3]));
}

static void alloc_scratch_vecs(AArch64RegState *rs, int count, RasmOp *out)
{
    for (int i = 0; i < count; i++)
        out[i] = a64reg_vec(rs, -1);
    for (int i = 0; i < count; i++)
        a64reg_vec_free(rs, out[i]);
}

static uint32_t get_priv(const SwsOpPriv *priv, SwsPixelType type, int i)
{
    return (type == SWS_PIXEL_U8)  ? priv->u8[i]
         : (type == SWS_PIXEL_U16) ? priv->u16[i]
         :                           priv->u32[i];
}

static void aarch64_jit_setup_swizzle(SwsAArch64Context *s, const SwsOp *op,
                                      SwsAArch64OpRegs *regs, int block_size)
{
    AArch64RegState *rs = &s->regstate;

    size_t el_size = ff_sws_pixel_type_size(op->type);
    size_t total_size = block_size * el_size;

    s->vec_size = FFMIN(total_size, 16);
    s->use_vh = (s->vec_size != total_size);

    s->el_size = el_size;
    s->el_count = s->vec_size / el_size;

    RasmOp *sl = regs->sl;
    RasmOp *sh = regs->sh;
    RasmOp *dl = regs->dl;
    RasmOp *dh = regs->dh;
    RasmOp *vt = regs->vt;
    SwsAArch64OpRegs *prev = &regs[-1];

        {
            bool reorder = true;
            bool used[4] = { false, false, false, false };
            for (int i = 0; i < 4; i++) {
                if (!SWS_OP_NEEDED(op, i))
                    continue;
                if (used[op->swizzle.in[i]]) {
                    reorder = false;
                    break;
                }
                used[op->swizzle.in[i]] = true;
            }

            // LOOP_IN(i) save_vector(ctx, &vet, i);
            if (reorder) {
                // cc.comment("swizzle (reorder)");
                // LOOP_OUT(i) vl[i] = src_vl[op->swizzle.in[i]];
                for (int i = 0; i < 4; i++) { if (SWS_OP_NEEDED(op, i)) { sl[op->swizzle.in[i]] = prev->dl[op->swizzle.in[i]]; dl[i] = sl[op->swizzle.in[i]]; } }
                // LOOP_OUT(i) vh[i] = src_vh[op->swizzle.in[i]];
                if (s->use_vh)
                    for (int i = 0; i < 4; i++) { if (SWS_OP_NEEDED(op, i)) { sh[op->swizzle.in[i]] = prev->dh[op->swizzle.in[i]]; dh[i] = sh[op->swizzle.in[i]]; } }
            } else {
                /**
                 * At least one input component is read by more than one
                 * output component (e.g. duplicating a single gray
                 * channel into several output channels) -- pure register
                 * aliasing is not possible since a single physical
                 * register cannot be "renamed" to two different logical
                 * outputs. Slots that keep their own value are aliased
                 * as usual; slots that need someone else's value get a
                 * fresh register, copied by aarch64_jit_op_swizzle().
                 */
                for (int i = 0; i < 4; i++) {
                    if (!SWS_OP_NEEDED(op, i))
                        continue;
                    if (op->swizzle.in[i] == i) {
                        dl[i] = sl[i] = prev->dl[i];
                        if (s->use_vh)
                            dh[i] = sh[i] = prev->dh[i];
                    } else {
                        sl[i] = prev->dl[op->swizzle.in[i]];
                        dl[i] = a64reg_vec(rs, -1);
                        if (s->use_vh) {
                            sh[i] = prev->dh[op->swizzle.in[i]];
                            dh[i] = a64reg_vec(rs, -1);
                        }
                    }
                }
            }
        }
}

static void aarch64_jit_op_swizzle(SwsAArch64Context *s, const SwsOp *op,
                                   SwsAArch64OpRegs *regs, int block_size)
{
    RasmContext *r = s->rctx;

    size_t el_size = ff_sws_pixel_type_size(op->type);
    size_t total_size = block_size * el_size;

    s->vec_size = FFMIN(total_size, 16);
    s->use_vh = (s->vec_size != total_size);

    /**
     * Must match the reorder/copy decision made in
     * aarch64_jit_setup_swizzle(): a pure reorder needs no instructions
     * at all (it was handled entirely via register aliasing), while a
     * copy (some input read by more than one output) needs a real mov
     * for every slot that didn't keep its own value.
     */
    bool reorder = true;
    bool used[4] = { false, false, false, false };
    for (int i = 0; i < 4; i++) {
        if (!SWS_OP_NEEDED(op, i))
            continue;
        if (used[op->swizzle.in[i]]) {
            reorder = false;
            break;
        }
        used[op->swizzle.in[i]] = true;
    }
    if (reorder)
        return;

    for (int i = 0; i < 4; i++) {
        if (SWS_OP_NEEDED(op, i) && op->swizzle.in[i] != i) {
            i_mov16b(r, regs->dl[i], regs->sl[i]);
            if (s->use_vh) {
                i_mov16b(r, regs->dh[i], regs->sh[i]);
            }
        }
    }
}

static int aarch64_jit_setup(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                             SwsImplResult *res, SwsAArch64OpRegs *regs, int n,
                             SwsCompMask imask, SwsCompMask omask)
{
    AArch64RegState *rs = &s->regstate;

    p += n;
    res += n;
    regs += n;

    size_t el_size = ff_sws_pixel_type_size(p->type);
    size_t total_size = p->block_size * el_size;

    s->vec_size = FFMIN(total_size, 16);
    s->use_vh = (s->vec_size != total_size);

    s->el_size = el_size;
    s->el_count = s->vec_size / el_size;

    /* GPRs */
    switch (p->uop) {
    case SWS_UOP_READ_BIT:
    case SWS_UOP_READ_NIBBLE:
    case SWS_UOP_READ_PLANAR:
    case SWS_UOP_READ_PACKED:
        LOOP(imask, i) { s->in     [i] = a64reg_gpx(rs, -1); }
        LOOP(imask, i) { s->in_bump[i] = a64reg_gpx(rs, -1); }
        break;
    case SWS_UOP_WRITE_BIT:
    case SWS_UOP_WRITE_NIBBLE:
    case SWS_UOP_WRITE_PACKED:
    case SWS_UOP_WRITE_PLANAR:
        LOOP(omask, i) { s->out     [i] = a64reg_gpx(rs, -1); }
        LOOP(omask, i) { s->out_bump[i] = a64reg_gpx(rs, -1); }
        break;
    }

    /* I/O */
    RasmOp *sl = regs->sl;
    RasmOp *sh = regs->sh;
    RasmOp *dl = regs->dl;
    RasmOp *dh = regs->dh;
    RasmOp *vt = regs->vt;
    SwsAArch64OpRegs *prev = n ? &regs[-1] : regs;
    switch (p->uop) {
    case SWS_UOP_READ_PLANAR:
        LOOP_MASK      (p, i) { dl[i] = a64reg_vec(rs, -1); }
        LOOP_MASK_VH(s, p, i) { dh[i] = a64reg_vec(rs, -1); }
        break;
    case SWS_UOP_READ_PACKED: {
        /* Count number of elems. */
        int n = 0;
        LOOP_MASK(p, i)
            n++;

        a64reg_contiguous_vec    (rs, n, dl);
        if (s->use_vh)
            a64reg_contiguous_vec(rs, n, dh);
        break;
    }
    case SWS_UOP_READ_NIBBLE:
        LOOP_MASK      (p, i) { dl[i] = a64reg_vec(rs, -1); }
        LOOP_MASK_VH(s, p, i) { dh[i] = a64reg_vec(rs, -1); }
        alloc_scratch_vecs(rs, 1, vt);
        break;
    case SWS_UOP_READ_BIT:
        LOOP_MASK      (p, i) { dl[i] = a64reg_vec(rs, -1); }
        LOOP_MASK_VH(s, p, i) { dh[i] = a64reg_vec(rs, -1); }
        alloc_scratch_vecs(rs, 1, vt);
        break;
    case SWS_UOP_WRITE_PLANAR:
        LOOP_MASK      (p, i) { sl[i] = prev->dl[i]; }
        LOOP_MASK_VH(s, p, i) { sh[i] = prev->dh[i]; }
        break;
    case SWS_UOP_WRITE_PACKED:
        // TODO
        LOOP_MASK      (p, i) { sl[i] = prev->dl[i]; }
        LOOP_MASK_VH(s, p, i) { sh[i] = prev->dh[i]; }
        break;
    case SWS_UOP_WRITE_NIBBLE:
        LOOP_MASK      (p, i) { sl[i] = prev->dl[i]; }
        LOOP_MASK_VH(s, p, i) { sh[i] = prev->dh[i]; }
        alloc_scratch_vecs(rs, 2, vt);
        break;
    case SWS_UOP_WRITE_BIT:
        LOOP_MASK      (p, i) { sl[i] = prev->dl[i]; }
        LOOP_MASK_VH(s, p, i) { sh[i] = prev->dh[i]; }
        alloc_scratch_vecs(rs, 2, vt);
        break;
    case SWS_UOP_SWAP_BYTES:
        LOOP_MASK      (p, i) { dl[i] = sl[i] = prev->dl[i]; }
        LOOP_MASK_VH(s, p, i) { dh[i] = sh[i] = prev->dh[i]; }
        break;
    case SWS_UOP_EXPAND_PAIR:
    case SWS_UOP_EXPAND_QUAD:
    case SWS_UOP_TO_U8:
    case SWS_UOP_TO_U16:
    case SWS_UOP_TO_U32:
    case SWS_UOP_TO_F32: {
        SwsPixelType to_type = (p->uop == SWS_UOP_EXPAND_PAIR) ? SWS_PIXEL_U16
                             : (p->uop == SWS_UOP_EXPAND_QUAD) ? SWS_PIXEL_U32
                             : (p->uop == SWS_UOP_TO_U8)       ? SWS_PIXEL_U8
                             : (p->uop == SWS_UOP_TO_U16)      ? SWS_PIXEL_U16
                             : (p->uop == SWS_UOP_TO_U32)      ? SWS_PIXEL_U32
                             :                                   SWS_PIXEL_F32;
        size_t src_el_size = s->el_size;
        size_t dst_el_size = ff_sws_pixel_type_size(to_type);
        bool src_use_vh = (p->block_size * src_el_size) > 16;
        bool dst_use_vh = (p->block_size * dst_el_size) > 16;

        LOOP_MASK(p, i)     { dl[i] = sl[i] = prev->dl[i]; }
        LOOP_MASK_VH(s, p, i) { dh[i] = sh[i] = prev->dh[i]; } // TODO remove tmp needed?
        if (src_use_vh && dst_use_vh) {
            LOOP_MASK(p, i) { dh[i] = sh[i] = prev->dh[i]; }
        } else if (!src_use_vh && dst_use_vh) {
            LOOP_MASK(p, i) { dh[i] = a64reg_vec(rs, -1); }
        } else if (src_use_vh && !dst_use_vh) {
            LOOP_MASK(p, i) { sh[i] = prev->dh[i]; a64reg_vec_free(rs, sh[i]); }
        }
        break;
    }
    case SWS_UOP_SCALE:
        LOOP_MASK      (p, i) { dl[i] = sl[i] = prev->dl[i]; }
        LOOP_MASK_VH(s, p, i) { dh[i] = sh[i] = prev->dh[i]; }
        break;
    case SWS_UOP_MIN:
        LOOP_MASK      (p, i) { dl[i] = sl[i] = prev->dl[i]; }
        LOOP_MASK_VH(s, p, i) { dh[i] = sh[i] = prev->dh[i]; }
        break;
    case SWS_UOP_MAX:
        LOOP_MASK      (p, i) { dl[i] = sl[i] = prev->dl[i]; }
        LOOP_MASK_VH(s, p, i) { dh[i] = sh[i] = prev->dh[i]; }
        break;
    case SWS_UOP_UNPACK:
        sl[0] = prev->dl[0];
        if (s->use_vh)
            sh[0] = prev->dh[0];
        LOOP_MASK      (p, i) { dl[i] = i ? a64reg_vec(rs, -1) : sl[i]; }
        LOOP_MASK_VH(s, p, i) { dh[i] = i ? a64reg_vec(rs, -1) : sh[i]; }
        break;
    case SWS_UOP_PACK:
        LOOP_MASK      (p, i) { dl[i] = sl[i] = prev->dl[i]; }
        LOOP_MASK_VH(s, p, i) { dh[i] = sh[i] = prev->dh[i]; }
        LOOP_MASK      (p, i) { if (i) { a64reg_vec_free(rs, sl[i]); } }
        LOOP_MASK_VH(s, p, i) { if (i) { a64reg_vec_free(rs, sh[i]); } }
        break;
    case SWS_UOP_LSHIFT:
        LOOP_MASK      (p, i) { dl[i] = sl[i] = prev->dl[i]; }
        LOOP_MASK_VH(s, p, i) { dh[i] = sh[i] = prev->dh[i]; }
        break;
    case SWS_UOP_RSHIFT:
        LOOP_MASK      (p, i) { dl[i] = sl[i] = prev->dl[i]; }
        LOOP_MASK_VH(s, p, i) { dh[i] = sh[i] = prev->dh[i]; }
        break;
    case SWS_UOP_CLEAR:
        /* TODO factor clear into setup whenever possible. */
        LOOP_MASK      (p, i) {
            dl[i] = (n && rasm_op_type(prev->dl[i]) != RASM_OP_NONE) ? prev->dl[i] : a64reg_vec(rs, -1);
        } else {
            dl[i] = sl[i] = prev->dl[i];
        }
        LOOP_MASK_VH(s, p, i) {
            dh[i] = (n && rasm_op_type(prev->dh[i]) != RASM_OP_NONE) ? prev->dh[i] : a64reg_vec(rs, -1);
        } else {
            dh[i] = sh[i] = prev->dh[i];
        }
        break;
    case SWS_UOP_LINEAR:
    case SWS_UOP_LINEAR_FMA: {
        /**
         * p->mask only covers rows the matrix actually computes (see
         * the SWS_UOP_LINEAR/LINEAR_FMA case in
         * ff_sws_aarch64_ops_translate()) -- it excludes both columns
         * that are read as inputs but never written (e.g. a 3-in/1-out
         * matrix like RGB -> gray only needs output row 0, but still
         * reads input columns 0, 1 and 2), and rows that are pure
         * identity passthroughs the matrix doesn't touch at all (e.g. a
         * channel reorder with only one real coefficient row). Both
         * need their register propagated unchanged so later ops can
         * still read them.
         */
        LOOP_MASK      (p, i) { dl[i] = sl[i] = prev->dl[i]; }
        LOOP_MASK_VH(s, p, i) { dh[i] = sh[i] = prev->dh[i]; }
        for (int i = 0; i < 4; i++) {
            if (SWS_COMP_TEST(p->mask, i))
                continue;
            if (rasm_op_type(prev->dl[i]) != RASM_OP_NONE)
                dl[i] = sl[i] = prev->dl[i];
            if (s->use_vh && rasm_op_type(prev->dh[i]) != RASM_OP_NONE)
                dh[i] = sh[i] = prev->dh[i];
        }

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
        LOOP      (save_mask, i) { dl[i] = a64reg_vec(rs, -1); }
        LOOP_VH(s, save_mask, i) { dh[i] = a64reg_vec(rs, -1); }
        if (p->uop == SWS_UOP_LINEAR)
            alloc_scratch_vecs(rs, 4, &vt[8]);
        LOOP      (save_mask, i) { a64reg_vec_free(rs, sl[i]); }
        LOOP_VH(s, save_mask, i) { a64reg_vec_free(rs, sh[i]); }
        break;
    }
    case SWS_UOP_DITHER:
        /**
         * p->mask only covers the components that actually get a dither
         * offset (see the SWS_UOP_DITHER case in
         * ff_sws_aarch64_ops_translate()); it is not the generic
         * "needed downstream" mask like most other uops. Components not
         * in p->mask still need their register propagated unchanged so
         * later ops can read them.
         */
        for (int i = 0; i < 4; i++) {
            if (rasm_op_type(prev->dl[i]) != RASM_OP_NONE)
                dl[i] = sl[i] = prev->dl[i];
        }
        if (s->use_vh) {
            for (int i = 0; i < 4; i++) {
                if (rasm_op_type(prev->dh[i]) != RASM_OP_NONE)
                    dh[i] = sh[i] = prev->dh[i];
            }
        }
        alloc_scratch_vecs(rs, 2, vt);
        break;
    }

    /* constants */
    switch (p->uop) {
    case SWS_UOP_READ_BIT:
        regs->vk[0] = jit_push_v128(s, res->priv.data);
        regs->vk[1] = jit_push_vimm(s, SWS_PIXEL_U8, 1);
        break;
    case SWS_UOP_READ_NIBBLE:
        regs->vk[0] = jit_push_vimm(s, SWS_PIXEL_U8, 0x0f);
        break;
    case SWS_UOP_WRITE_BIT:
        regs->vk[0] = jit_push_v128(s, res->priv.data);
        break;
    case SWS_UOP_UNPACK:
        LOOP_MASK(p, i) {
            uint32_t val = (1u << p->par.pack.pattern[i]) - 1;
            regs->vk[i] = jit_push_vimm(s, p->type, val);
        }
        break;
    case SWS_UOP_CLEAR:
        regs->vk[0] = jit_push_v128(s, res->priv.data);
        break;
    case SWS_UOP_MIN:
    case SWS_UOP_MAX:
        LOOP_MASK(p, i) {
            uint32_t val = get_priv(&res->priv, p->type, i);
            regs->vk[i] = jit_push_vimm(s, p->type, val);
        }
        break;
    case SWS_UOP_SCALE: {
        uint32_t val = get_priv(&res->priv, p->type, 0);
        regs->vk[0] = jit_push_vimm(s, p->type, val);
        break;
    }
    case SWS_UOP_LINEAR:
    case SWS_UOP_LINEAR_FMA: {
        const SwsPixel *coeffs = (const SwsPixel *) res->priv.ptr;
        int i_coeff = 0;
        LOOP_MASK(p, i) {
            for (int j = 0; j < 5; j++) {
                bool is_offset = (j == 0);
                int src_j = is_offset ? 4 : (j - 1);
                if (p->par.lin.zero & SWS_MASK(i, src_j))
                    continue;
                regs->linear_vcoeff[i][j] = jit_push_elem(s, SWS_PIXEL_U32, coeffs[i_coeff++].u32);
            }
        }
        break;
    }
    case SWS_UOP_DITHER:
        regs->dither_ptr = jit_push_u64(s, (uint64_t) res->priv.ptr);
        break;
    default:
        break;
    }

    return 0;
}

/*********************************************************************/
static void asmgen_common_frame(SwsAArch64Context *s, SwsCompMask imask, SwsCompMask omask)
{
    AArch64RegState *rs = &s->regstate;

    /* Loop iterator variables. */
    s->bx        = a64reg_gpw(rs, 6);
    s->y         = a64reg_gpw(rs, 3);  /* Reused from SwsOpFunc.y_start argument. */

    /* Scratch registers. */
    s->tmp0      = a64reg_gpx(rs, 16); /* IP0 */
    s->tmp1      = a64reg_gpx(rs, 17); /* IP1 */
}

static void asmgen_process_frame(SwsAArch64Context *s, SwsCompMask imask, SwsCompMask omask)
{
    AArch64RegState *rs = &s->regstate;

    asmgen_common_frame(s, imask, omask);

    /* SwsOpFunc arguments. */
    s->exec      = a64reg_argx(rs, 0); // const SwsOpExec *exec
    s->impl      = a64reg_argx(rs, 1); // const void *priv
    s->bx_start  = a64reg_argw(rs, 2); // int bx_start
    s->y_start   = a64reg_argw(rs, 3); // int y_start
    s->bx_end    = a64reg_argw(rs, 4); // int bx_end
    s->y_end     = a64reg_argw(rs, 5); // int y_end
}

static int aarch64_jit_process(SwsAArch64Context *s, const SwsOpList *ops, SwsCompMask imask, SwsCompMask omask)
{
    RasmContext *r = s->rctx;
    char func_name[128];

    snprintf(func_name, sizeof(func_name), "jit_process_%s_%s_neon",
             av_get_pix_fmt_name(ops->src.format),
             av_get_pix_fmt_name(ops->dst.format));
    rasm_func_begin(r, func_name, true, false);

    asmgen_process(s, imask, omask);

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

    /* Use at most two full vregs during the widest precision section */
    int block_size = (ff_sws_op_list_max_size(ops) == 4) ? 8 : 16;

    /* Only DITHER (Task 6) ever appends to this -- it exists solely to
     * keep its runtime matrix pointer (malloc'd by ff_sws_aarch64_setup(),
     * too large to inline as compile-time data) alive until the compiled
     * function itself is torn down; mirrors ops.c's aarch64_compile(). */
    SwsOpChain *chain = ff_sws_op_chain_alloc();
    if (!chain)
        return AVERROR(ENOMEM);

    *out = (SwsCompiledOp) {
        .priv        = chain,
        .slice_align = 1,
        .free        = ff_sws_op_chain_free_cb,
        .block_size  = block_size,
    };

#if 1
    RasmContext *r = rasm_alloc();
    if (!r) {
        ff_sws_op_chain_free(chain);
        return AVERROR(ENOMEM); // TODO check
    }

    SwsAArch64Context s = {
        .rctx            = r,
//        .dither_data_idx = -1,
    };
#endif

    /* Translate all ops into implementation parameters and setup all
     * constant data. */
    SwsAArch64OpImplParams params[SWS_MAX_OPS] = { 0 };
    SwsAArch64OpRegs regs[SWS_MAX_OPS] = { 0 };
    SwsImplResult res[SWS_MAX_OPS] = { 0 };
    for (int i = 0; i < ops->num_ops; i++) {
        if (ops->ops[i].op == SWS_OP_SWIZZLE)
            continue;
        ret = ff_sws_aarch64_ops_translate(ctx, ops, i, block_size, &params[i]);
        if (ret < 0) {
            printf("[%s][%d] %s() goto error\n", __FILE__, __LINE__, __func__);
            goto error;
        }
        ret = ff_sws_aarch64_setup(ops, block_size, i, &params[i], &res[i]);
        if (ret < 0) {
            printf("[%s][%d] %s() goto error\n", __FILE__, __LINE__, __func__);
            goto error;
        }
    }

    printf("%s -> %s\n",
           av_get_pix_fmt_name(ops->src.format),
           av_get_pix_fmt_name(ops->dst.format));

    const SwsOp *read      = ff_sws_op_list_input(ops);
    const SwsOp *write     = ff_sws_op_list_output(ops);
    const int read_planes  = read ? ff_sws_rw_op_planes(read) : 0;
    const int write_planes = ff_sws_rw_op_planes(write);
    SwsCompMask imask = SWS_COMP_MASK(read_planes > 0,  read_planes > 1,  read_planes > 2,  read_planes > 3);
    SwsCompMask omask = SWS_COMP_MASK(write_planes > 0, write_planes > 1, write_planes > 2, write_planes > 3);

    asmgen_process_frame(&s, imask, omask);

    for (int i = 0; i < ops->num_ops; i++) {
        if (ops->ops[i].op == SWS_OP_SWIZZLE) {
            aarch64_jit_setup_swizzle(&s, &ops->ops[i], &regs[i], block_size);
            continue;
        }
        ret = aarch64_jit_setup(&s, params, res, regs, i, imask, omask);
        if (ret < 0) {
            printf("[%s][%d] %s() goto error\n", __FILE__, __LINE__, __func__);
            goto error;
        }
    }

    /* create process */
    ret = aarch64_jit_process(&s, ops, imask, omask);
    if (ret < 0) {
        printf("[%s][%d] %s() goto error\n", __FILE__, __LINE__, __func__);
        goto error;
    }

    for (int i = 0; i < ops->num_ops; i++)
        print_regs(&params[i], &regs[i]);

    // TODO free res

    /* add all ops */
    rasm_set_current_node(r, s.loop);
    for (int i = 0; i < ops->num_ops; i++) {
        if (ops->ops[i].op == SWS_OP_SWIZZLE) {
            aarch64_jit_op_swizzle(&s, &ops->ops[i], &regs[i], block_size);
            continue;
        }
        ret = asmgen_op_jit(&s, &params[i], &regs[i]);
        if (ret < 0) {
            printf("[%s][%d] %s() goto error\n", __FILE__, __LINE__, __func__);
            goto error;
        }
    }

    if (s.data_count)
        load_constants(&s);

    AVBPrint bp;
    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
    rasm_print(s.rctx, &bp);

    if (1 || getenv("SWS_JIT_DUMP")) {
        fputs(bp.str, stdout);
    }

    uint8_t *text;
    size_t text_size;
    ret = ff_sws_jit_assemble_llvm(bp.str, &text, &text_size);
    if (ret < 0) {
        printf("[%s][%d] %s() ASSEMBLE ERROR ret %d\n", __FILE__, __LINE__, __func__, ret);
        // fputs(bp.str, stdout);
    }

    av_bprint_finalize(&bp, NULL);

    out->func      = (SwsOpFunc) text;
    out->cpu_flags = AV_CPU_FLAG_NEON;

error:
    if (ret < 0) {
        rasm_free(&s.rctx);
        ff_sws_op_chain_free(chain);
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
