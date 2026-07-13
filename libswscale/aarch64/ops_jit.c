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
/* Emit JIT code. */
#define AARCH64_ASMGEN_JIT
#include "ops_asmgen.c"

/*********************************************************************/
/* Immediates. */

/* Emit load instructions for all collected immediates (v28..v31) and
 * 128-bit data pool vectors (v8..v15) before the inner loop.
 * Uses tmp0 as a scratch GPR for values that cannot be encoded by movi,
 * and as the base pointer for adr + ldr of the data pool. */
static void load_constants(SwsAArch64Context *s)
{
#if 0
    RasmContext *r = s->rctx;
    int pool_idx[SWS_AARCH64_MAX_IMM];
    bool via_pool[SWS_AARCH64_MAX_IMM] = { 0 };

    if (s->n_imm) {
        rasm_add_comment(r, "immediates");

        /* First load large values into distinct scratch GPRs, then movi
         * small values into vectors, then dup the scratch values into
         * vectors. Separating the passes -- and giving each large value
         * its own GPR -- allows the CPU to overlap the (independent)
         * integer and vector instructions instead of stalling on a
         * read-after-write hazard between each mov and its own dup.
         *
         * A word-sized (repeat_len == 4) value with both halves non-zero
         * fits neither a single movz/movn nor (in general) a logical
         * immediate, so building it would need extra GPR arithmetic --
         * instead, push it as a ready-made 4-word broadcast into the
         * data pool below and load it directly, skipping the GPR
         * entirely. 16-bit values (repeat_len <= 2) always fit a single
         * mov, so this only ever applies to repeat_len == 4. */
        RasmOp tmp[SWS_AARCH64_MAX_IMM];
        for (int i = 0; i < s->n_imm; i++) {
            int small_value = (int) (s->imm[i].meta >> 16);
            int repeat_len  = (int) ((s->imm[i].meta >> 8) & 0xff);
            if (small_value || repeat_len == 1)
                continue;
            uint32_t val   = s->imm[i].val;
            uint32_t low16 = val & 0xffff;
            uint32_t hi16  = val >> 16;
            if (repeat_len == 4 && low16 && hi16) {
                /* s->vimm[i] (v28+i) was already handed out to callers
                 * during the setup pass, so it must still end up holding
                 * this value -- push it into the data pool and copy it
                 * into place once the pool has been loaded, below. */
                uint32_t words[4] = { val, val, val, val };
                pool_idx[i] = jit_push_data(s, words);
                via_pool[i] = true;
            } else {
                tmp[i] = jit_gpw(s, -1);
                i_mov(r, tmp[i], IMM((int32_t) (repeat_len == 2 ? low16 : val)));
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
            if (small_value || repeat_len == 1 || via_pool[i])
                continue;
            switch (len) {
            case 2: i_dup(r, v_8h(s->vimm[i]), tmp[i]); break;
            case 4: i_dup(r, v_4s(s->vimm[i]), tmp[i]); break;
            }
            jit_free_gpr(s, tmp[i]);
        }
    }

    if (s->data_count) {
        /* Immediates above may have lazily grown the data pool (the
         * via_pool fallback just above), so the "ldata" label and its
         * contents can only be finalized now, with the final s->data_count.
         * Creating it switches the current node away from the function
         * body, so save/restore it around the const entry. */
        RasmNode *saved_node = rasm_get_current_node(r);
        int data_label = rasm_const_begin(r, "ldata");
        rasm_add_data(r, s->data, s->data_count * 4, RASM_DATA_WORD);
        rasm_set_current_node(r, saved_node);

        rasm_add_comment(r, "data pool");

        RasmOp ptr = s->tmp0;
        i_adr(r, ptr, rasm_op_label(data_label));
        for (int i = 0; i < s->data_count; i++)
            i_ldr(r, s->vdata[i], a64op_off(ptr, (int16_t) (i * 16)));
    }

    /* Now that the data pool above has been loaded, copy any pool-sourced
     * immediates into the v28+i register their callers were already
     * handed during the setup pass. */
    for (int i = 0; i < s->n_imm; i++) {
        if (via_pool[i])
            i_mov(r, v_16b(s->vimm[i]), v_16b(s->vdata[pool_idx[i]]));
    }
#endif
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
    // reshape_all_vectors(s, s->el_count, el_size);

    rasm_add_commentf(r, (char[128]){0}, 128, "=> %s", op_type_names[p->uop]);

printf("[%s][%d] %s() %s\n", __FILE__, __LINE__, __func__, op_type_names[p->uop]);
    switch (p->uop) {
    case SWS_UOP_READ_BIT:     asmgen_op_read_bit(s, p, regs);     break;
    case SWS_UOP_READ_NIBBLE:  asmgen_op_read_nibble(s, p, regs);  break;
    case SWS_UOP_READ_PACKED:  asmgen_op_read_packed(s, p, regs);  break;
    case SWS_UOP_READ_PLANAR:  asmgen_op_read_planar(s, p, regs);  break;
    case SWS_UOP_WRITE_BIT:    asmgen_op_write_bit(s, p, regs);    break;
    case SWS_UOP_WRITE_NIBBLE: asmgen_op_write_nibble(s, p, regs); break;
    case SWS_UOP_WRITE_PACKED: asmgen_op_write_packed(s, p, regs); break;
    case SWS_UOP_WRITE_PLANAR: asmgen_op_write_planar(s, p, regs); break;
    case SWS_UOP_SWAP_BYTES:   asmgen_op_swap_bytes(s, p, regs);   break;
    case SWS_UOP_MOVE:         asmgen_op_move(s, p, regs);         break;
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

#if 0
typedef union SwsOpPriv {
    DECLARE_ALIGNED_16(char, data)[16];

    /* Common types */
    void *ptr;
    uint8_t    u8[16];
    int8_t     i8[16];
    uint16_t   u16[8];
    int16_t    i16[8];
    uint32_t   u32[4];
    int32_t    i32[4];
    float      f32[4];
    uint64_t   u64[2];
    int64_t    i64[2];
    uintptr_t uptr[2];
    intptr_t  iptr[2];
} SwsOpPriv;
#endif

/*********************************************************************/
static RasmOp jit_push_v128(SwsAArch64Context *s, void *val)
{
    /* Check whether we already have it */
    for (int i = 0; i < s->data_count; i++) {
        if (!memcmp(&s->data[i].val, val, 16))
            return s->data[i].op;
    }

    /* Add it to our data and create a new vector */
    int idx = s->data_count++;
    memcpy(s->data[idx].val, val, 16);
    // s->data[idx].op = jit_vec(s, -1);

    return s->data[idx].op;

    // at setup time, i_ldr directly into the reserved RasmOp
}

static RasmOp jit_push_imm(SwsAArch64Context *s, SwsPixelType type, uint32_t val)
{
    /* Expand to u32 */
    switch (type) {
    case SWS_PIXEL_U8:
        val = val | (val <<  8);
        av_fallthrough;
    case SWS_PIXEL_U16:
        val = val | (val << 16);
        break;
    }

    /* Check whether we already have it */
    for (int i = 0; i < s->imm_count; i++) {
        if (s->imm[i].val == val)
            return s->imm[i].op;
    }

    /* Add it to our data and create a new vector */
    int idx = s->imm_count++;
    s->imm[idx].val = val;
    // s->imm[idx].op = jit_vec(s, -1);

    return s->imm[idx].op;

    // at setup time, depending on the value, either movi to full vector, or ldr+dup into full vector

#if 0
    /* Expand to u32 */
    switch (len) {
    case 1:
        u32 = u32 | (u32 <<  8);
        av_fallthrough;
    case 2:
        u32 = u32 | (u32 << 16);
        break;
    }

    /* Check whether we already have it */
    for (int i = 0; i < s->imm_count; i++) {
        if (s->imm[i].val == u32)
            return s->imm[i].op;
    }

    /* Check if data can be represented by repeating smaller value */
    int repeat_len;
    int small_value; // TODO should be movi-encodable
    union {
        uint32_t u32;
        uint16_t u16[2];
        uint8_t  u8[4];
    } u;
    u.u32 = u32;
    if (u.u16[0] != u.u16[1]) {
        repeat_len = 4;
        small_value = (u.u32 < 0x100);
    } else if (u.u8[0] != u.u8[1]) {
        repeat_len = 2;
        small_value = (u.u16[0] < 0x100);
    } else {
        repeat_len = 1;
        small_value = 1;
    }

    /* Add it to our data and create a new vector */
    int idx = s->imm_count++;
    s->imm[idx].val         = u32;
    s->imm[idx].len         = len;
    s->imm[idx].repeat_len  = repeat_len;
    s->imm[idx].small_value = small_value;
    // s->imm[idx].op          = jit_vec(s, -1);

    return s->imm[idx].op;
#endif
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

static int aarch64_jit_setup(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                             const SwsImplResult *res, SwsAArch64OpRegs *regs, int i, SwsCompMask imask, SwsCompMask omask)
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

    /* banks */
    switch (p->uop) {
    case SWS_UOP_READ_BIT:
    case SWS_UOP_READ_NIBBLE:
    case SWS_UOP_READ_PACKED:
    case SWS_UOP_READ_PLANAR:
        memcpy(regs[0].dl, regs[1].sl, sizeof(regs[0].dl));
        memcpy(regs[0].dh, regs[1].sh, sizeof(regs[0].dh));
        // memcpy(regs[0].sl, regs[0].dl, sizeof(regs[0].sl));
        // memcpy(regs[0].sh, regs[0].dh, sizeof(regs[0].sh));
        fprintf(stderr, "=> READ in\n");
        LOOP(imask, i) { s->in     [i] = a64frame_gpx(&s->frame, -1); }
        fprintf(stderr, "=> READ bump\n");
        LOOP(imask, i) { s->in_bump[i] = a64frame_gpx(&s->frame, -1); }
        break;
    case SWS_UOP_WRITE_BIT:
    case SWS_UOP_WRITE_NIBBLE:
    case SWS_UOP_WRITE_PACKED:
    case SWS_UOP_WRITE_PLANAR:
        LOOP_MASK      (p, i) { regs[0].sl[i] = a64frame_vec(&s->vframe, -1); }
        LOOP_MASK_VH(s, p, i) { regs[0].sh[i] = a64frame_vec(&s->vframe, -1); }
        fprintf(stderr, "=> WRITE out\n");
        LOOP(omask, i) { s->out     [i] = a64frame_gpx(&s->frame, -1); }
        fprintf(stderr, "=> WRITE bump\n");
        LOOP(omask, i) { s->out_bump[i] = a64frame_gpx(&s->frame, -1); }
        break;
    case SWS_UOP_SWAP_BYTES:
    case SWS_UOP_LSHIFT:
    case SWS_UOP_RSHIFT:
    case SWS_UOP_MIN:
    case SWS_UOP_MAX:
    case SWS_UOP_SCALE:
        memcpy(regs[0].dl, regs[1].sl, sizeof(regs[0].dl));
        memcpy(regs[0].dh, regs[1].sh, sizeof(regs[0].dh));
        memcpy(regs[0].sl, regs[0].dl, sizeof(regs[0].sl));
        memcpy(regs[0].sh, regs[0].dh, sizeof(regs[0].sh));
        break;
    case SWS_UOP_CLEAR:
        memcpy(regs[0].dl, regs[1].sl, sizeof(regs[0].dl));
        memcpy(regs[0].dh, regs[1].sh, sizeof(regs[0].dh));
        memcpy(regs[0].sl, regs[0].dl, sizeof(regs[0].sl));
        memcpy(regs[0].sh, regs[0].dh, sizeof(regs[0].sh));
        LOOP_MASK      (p, i) { a64frame_vec_free(&s->vframe, regs[0].sl[i]); regs[0].sl[i] = rasm_op_none(); }
        LOOP_MASK_VH(s, p, i) { a64frame_vec_free(&s->vframe, regs[0].sh[i]); regs[0].sh[i] = rasm_op_none(); }
        break;
    case SWS_UOP_MOVE:
        // TODO wrong
        memcpy(regs[0].dl, regs[1].sl, sizeof(regs[0].dl));
        memcpy(regs[0].dh, regs[1].sh, sizeof(regs[0].dh));
        memcpy(regs[0].sl, regs[0].dl, sizeof(regs[0].sl));
        memcpy(regs[0].sh, regs[0].dh, sizeof(regs[0].sh));
        break;
    case SWS_UOP_UNPACK:
        break;
    case SWS_UOP_PACK:
        break;
    case SWS_UOP_TO_U8:
    case SWS_UOP_TO_U16:
    case SWS_UOP_TO_U32:
    case SWS_UOP_TO_F32: {

        memcpy(regs[0].dl, regs[1].sl, sizeof(regs[0].dl));
        memcpy(regs[0].dh, regs[1].sh, sizeof(regs[0].dh));
        memcpy(regs[0].sl, regs[0].dl, sizeof(regs[0].sl));
        memcpy(regs[0].sh, regs[0].dh, sizeof(regs[0].sh));

        size_t src_el_size = s->el_size;
        SwsPixelType to_type;
        switch (p->uop) {
        case SWS_UOP_TO_U8:  to_type = SWS_PIXEL_U8;  break;
        case SWS_UOP_TO_U16: to_type = SWS_PIXEL_U16; break;
        case SWS_UOP_TO_U32: to_type = SWS_PIXEL_U32; break;
        case SWS_UOP_TO_F32: to_type = SWS_PIXEL_F32; break;
        default:
            // av_assert0(!"Invalid uop!");
            break;
        }
        size_t dst_el_size = ff_sws_pixel_type_size(to_type);

        printf("[%s][%d] %s() blocksize %d src %d dst %d\n", __FILE__, __LINE__, __func__, (int) p->block_size, (int) src_el_size, (int) dst_el_size);

        if (p->block_size == 8) {
            if        (src_el_size == 1 && dst_el_size == 2) {
                /* 8 -> 16 */
            } else if (src_el_size == 1 && dst_el_size == 4) {
                /* 8 -> 32 */
                LOOP_MASK(p, i) { a64frame_vec_free(&s->vframe, regs[0].sh[i]); regs[0].sh[i] = rasm_op_none(); }
            } else if (src_el_size == 2 && dst_el_size == 1) {
                /* 16 -> 8 */
            } else if (src_el_size == 2 && dst_el_size == 4) {
                /* 16 -> 32 */
                LOOP_MASK(p, i) { a64frame_vec_free(&s->vframe, regs[0].sh[i]); regs[0].sh[i] = rasm_op_none(); }
            } else if (src_el_size == 4 && dst_el_size == 1) {
                /* 32 -> 8 */
                LOOP_MASK(p, i) { regs[0].sh[i] = a64frame_vec(&s->vframe, -1); }
            } else if (src_el_size == 4 && dst_el_size == 2) {
                /* 32 -> 16 */
                LOOP_MASK(p, i) { regs[0].sh[i] = a64frame_vec(&s->vframe, -1); }
            }
        } else /* if (p->block_size == 16) */ {
            if        (src_el_size == 1 && dst_el_size == 2) {
                /* 16 -> 32 */
                LOOP_MASK(p, i) { a64frame_vec_free(&s->vframe, regs[0].sh[i]); regs[0].sh[i] = rasm_op_none(); }
            } else if (src_el_size == 2 && dst_el_size == 1) {
                /* 32 -> 16 */
                LOOP_MASK(p, i) { regs[0].sh[i] = a64frame_vec(&s->vframe, -1); }
            }
        }

        break;
    }
    case SWS_UOP_EXPAND_PAIR:
        break;
    case SWS_UOP_EXPAND_QUAD:
        break;
    case SWS_UOP_LINEAR:
    case SWS_UOP_LINEAR_FMA:
        // TODO wrong
        memcpy(regs[0].dl, regs[1].sl, sizeof(regs[0].dl));
        memcpy(regs[0].dh, regs[1].sh, sizeof(regs[0].dh));
        memcpy(regs[0].sl, regs[0].dl, sizeof(regs[0].sl));
        memcpy(regs[0].sh, regs[0].dh, sizeof(regs[0].sh));
        break;
    case SWS_UOP_DITHER:
        memcpy(regs[0].dl, regs[1].sl, sizeof(regs[0].dl));
        memcpy(regs[0].dh, regs[1].sh, sizeof(regs[0].dh));
        memcpy(regs[0].sl, regs[0].dl, sizeof(regs[0].sl));
        memcpy(regs[0].sh, regs[0].dh, sizeof(regs[0].sh));
        break;
    default:
        // TODO av_assert(0);
        break;
    }

    print_regs(p, regs);

    /* Set up constants. */
    switch (p->uop) {
    case SWS_UOP_READ_BIT:
        regs->vk[0] = jit_push_v128(s, res->priv.data);     /* shift_vec */
        regs->vk[1] = jit_push_imm(s, SWS_PIXEL_U8, 1);     /* bitmask_vec */
        break;
    case SWS_UOP_READ_NIBBLE:
        regs->vk[0] = jit_push_imm(s, SWS_PIXEL_U8, 0x0f);  /* nibble_mask */
        break;
    case SWS_UOP_WRITE_BIT:
        regs->vk[0] = jit_push_v128(s, res->priv.data); /* shift_vec */
        break;
    case SWS_UOP_UNPACK:
        LOOP_MASK(p, i) {
            uint32_t val = (1u << p->par.pack.pattern[i]) - 1;
            regs->vk[i] = jit_push_imm(s, p->type, val);
        }
        break;
    case SWS_UOP_CLEAR:
        /* TODO load directly to output */
        break;
    case SWS_UOP_MIN:
    case SWS_UOP_MAX:
        LOOP_MASK(p, i) {
            uint32_t val = (p->type == SWS_PIXEL_U8)  ? res->priv.u8[i]
                         : (p->type == SWS_PIXEL_U16) ? res->priv.u16[i]
                         :                              res->priv.u32[i];
            regs->vk[i] = jit_push_imm(s, p->type, val);
        }
        break;
    case SWS_UOP_SCALE: {
         uint32_t val = (p->type == SWS_PIXEL_U8)  ? res->priv.u8[0]
                      : (p->type == SWS_PIXEL_U16) ? res->priv.u16[0]
                      :                              res->priv.u32[0];
        regs->vk[0] = jit_push_imm(s, p->type, val);   /* scale_vec */
        break;
    }
    case SWS_UOP_LINEAR:
    case SWS_UOP_LINEAR_FMA:
#if 0
    RasmContext *r = s->rctx;
    RasmOp *vc = regs->vk;

    RasmOp ptr = s->tmp0;
    RasmOp coeff_veclist;

    /* Preload coefficients from impl->priv. */
    const int num_vregs = linear_num_vregs(p);
    av_assert0(num_vregs <= 4);
    switch (num_vregs) {
    case 1: coeff_veclist = vv_1(vc[0]);                      break;
    case 2: coeff_veclist = vv_2(vc[0], vc[1]);               break;
    case 3: coeff_veclist = vv_3(vc[0], vc[1], vc[2]);        break;
    case 4: coeff_veclist = vv_4(vc[0], vc[1], vc[2], vc[3]); break;
    }
    i_ldr(r, ptr, s->impl_priv);                            CMT("v128 *vcoeff_ptr = impl->priv.ptr;");
    asmgen_set_load_cont_node(s);
    i_ld1(r, coeff_veclist, a64op_base(ptr));               CMT("coeff_veclist = *vcoeff_ptr;");
#endif
        break;
    case SWS_UOP_DITHER:
#if 0
    RasmContext *r = s->rctx;
    RasmOp src_ptr = s->tmp0;

    i_ldr(r, src_ptr, s->impl_priv);                        CMT("void *ptr = impl->priv.ptr;");
    asmgen_set_load_cont_node(s);
#endif
        break;
    default:
        break;
    }
#if 0
    switch (p->uop) {
    case SWS_UOP_CLEAR: {
        break;
    }
    case SWS_UOP_LINEAR:
    case SWS_UOP_LINEAR_FMA: {
        const int num_vregs = linear_num_vregs(p);
        av_assert0(num_vregs <= 4);
        regs->linear.num_vregs = num_vregs;
        const float *coeffs = (const float *) res->priv.ptr;
        for (int vi = 0; vi < num_vregs; vi++) {
            const uint32_t *words = (const uint32_t *) &coeffs[vi * 4];
            int idx = jit_push_data(s, words);
            regs->linear.coeff[vi] = a64op_vec4s(SWS_AARCH64_REGID_VDATA + idx);
        }
        res->free(&res->priv);
        break;
    }
    case SWS_UOP_DITHER: {
        s->chain->impl[n].priv = res->priv;
        s->chain->free[n] = res->free;
        s->chain->num_impl = FFMAX(s->chain->num_impl, n + 1);
        regs->dither.offset = n * sizeof_impl + offsetof_impl_priv;
        break;
    }
    default:
        break;
    }
#endif
    return 0;
}

/*********************************************************************/
static void asmgen_common_frame(SwsAArch64Context *s, SwsCompMask imask, SwsCompMask omask)
{
    AArch64Frame *f = &s->frame;

    /* Loop iterator variables. */
    s->bx        = a64frame_gpw(f, 6);
    s->y         = a64frame_gpw(f, 3);  /* Reused from SwsOpFunc.y_start argument. */

    /* Scratch registers. */
    s->tmp0      = a64frame_gpx(f, 16); /* IP0 */
    s->tmp1      = a64frame_gpx(f, 17); /* IP1 */
}

static void asmgen_process_frame(SwsAArch64Context *s, SwsCompMask imask, SwsCompMask omask)
{
    AArch64Frame *f = &s->frame;

    asmgen_common_frame(s, imask, omask);

    /* SwsOpFunc arguments. */
    s->exec      = a64frame_argx(f, 0); // const SwsOpExec *exec
    s->impl      = a64frame_argx(f, 1); // const void *priv
    s->bx_start  = a64frame_argw(f, 2); // int bx_start
    s->y_start   = a64frame_argw(f, 3); // int y_start
    s->bx_end    = a64frame_argw(f, 4); // int bx_end
    s->y_end     = a64frame_argw(f, 5); // int y_end
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

    *out = (SwsCompiledOp) {
        .priv        = NULL /* chain */, /* TODO */
        .slice_align = 1,
        .free        = NULL /* ff_sws_op_chain_free_cb */, /* TODO */
        .block_size  = block_size,
    };

#if 1
    RasmContext *r = rasm_alloc();
    if (!r)
        return AVERROR(ENOMEM); // TODO check

    SwsAArch64Context s = {
        .sws        = ctx,
        .block_size = block_size,
        .rctx       = r,
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

    const SwsOp *read      = ff_sws_op_list_input(ops);
    const SwsOp *write     = ff_sws_op_list_output(ops);
    const int read_planes  = read ? ff_sws_rw_op_planes(read) : 0;
    const int write_planes = ff_sws_rw_op_planes(write);
    SwsCompMask imask = SWS_COMP_MASK(read_planes > 0,  read_planes > 1,  read_planes > 2,  read_planes > 3);
    SwsCompMask omask = SWS_COMP_MASK(write_planes > 0, write_planes > 1, write_planes > 2, write_planes > 3);

    asmgen_process_frame(&s, imask, omask);

    /* Allocate GPRs backwards */
    for (int i = ops->num_ops - 1; i >= 0; i--) {
        ret = aarch64_jit_setup(&s, params, res, regs, i, imask, omask);
        if (ret < 0)
            goto error;
    }

    /* create process */
    ret = aarch64_jit_process(&s, ops, imask, omask);
    if (ret < 0)
        goto error;

#if 0
    for (int i = 0; i < ops->num_ops; i++) {
        print_regs(params, regs, i);
    }
#endif

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
    /* emit data pool and immediates */
    if (s.data_count > 0 || s.imm_count > 0) {
        rasm_set_current_node(r, s.setup);
        load_constants(&s);
    }

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
