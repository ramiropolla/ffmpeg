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
    [SWS_UOP_PERMUTE       ] = "permute",
    [SWS_UOP_COPY          ] = "copy",
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

static int asmgen_op_jit(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                         SwsAArch64OpRegs *regs)
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
    reshape_io_vectors(regs, s->el_count, el_size);
    reshape_temp_vectors(regs, s->el_count, el_size);
    reshape_const_vectors(regs, s->el_count, el_size);
    // reshape_lin_vectors(regs, el_size);

    rasm_add_commentf(r, (char[128]){0}, 128, "=> %s", op_type_names[p->uop]);

printf("[%s][%d] %s() %s\n", __FILE__, __LINE__, __func__, op_type_names[p->uop]);
    switch (p->uop) {
    case SWS_UOP_READ_BIT:     asmgen_op_read_bit(s, p, regs);     break;
    case SWS_UOP_READ_NIBBLE:  asmgen_op_read_nibble(s, p, regs);  break;
    case SWS_UOP_READ_PACKED:
        asmgen_op_read_packed(s, p, regs);
        if (rasm_op_type(regs->vt[0]) == AARCH64_OP_VEC) {
            /* Setup found the coalesced destination wasn't a contiguous
             * register run for ld2/ld3/ld4: the read landed in a fresh
             * block (regs->dl/dh) and stashed the real target in vt[].
             * A component the read still has to fetch (ld4 always reads
             * all of them) may have no real consumer downstream (e.g. an
             * alpha channel that gets dropped) -- vt[i] stays "none" for
             * those, so nothing needs moving out of the fresh register. */
            LOOP_MASK(p, i) {
                if (rasm_op_type(regs->vt[i]) == AARCH64_OP_VEC)
                    i_mov(r, regs->vt[i], regs->dl[i]);
                if (s->use_vh && rasm_op_type(regs->vt[4 + i]) == AARCH64_OP_VEC)
                    i_mov(r, regs->vt[4 + i], regs->dh[i]);
            }
        }
        break;
    case SWS_UOP_READ_PLANAR:  asmgen_op_read_planar(s, p, regs);  break;
    case SWS_UOP_WRITE_BIT:    asmgen_op_write_bit(s, p, regs);    break;
    case SWS_UOP_WRITE_NIBBLE: asmgen_op_write_nibble(s, p, regs); break;
    case SWS_UOP_WRITE_PACKED: asmgen_op_write_packed(s, p, regs); break;
    case SWS_UOP_WRITE_PLANAR: asmgen_op_write_planar(s, p, regs); break;
    case SWS_UOP_SWAP_BYTES:   asmgen_op_swap_bytes(s, p, regs);   break;
    case SWS_UOP_PERMUTE:      asmgen_op_move(s, p, regs);         break;
    case SWS_UOP_COPY:         asmgen_op_move(s, p, regs);         break;
    case SWS_UOP_UNPACK:       asmgen_op_unpack(s, p, regs);       break;
    case SWS_UOP_PACK:         asmgen_op_pack(s, p, regs);         break;
    case SWS_UOP_LSHIFT:       asmgen_op_lshift(s, p, regs);       break;
    case SWS_UOP_RSHIFT:       asmgen_op_rshift(s, p, regs);       break;
    case SWS_UOP_CLEAR:
        // TODO
        break;
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
        if (!memcmp(&s->data[i].vec, val, 16)) {
            return s->data[i].op;
        }
    }

    /* Add it to our data and create a new vector. */
    int idx = s->data_count++;
    memcpy(&s->data[idx].vec, val, 16);
    s->data[idx].op = v_q(a64reg_unclobbered_vec(&s->regstate));

    return s->data[idx].op;
}

/* Returns a RasmOp with the u64. */
static RasmOp jit_push_u64(SwsAArch64Context *s, uint64_t val)
{
    /* Add it to our data and create a new vector. */
    int idx = s->data_count++;
    s->data[idx].vec.u64[0] = val;
    s->data[idx].op = a64reg_unclobbered_gpx(&s->regstate);
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
    /* Check if we already have it. */
    for (int i = 0; i < s->data_count; i++) {
        for (int j = 0; j < 4; j++) {
            if (s->data[i].vec.u32[j] == val)
                return a64op_elem(v_4s(s->data[i].op), j);
        }
    }

    int idx = s->elem_count++;
    int data_idx;
    int data_elem;
    if (!(idx & 3)) {
        data_idx  = s->data_count;
        data_elem = 0;
        SwsAArch64Vector dummy = { 0 };
        jit_push_v128(s, &dummy);
    } else {
        data_idx  = s->elem[idx - 1].data_idx;
        data_elem = s->elem[idx - 1].data_elem + 1;
    }
    s->elem[idx].data_idx  = data_idx;
    s->elem[idx].data_elem = data_elem;
    s->data[data_idx].vec.u32[data_elem] = val;

    return a64op_elem(v_4s(s->data[data_idx].op), data_elem);
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
        snprintf(buf, 16, "??");
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

/* Allocate `count` scratch vector registers whose values are only ever
 * read by instructions this same op emits (never coalesced with a
 * neighboring op's src/dst). They stay mutually distinct for the
 * duration of this op (all `count` are allocated before any of them is
 * freed -- op bodies routinely need more than one of these live at once,
 * e.g. a temp pair loaded by one ldp, or several fmul results still
 * pending their matching fadd), then all get freed together so ops
 * processed later in the backward walk (earlier in program order) can
 * reuse the same physical slots instead of them staying reserved for the
 * rest of setup. */
static void alloc_scratch_vecs(AArch64RegState *rs, int count, RasmOp *out)
{
    for (int i = 0; i < count; i++)
        out[i] = a64reg_vec(rs, -1);
    for (int i = 0; i < count; i++)
        a64reg_vec_free(rs, out[i]);
}

/* Allocate `count` consecutive free vector registers, needed by
 * structured loads (ld2/ld3/ld4 -- see vv_2()/vv_3()/vv_4() in rasm.h,
 * which assert consecutive register numbers). */
static void a64reg_vec_block(AArch64RegState *rs, int count, RasmOp *out)
{
    for (int base = 0; base <= 32 - count; base++) {
        uint32_t mask = ((1u << count) - 1) << base;
        if (!(rs->vec_used & mask)) {
            for (int i = 0; i < count; i++)
                out[i] = a64reg_vec(rs, base + i);
            return;
        }
    }
    av_assert0(!"no consecutive vector registers available");
}

/* True if regs[0..n-1] are already a consecutive run of real vector
 * registers (READ_PACKED's mask is always components 0..n-1, with no
 * gaps -- see ff_sws_aarch64_ops_translate()). */
static bool vec_regs_contiguous(const RasmOp *regs, int n)
{
    if (rasm_op_type(regs[0]) != AARCH64_OP_VEC)
        return false;
    int base = a64op_vec_n(regs[0]);
    for (int i = 1; i < n; i++) {
        if (rasm_op_type(regs[i]) != AARCH64_OP_VEC || a64op_vec_n(regs[i]) != base + i)
            return false;
    }
    return true;
}

static int aarch64_jit_setup_banks(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                                   SwsAArch64OpRegs *regs, int i, SwsCompMask imask, SwsCompMask omask)
{
    p += i;
    regs += i;

    size_t el_size = ff_sws_pixel_type_size(p->type);
    size_t total_size = p->block_size * el_size;

    s->vec_size = FFMIN(total_size, 16);
    s->use_vh = (s->vec_size != total_size);

    s->el_size = el_size;
    s->el_count = s->vec_size / el_size;

    /* banks */
    switch (p->uop) {
    case SWS_UOP_READ_BIT:
    case SWS_UOP_READ_NIBBLE:
    case SWS_UOP_READ_PLANAR:
        memcpy(regs[0].dl, regs[1].sl, sizeof(regs[0].dl));
        memcpy(regs[0].dh, regs[1].sh, sizeof(regs[0].dh));
        if (p->uop == SWS_UOP_READ_BIT || p->uop == SWS_UOP_READ_NIBBLE) {
            /* vt[0] here is pure scratch, confined to this op's own
             * instruction sequence (asmgen_op_read_bit()/_read_nibble()
             * combine it into dl[0] and never reference it again) --
             * unlike the direct a64reg_vec() this used to be, route it
             * through alloc_scratch_vecs() so the register is freed
             * immediately instead of staying marked "used" for the rest
             * of the backward walk. READ is normally the first op in
             * program order (last processed here), so this specific
             * leak couldn't compound with anything else -- found while
             * investigating a separate register-exhaustion report, not
             * itself the cause, but a real bug regardless. */
            RasmOp vtmp[1];
            alloc_scratch_vecs(&s->regstate, 1, vtmp);
            regs[0].vt[0] = vtmp[0];
        }
        fprintf(stderr, "=> READ in\n");
        LOOP(imask, i) { s->in     [i] = a64reg_gpx(&s->regstate, -1); }
        fprintf(stderr, "=> READ bump\n");
        LOOP(imask, i) { s->in_bump[i] = a64reg_gpx(&s->regstate, -1); }
        break;
    case SWS_UOP_READ_PACKED: {
        /* ld2/ld3/ld4 need a *consecutive* run of registers. Whatever got
         * coalesced from the successor only reflects which registers
         * happened to be free when it was allocated, so it isn't
         * guaranteed contiguous -- when it isn't, read into a fresh
         * contiguous block instead and swizzle (real mov instructions,
         * emitted in asmgen_op_jit()) into what the successor actually
         * expects. Suboptimal when it triggers, but correct; a real fix
         * needs the read side to have a say in the allocation up front. */
        memcpy(regs[0].dl, regs[1].sl, sizeof(regs[0].dl));
        memcpy(regs[0].dh, regs[1].sh, sizeof(regs[0].dh));

        int n = 0;
        LOOP_MASK(p, i) n++;
        bool need_swizzle = !vec_regs_contiguous(regs[0].dl, n) ||
                             (s->use_vh && !vec_regs_contiguous(regs[0].dh, n));
        if (need_swizzle) {
            RasmOp fresh_l[4];
            RasmOp fresh_h[4];
            a64reg_vec_block(&s->regstate, n, fresh_l);
            if (s->use_vh)
                a64reg_vec_block(&s->regstate, n, fresh_h);
            int j = 0;
            LOOP_MASK(p, i) {
                regs[0].vt[i] = regs[0].dl[i];     /* swizzle target (low) */
                regs[0].dl[i] = fresh_l[j];
                if (s->use_vh) {
                    regs[0].vt[4 + i] = regs[0].dh[i]; /* swizzle target (high) */
                    regs[0].dh[i] = fresh_h[j];
                }
                j++;
            }
        }
        fprintf(stderr, "=> READ in\n");
        LOOP(imask, i) { s->in     [i] = a64reg_gpx(&s->regstate, -1); }
        fprintf(stderr, "=> READ bump\n");
        LOOP(imask, i) { s->in_bump[i] = a64reg_gpx(&s->regstate, -1); }
        break;
    }
    case SWS_UOP_WRITE_BIT:
    case SWS_UOP_WRITE_NIBBLE:
    case SWS_UOP_WRITE_PACKED:
    case SWS_UOP_WRITE_PLANAR:
        LOOP_MASK      (p, i) { regs[0].sl[i] = a64reg_vec(&s->regstate, -1); }
        LOOP_MASK_VH(s, p, i) { regs[0].sh[i] = a64reg_vec(&s->regstate, -1); }
        if (p->uop == SWS_UOP_WRITE_BIT || p->uop == SWS_UOP_WRITE_NIBBLE) {
            RasmOp vtmp[2];
            alloc_scratch_vecs(&s->regstate, 2, vtmp);
            regs[0].vt[0] = vtmp[0];
            regs[0].vt[1] = vtmp[1];
        }
        fprintf(stderr, "=> WRITE out\n");
        LOOP(omask, i) { s->out     [i] = a64reg_gpx(&s->regstate, -1); }
        fprintf(stderr, "=> WRITE bump\n");
        LOOP(omask, i) { s->out_bump[i] = a64reg_gpx(&s->regstate, -1); }
        break;
    case SWS_UOP_SWAP_BYTES:
    case SWS_UOP_LSHIFT:
    case SWS_UOP_RSHIFT:
    case SWS_UOP_MIN:
    case SWS_UOP_MAX:
    case SWS_UOP_SCALE:
        /* Elementwise, one-in-one-out: dst can alias src in place. */
        memcpy(regs[0].dl, regs[1].sl, sizeof(regs[0].dl));
        memcpy(regs[0].dh, regs[1].sh, sizeof(regs[0].dh));
        memcpy(regs[0].sl, regs[0].dl, sizeof(regs[0].sl));
        memcpy(regs[0].sh, regs[0].dh, sizeof(regs[0].sh));
        break;
    case SWS_UOP_CLEAR:
        /* Masked (actually-cleared) components: the value is loaded
         * exactly once, before the loop, directly into whichever
         * register the successor already expects (coalesced below) --
         * so unlike a normal elementwise op, that register must stay
         * reserved for the rest of compilation: nothing earlier in
         * program order may be handed the same number, since it's
         * written only once but read every iteration from here on. Not
         * aliasing/freeing sl/sh for those is exactly what keeps them
         * reserved -- a64reg_vec() never hands out a number that's
         * still marked used.
         *
         * Non-masked components (e.g. R/G/B when only alpha is being
         * cleared) are pure passthrough -- CLEAR touches nothing there
         * at all, so sl/sh must alias dl/dh directly, or the
         * predecessor never learns it needs to write the real target
         * register (left unset, sl[i]/sh[i] would decode as v0 --
         * a64op_vec_n() just reads a raw byte, no type check for
         * rasm_op_none() -- silently corrupting whatever v0 holds
         * instead of erroring; this exact bug, just for CONVERT/LINEAR
         * instead of CLEAR, is what "-src yuva444p -dst argb" hit). */
        memcpy(regs[0].dl, regs[1].sl, sizeof(regs[0].dl));
        memcpy(regs[0].dh, regs[1].sh, sizeof(regs[0].dh));
        for (int i = 0; i < 4; i++) {
            if (p->mask & SWS_COMP(i))
                continue;
            regs[0].sl[i] = regs[0].dl[i];
            if (s->use_vh)
                regs[0].sh[i] = regs[0].dh[i];
        }
        break;
    case SWS_UOP_PERMUTE:
    case SWS_UOP_COPY: {
        /* asmgen_op_move() emits real mov instructions between distinct
         * src/dst registers (see swizzle_a64op()/swizzle_emit()), so src
         * and dst must not alias -- except for components that are
         * neither read nor written by any move (pure passthrough). */
        memcpy(regs[0].dl, regs[1].sl, sizeof(regs[0].dl));
        memcpy(regs[0].dh, regs[1].sh, sizeof(regs[0].dh));

        bool is_src[4] = { false, false, false, false };
        bool is_dst[4] = { false, false, false, false };
        bool need_tmp  = false;
        for (int m = 0; m < p->par.move.num_moves; m++) {
            int8_t mv_src = p->par.move.src[m];
            int8_t mv_dst = p->par.move.dst[m];
            /* A move can read/write the temp on one side while still
             * touching a *real* component on the other (e.g. "save comp0
             * to tmp" is src=0, dst=-1) -- so these three checks are
             * independent, not mutually exclusive. */
            if (mv_src == -1 || mv_dst == -1)
                need_tmp = true;
            if (mv_src != -1)
                is_src[mv_src] = true;
            if (mv_dst != -1)
                is_dst[mv_dst] = true;
        }

        for (int i = 0; i < 4; i++) {
            /* No move writes this position, so its output is implicitly
             * the unmodified input -- the move-list generator
             * (convert_swizzle_to_moves()) omits the identity move here
             * on the assumption that src and dst for position i already
             * coincide, which is only true if we make it true: alias
             * dst=src, even if i is *also* used as a source for other
             * positions (that's fine -- the other moves need to read the
             * original value, and this position holding still-unmodified
             * data via the same register satisfies both at once).
             *
             * Otherwise, if the original value is *also* still needed as
             * a source for another move, it must survive in an
             * independent register, since dst is about to be overwritten.
             *
             * (is_dst[i] && !is_src[i]: written by a move, never read as
             * a source elsewhere -- sl[i] is irrelevant, leave unset.)
             *
             * Allocate the low bank for all four components before the
             * high bank (below) rather than interleaving low/high per
             * component, purely so the resulting registers read as a
             * contiguous low..high run in the generated assembly. */
            if (!is_dst[i])
                regs[0].sl[i] = regs[0].dl[i];
            else if (is_src[i])
                regs[0].sl[i] = a64reg_vec(&s->regstate, -1);
        }
        if (s->use_vh) {
            for (int i = 0; i < 4; i++) {
                if (!is_dst[i])
                    regs[0].sh[i] = regs[0].dh[i];
                else if (is_src[i])
                    regs[0].sh[i] = a64reg_vec(&s->regstate, -1);
            }
        }
        if (need_tmp) {
            RasmOp vtmp[2];
            alloc_scratch_vecs(&s->regstate, s->use_vh ? 2 : 1, vtmp);
            regs[0].vt[0] = vtmp[0];
            if (s->use_vh)
                regs[0].vt[1] = vtmp[1];
        }
        break;
    }
    case SWS_UOP_UNPACK: {
        /* Only component 0 (the packed value) is read. */
        memcpy(regs[0].dl, regs[1].sl, sizeof(regs[0].dl));
        memcpy(regs[0].dh, regs[1].sh, sizeof(regs[0].dh));

        int off0 = p->par.pack.pattern[3] + p->par.pack.pattern[2] + p->par.pack.pattern[1];
        if (off0) {
            regs[0].sl[0] = a64reg_vec(&s->regstate, -1);
            if (s->use_vh)
                regs[0].sh[0] = a64reg_vec(&s->regstate, -1);
        } else {
            /* asmgen_op_unpack() emits no instruction for component 0
             * in this case, so dst must be the exact same register as
             * src. */
            regs[0].sl[0] = regs[0].dl[0];
            if (s->use_vh)
                regs[0].sh[0] = regs[0].dh[0];
        }
        break;
    }
    case SWS_UOP_PACK: {
        /* Only component 0 is produced; components 1..3 are independent
         * inputs consumed (and clobbered in place) while combining into
         * it -- see asmgen_op_pack(). */
        regs[0].dl[0] = regs[1].sl[0];
        regs[0].dh[0] = regs[1].sh[0];
        regs[0].sl[0] = regs[0].dl[0];
        regs[0].sh[0] = regs[0].dh[0];

        LOOP_MASK(p, i) {
            if (i == 0)
                continue;
            regs[0].sl[i] = a64reg_vec(&s->regstate, -1);
            regs[0].dl[i] = regs[0].sl[i];
            if (s->use_vh) {
                regs[0].sh[i] = a64reg_vec(&s->regstate, -1);
                regs[0].dh[i] = regs[0].sh[i];
            }
        }
        break;
    }
    case SWS_UOP_TO_U8:
    case SWS_UOP_TO_U16:
    case SWS_UOP_TO_U32:
    case SWS_UOP_TO_F32: {
        /* asmgen_op_convert() chains multiple internal stages, each
         * reading then writing regs->dl/dh directly (its local sl/dl/sh/dh
         * views just get reassigned between stages -- the underlying
         * registers are always regs->sl/dl/sh/dh) -- so the *low* bank can
         * always alias src=dst in place across the whole conversion
         * (verified by hand against every branch). The *high* bank can
         * alias too UNLESS this op sheds vh entirely (input needed it,
         * output doesn't): that narrowing path combines lo+hi via an
         * `ins`, and whether dh can alias sh there depends on whether a
         * pre-stage already ran (see below). */
        memcpy(regs[0].dl, regs[1].sl, sizeof(regs[0].dl));
        memcpy(regs[0].dh, regs[1].sh, sizeof(regs[0].dh));

        /* A component outside this op's own mask (e.g. an alpha channel
         * bypassing a color-matrix pipeline that only masks Y/U/V-derived
         * R/G/B) is pure passthrough -- asmgen_op_convert() never touches
         * it (it only ever loops over LOOP_MASK()), so sl/sh must alias
         * dl/dh directly here too, same as the masked low-bank case just
         * below. Left unset, sl[i]/sh[i] would stay rasm_op_none() --
         * which a64op_vec_n() silently decodes as register v0 (it just
         * reads a raw byte, no type check), not an error -- silently
         * corrupting whatever v0 holds instead of ever being caught. */
        for (int i = 0; i < 4; i++) {
            if (p->mask & SWS_COMP(i))
                continue;
            regs[0].sl[i] = regs[0].dl[i];
            regs[0].sh[i] = regs[0].dh[i];
        }

        SwsPixelType to_type = (p->uop == SWS_UOP_TO_U8)  ? SWS_PIXEL_U8  :
                               (p->uop == SWS_UOP_TO_U16) ? SWS_PIXEL_U16 :
                               (p->uop == SWS_UOP_TO_U32) ? SWS_PIXEL_U32 :
                                                            SWS_PIXEL_F32;
        bool src_use_vh = (p->block_size * s->el_size) > 16;
        bool dst_use_vh = (p->block_size * ff_sws_pixel_type_size(to_type)) > 16;

        LOOP_MASK(p, i) { regs[0].sl[i] = regs[0].dl[i]; }
        if (src_use_vh && dst_use_vh) {
            LOOP_MASK(p, i) { regs[0].sh[i] = regs[0].dh[i]; }
        } else if (src_use_vh && !dst_use_vh) {
            /* Losing vh: the narrowing combine reads the *original* sh
             * via the `ins` instruction, one way or another -- the
             * question is what "original" means at that point:
             *
             * If p->type == F32, the f32->u32 pre-stage (top of
             * asmgen_op_convert()) already ran first and reassigned its
             * *own* local "sh" to "dh" (mirroring how the low bank always
             * aliases) -- so from here on, sh and dh both refer to
             * whatever the pre-stage wrote, and every later read/write of
             * either name lands on that same self-referencing chain, in
             * program order, with nothing else needing the pre-stage's
             * *true* original sh once it's been folded in. dh can safely
             * alias sh -- no extra registers needed at all.
             *
             * Otherwise (integer source, no pre-stage), sh still holds
             * its true, never-yet-touched original value when this
             * branch's own dh-narrowing and ins both run, so dh must be
             * fresh and independent -- aliasing here would have the
             * dh-write clobber the very value ins still needs to read.
             */
            LOOP_MASK(p, i) { regs[0].sh[i] = a64reg_vec(&s->regstate, -1); }
            if (p->type == SWS_PIXEL_F32) {
                LOOP_MASK(p, i) { regs[0].dh[i] = regs[0].sh[i]; }
            } else {
                /* dh[0..n-1] must stay mutually distinct while this op's
                 * own instructions run (one dh write per component, all
                 * still pending their matching ins read) -- collapsing
                 * them onto one shared register (as a naive
                 * allocate-then-immediately-free per component would)
                 * clobbers an earlier component's pending value. */
                RasmOp dtmp[4];
                int n = 0;
                LOOP_MASK(p, i) n++;
                alloc_scratch_vecs(&s->regstate, n, dtmp);
                int j = 0;
                LOOP_MASK(p, i) { regs[0].dh[i] = dtmp[j++]; }
            }
        }
        /* !src_use_vh && dst_use_vh (gaining vh): dh already came from
         * the successor's coalescing above; there's no sh to alias
         * against since the input never had a high bank. */
        break;
    }
    case SWS_UOP_EXPAND_PAIR:
    case SWS_UOP_EXPAND_QUAD:
        /* Same low-bank aliasing as TO_U8/16/32/F32 above. In practice the
         * source here is always a single, lo-only bank (block_size is
         * chosen so the widest type anywhere in the chain still fits
         * lo+hi), so there's never a "losing vh" case to worry about --
         * this op only ever gains a high bank, never sheds one. */
        memcpy(regs[0].dl, regs[1].sl, sizeof(regs[0].dl));
        memcpy(regs[0].dh, regs[1].sh, sizeof(regs[0].dh));
        /* Same out-of-mask passthrough fix as TO_U8/16/32/F32 above --
         * see that case's comment for why leaving sl[i]/sh[i] unset for
         * an unmasked component (e.g. bypassing alpha) is a real bug,
         * not just a missed optimization. */
        for (int i = 0; i < 4; i++) {
            if (p->mask & SWS_COMP(i))
                continue;
            regs[0].sl[i] = regs[0].dl[i];
            regs[0].sh[i] = regs[0].dh[i];
        }
        LOOP_MASK(p, i) { regs[0].sl[i] = regs[0].dl[i]; }
        break;
    case SWS_UOP_LINEAR:
    case SWS_UOP_LINEAR_FMA: {
        /* Each masked *output* component may depend on any input
         * component its row of the matrix doesn't zero out (par.lin.zero),
         * so src and dst can never alias -- but only the input components
         * actually referenced by some row need a register; matches the
         * src_j loop in linear_pass(). */
        memcpy(regs[0].dl, regs[1].sl, sizeof(regs[0].dl));
        memcpy(regs[0].dh, regs[1].sh, sizeof(regs[0].dh));

        bool src_used[4] = { false, false, false, false };
        LOOP_MASK(p, i) {
            for (int src_j = 0; src_j < 4; src_j++) {
                if (!(p->par.lin.zero & SWS_MASK(i, src_j)))
                    src_used[src_j] = true;
            }
        }
        /* A component that's neither an output of this LINEAR (not in
         * mask) nor referenced as an input by any output row (not
         * src_used) -- e.g. an alpha channel bypassing the whole color
         * matrix -- is pure passthrough: alias sl/sh straight to dl/dh,
         * same bug/fix as TO_U8/16/32/F32 above (left unset, they'd
         * silently decode as v0, not error out). */
        for (int i = 0; i < 4; i++) {
            if ((p->mask & SWS_COMP(i)) || src_used[i])
                continue;
            regs[0].sl[i] = regs[0].dl[i];
            if (s->use_vh)
                regs[0].sh[i] = regs[0].dh[i];
        }
        /* Allocate the low bank for all four components before the high
         * bank, rather than interleaving low/high per component, purely
         * so the resulting registers read as a contiguous low..high run
         * in the generated assembly. */
        for (int i = 0; i < 4; i++) {
            if (src_used[i])
                regs[0].sl[i] = a64reg_vec(&s->regstate, -1);
        }
        if (s->use_vh) {
            for (int i = 0; i < 4; i++) {
                if (src_used[i])
                    regs[0].sh[i] = a64reg_vec(&s->regstate, -1);
            }
        }
        if (p->uop == SWS_UOP_LINEAR) {
            /* fmul+fadd split path (see linear_pass()) needs 4 scratch
             * accumulators, vt[8..11], which the instruction-reordering
             * in linear_pass() (rasm_set_current_node()) can keep
             * simultaneously pending -- must stay mutually distinct. */
            RasmOp vtmp[4];
            alloc_scratch_vecs(&s->regstate, 4, vtmp);
            for (int i = 0; i < 4; i++)
                regs[0].vt[8 + i] = vtmp[i];
        }
        break;
    }
    case SWS_UOP_DITHER:
        memcpy(regs[0].dl, regs[1].sl, sizeof(regs[0].dl));
        memcpy(regs[0].dh, regs[1].sh, sizeof(regs[0].dh));
        memcpy(regs[0].sl, regs[0].dl, sizeof(regs[0].sl));
        memcpy(regs[0].sh, regs[0].dh, sizeof(regs[0].sh));
        {
            /* dither_vl/dither_vh are loaded together by one ldp, so must
             * be distinct registers. */
            RasmOp vtmp[2];
            alloc_scratch_vecs(&s->regstate, s->use_vh ? 2 : 1, vtmp);
            regs[0].vt[0] = vtmp[0];
            if (s->use_vh)
                regs[0].vt[1] = vtmp[1];
        }
        break;
    default:
        // TODO av_assert(0);
        break;
    }

    print_regs(p, regs);
    return 0;
}

static uint32_t get_priv(const SwsOpPriv *priv, SwsPixelType type, int i)
{
    return (type == SWS_PIXEL_U8)  ? priv->u8[i]
         : (type == SWS_PIXEL_U16) ? priv->u16[i]
         :                           priv->u32[i];
}

static uint32_t get_umax(SwsPixelType type)
{
    return (type == SWS_PIXEL_U8)  ? 0x000000ff
         : (type == SWS_PIXEL_U16) ? 0x0000ffff
         :                           0xffffffff;
}

static int aarch64_jit_setup_constants(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                                       SwsImplResult *res, SwsAArch64OpRegs *regs, int i)
{
    p += i;
    res += i;
    regs += i;

    size_t el_size = ff_sws_pixel_type_size(p->type);
    size_t total_size = p->block_size * el_size;

    s->vec_size = FFMIN(total_size, 16);
    s->use_vh = (s->vec_size != total_size);

    s->el_size = el_size;
    s->el_count = s->vec_size / el_size;

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
        LOOP_MASK(p, i) {
            uint32_t val = (p->par.clear.zero & SWS_COMP(i)) ? 0
                         : (p->par.clear.one  & SWS_COMP(i)) ? get_umax(p->type)
                         :                                     get_priv(&res->priv, p->type, i);
            if (p->type == SWS_PIXEL_F32) {
                regs->vk[i] = jit_push_elem(s, p->type, val);
            } else {
                regs->vk[i] = jit_push_vimm(s, p->type, val);
            }
        }
        break;
    case SWS_UOP_MIN:
    case SWS_UOP_MAX:
        LOOP_MASK(p, i) {
            uint32_t val = get_priv(&res->priv, p->type, i);
            if (p->type == SWS_PIXEL_F32) {
                regs->vk[i] = jit_push_elem(s, p->type, val);
            } else {
                regs->vk[i] = jit_push_vimm(s, p->type, val);
            }
        }
        break;
    case SWS_UOP_SCALE: {
        uint32_t val = get_priv(&res->priv, p->type, 0);
        if (p->type == SWS_PIXEL_F32) {
            regs->vk[0] = jit_push_elem(s, p->type, val);
        } else {
            regs->vk[0] = jit_push_vimm(s, p->type, val);
        }
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
        printf("[%s][%d] %s() %p\n", __FILE__, __LINE__, __func__, res->priv.ptr);
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
        .sws             = ctx,
        .block_size      = block_size,
        .rctx            = r,
//        .chain           = chain,
//        .dither_data_idx = -1,
    };
#endif

    /* Translate all ops into implementation parameters and setup all
     * constant data. */
    SwsAArch64OpImplParams params[SWS_MAX_OPS] = { 0 };
    SwsAArch64OpRegs regs[SWS_MAX_OPS] = { 0 };
    SwsImplResult res[SWS_MAX_OPS] = { 0 };
    for (int i = 0; i < ops->num_ops; i++) {
        ret = ff_sws_aarch64_ops_translate(ctx, ops, i, block_size, &params[i]);
        if (ret < 0)
            goto error;
        ret = ff_sws_aarch64_setup(ops, block_size, i, &params[i], &res[i]);
        if (ret < 0)
            goto error;
    }

#if 0
    /* The Platform Register (r18) is not used. */
    jit_gpr(&s, 18);
#endif

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

    /* Pass 1: value-flow ("banks") registers, backward over every op. */
    printf("\n=== BACKWARD (register allocation order) ===\n");
    for (int i = ops->num_ops - 1; i >= 0; i--) {
        ret = aarch64_jit_setup_banks(&s, params, regs, i, imask, omask);
        if (ret < 0)
            goto error;
    }

    for (int i = 0; i < ops->num_ops; i++) {
        ret = aarch64_jit_setup_constants(&s, params, res, regs, i);
        if (ret < 0)
            goto error;
    }

    /* create process */
    ret = aarch64_jit_process(&s, ops, imask, omask);
    if (ret < 0)
        goto error;

    printf("\n=== FORWARD (final register map) ===\n");
    for (int i = 0; i < ops->num_ops; i++)
        print_regs(&params[i], &regs[i]);

    // TODO free res

printf("[%s][%d] %s()\n", __FILE__, __LINE__, __func__);

    /* add all ops */
    rasm_set_current_node(r, s.loop);
    for (int i = 0; i < ops->num_ops; i++) {
        ret = asmgen_op_jit(&s, &params[i], &regs[i]);
        if (ret < 0)
            goto error;
    }

printf("[%s][%d] %s()\n", __FILE__, __LINE__, __func__);

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
        printf("[%s][%d] %s() ret %d\n", __FILE__, __LINE__, __func__, ret);
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
