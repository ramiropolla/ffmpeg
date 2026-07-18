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
#include "ops_asmgen.c"

/*********************************************************************/
/* Immediates. */

static RasmOp jit_push_v128(SwsAArch64Context *s, void *val, int *out_idx);

/* Whether `val` can be built with a single 32-bit `mov` (the movz/movn
 * aliases -- one 16-bit half set/cleared, the other half all-zero/
 * all-one). Deliberately conservative: it does NOT check the full
 * AArch64 logical/bitmask-immediate encoding (a rotated run of 1s at a
 * power-of-2 element size), which `mov` can also alias to `orr` for --
 * that rotate+run-length math is easy to get subtly wrong by hand, and
 * every value this rejects still gets built correctly via the data
 * pool below, just with one spare register/instruction instead of the
 * cleverer encoding. What this must never do is fall through to a
 * movz+movk pair -- some assemblers auto-expand `mov` with an arbitrary
 * immediate into exactly that, which is the one thing callers here are
 * explicitly avoiding. */
static bool mov_imm_fits_single_insn(uint32_t val)
{
    if ((val & 0xffff0000u) == 0 || (val & 0x0000ffffu) == 0)
        return true;
    uint32_t inv = ~val;
    if ((inv & 0xffff0000u) == 0 || (inv & 0x0000ffffu) == 0)
        return true;
    return false;
}

/* Whether `val`, broadcast to all lanes of a vector, can be built with
 * a single `movi` -- cheaper than mov_imm_fits_single_insn()'s mov+dup
 * (no GPR touched at all). Deliberately narrow: rasm's `i_movi` has no
 * operand slot for AArch64's "lsl #n"/"msl #n" shift qualifier (every
 * existing i_movi() callsite in this codebase only ever uses the
 * implicit-LSL#0 form), so this only tries the three shapes reachable
 * without one: byte-replicate (.16b), and a single byte in the low
 * position with the rest of the halfword/word zeroed (.8h / .4s). Real
 * `movi` can also reach shifted-byte, MSL, and per-byte-0/ff .2d
 * patterns that this rejects -- those fall through to
 * mov_imm_fits_single_insn() or the data pool instead, same as any
 * other value this doesn't recognize. */
static bool can_movi_broadcast(uint32_t val)
{
    uint8_t b0 = (val      ) & 0xff;
    uint8_t b1 = (val >>  8) & 0xff;
    uint8_t b2 = (val >> 16) & 0xff;
    uint8_t b3 = (val >> 24) & 0xff;

    return (b0 == b1 && b1 == b2 && b2 == b3) /* .16b, imm8 = b0 */
        || (b1 == 0  && b3 == 0  && b0 == b2) /* .8h,  imm8 = b0, lsl #0 */
        || (b1 == 0  && b2 == 0  && b3 == 0); /* .4s,  imm8 = b0, lsl #0 */
}

/* Emits the movi chosen by can_movi_broadcast() -- caller must have
 * already confirmed it returns true for `val`. */
static void emit_movi_broadcast(RasmContext *r, RasmOp target, uint32_t val)
{
    uint8_t b0 = (val      ) & 0xff;
    uint8_t b1 = (val >>  8) & 0xff;
    uint8_t b2 = (val >> 16) & 0xff;
    uint8_t b3 = (val >> 24) & 0xff;

    if (b0 == b1 && b1 == b2 && b2 == b3)
        i_movi(r, v_16b(target), IMM(b0));
    else if (b1 == 0 && b3 == 0 && b0 == b2)
        i_movi(r, v_8h(target), IMM(b0));
    else
        i_movi(r, v_4s(target), IMM(b0)); /* b1==b2==b3==0, guaranteed by can_movi_broadcast() */
}

/* Emit load instructions for all collected immediates (v27..v31),
 * hoisted CLEAR values, and 128-bit data pool vectors (v8..v15) before
 * the inner loop. Uses tmp0 as a scratch GPR to build each immediate
 * and as the base pointer for adr + ldr of the data pool.
 *
 * Priority per value: a single `movi` when the shape allows it (no GPR
 * touched at all); otherwise a single `mov`+`dup` (no data pool slot
 * spent); otherwise routed through the const data pool -- broadcast
 * once as a 16-byte {val,val,val,val} entry via jit_push_v128() (this
 * must happen before the "data pool" section below builds/finalizes
 * its packed buffer, so every such push is done up front) and read
 * back into its real target register from there. Never a movk
 * sequence either way. */
static void load_constants(SwsAArch64Context *s)
{
    RasmContext *r = s->rctx;

    /* (target register, data-pool index) pairs for immediates/CLEAR
     * values that didn't fit a single mov -- their real ldr (into
     * `target`, not jit_push_v128()'s own throwaway auto-picked
     * register) has to wait until the "data pool" section below has
     * computed `ptr`. */
    struct { RasmOp target; int idx; } pool_load[SWS_AARCH64_MAX_IMM + SWS_AARCH64_MAX_CLEAR_HOIST];
    int pool_load_count = 0;

    for (int i = 0; i < s->imm_count; i++) {
        if (can_movi_broadcast(s->imm[i].val) || mov_imm_fits_single_insn(s->imm[i].val))
            continue;
        uint32_t words[4] = { s->imm[i].val, s->imm[i].val, s->imm[i].val, s->imm[i].val };
        pool_load[pool_load_count].target = s->imm[i].op;
        jit_push_v128(s, words, &pool_load[pool_load_count].idx);
        pool_load_count++;
    }
    for (int i = 0; i < s->clear_hoist_count; i++) {
        if (can_movi_broadcast(s->clear_hoist[i].val) || mov_imm_fits_single_insn(s->clear_hoist[i].val))
            continue;
        uint32_t val = s->clear_hoist[i].val;
        uint32_t words[4] = { val, val, val, val };
        pool_load[pool_load_count].target = s->clear_hoist[i].target;
        jit_push_v128(s, words, &pool_load[pool_load_count].idx);
        pool_load_count++;
    }

    if (s->imm_count) {
        rasm_add_comment(r, "immediates");

        RasmOp tmp = a64op_w(s->tmp0);
        for (int i = 0; i < s->imm_count; i++) {
            if (can_movi_broadcast(s->imm[i].val)) {
                emit_movi_broadcast(r, s->imm[i].op, s->imm[i].val);
                continue;
            }
            if (!mov_imm_fits_single_insn(s->imm[i].val))
                continue; /* loaded from the data pool below instead */
            i_mov(r, tmp, IMM((int32_t) s->imm[i].val));
            i_dup(r, v_4s(s->imm[i].op), tmp);
        }
    }

    if (s->clear_hoist_count) {
        /* CLEAR (Task 5): each entry's target is whatever register the
         * consumer already expects -- e.g. one lane of a WRITE_PACKED
         * st4's contiguous block -- not a shared pool slot, so (unlike
         * immediates above) there's no dedup and every entry gets its
         * own movi, or mov + dup, or data-pool load. */
        rasm_add_comment(r, "clear hoist");

        RasmOp tmp = a64op_w(s->tmp0);
        for (int i = 0; i < s->clear_hoist_count; i++) {
            if (can_movi_broadcast(s->clear_hoist[i].val)) {
                emit_movi_broadcast(r, s->clear_hoist[i].target, s->clear_hoist[i].val);
                continue;
            }
            if (!mov_imm_fits_single_insn(s->clear_hoist[i].val))
                continue; /* loaded from the data pool below instead */
            i_mov(r, tmp, IMM((int32_t) s->clear_hoist[i].val));
            i_dup(r, v_4s(s->clear_hoist[i].target), tmp);
        }
    }

    if (s->data_count) {
        rasm_add_comment(r, "data pool");

        /* s->data[] isn't itself a tightly packed array of 16-byte
         * vectors (SwsAArch64ConstVec also carries a RasmOp field), so
         * pack just the value bytes into a temporary contiguous buffer
         * before handing it to rasm_add_data() (which memdup()s it
         * immediately, so it doesn't need to outlive this call). */
        uint8_t packed[SWS_AARCH64_MAX_DATA_VECS][16];
        for (int i = 0; i < s->data_count; i++)
            memcpy(packed[i], s->data[i].val, 16);

        /* Creating the const entry switches the current node away from
         * the function body, so save/restore it around it. */
        RasmNode *saved_node = rasm_get_current_node(r);
        int data_label = rasm_const_begin(r, "ldata");
        rasm_add_data(r, packed, s->data_count * 4, RASM_DATA_WORD);
        rasm_set_current_node(r, saved_node);

        RasmOp ptr = s->tmp0;
        i_adr(r, ptr, rasm_op_label(data_label));
        for (int i = 0; i < s->data_count; i++)
            i_ldr(r, v_q(s->data[i].op), a64op_off(ptr, (int16_t) (i * 16)));

        /* Immediates/CLEAR values routed through the pool above (not
         * every jit_push_v128() caller -- LINEAR/READ_BIT/etc already
         * read their own s->data[i].op directly) also land in their
         * real target register here, from the same pool/ptr. */
        for (int i = 0; i < pool_load_count; i++) {
            i_ldr(r, v_q(pool_load[i].target),
                  a64op_off(ptr, (int16_t) (pool_load[i].idx * 16)));
        }

        /* DITHER (Task 6): the matrix pointer piggybacks on this same
         * pool entry (see jit_push_dither_ptr()) -- also read it back
         * as a scalar (GPR) into the persistent dither_src_ptr, from
         * the exact same pool offset the vector load above just used. */
        if (s->dither_data_idx >= 0) {
            i_ldr(r, s->dither_src_ptr,
                  a64op_off(ptr, (int16_t) (s->dither_data_idx * 16)));
        }
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
        /* Hoisted entirely into the one-time setup section (Task 5,
         * see aarch64_jit_setup_constants() and load_constants()) --
         * no per-iteration instruction needed,
         * asmgen_op_clear()/emit_clear() (which read a runtime-loaded
         * vk[0] that JIT never sets up for CLEAR) must not run here. */
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
/* `out_idx` (may be NULL) receives the s->data[] index this value
 * landed in -- most callers just want the register and don't care, but
 * a couple (the pool-fallback path in load_constants(), and
 * jit_push_dither_ptr() below) need to read the same pool entry back a
 * second time from a different register, once load_constants() has
 * computed the pool's base pointer, and can no longer just subtract a
 * fixed base offset from the returned register number now that it's
 * auto-picked instead of living at a fixed v8+idx slot. */
static RasmOp jit_push_v128(SwsAArch64Context *s, void *val, int *out_idx)
{
    /* Check whether we already have it */
    for (int i = 0; i < s->data_count; i++) {
        if (!memcmp(&s->data[i].val, val, 16)) {
            if (out_idx)
                *out_idx = i;
            return s->data[i].op;
        }
    }

    /* Add it to our data and create a new vector -- auto-picked from
     * whatever the whole-chain value-flow ("banks") pass, which has
     * already finished for every op by the time any constant is ever
     * pushed, left free. */
    av_assert0(s->data_count < SWS_AARCH64_MAX_DATA_VECS);
    int idx = s->data_count++;
    memcpy(s->data[idx].val, val, 16);
    s->data[idx].op = a64reg_vec(&s->regstate, -1);

    if (out_idx)
        *out_idx = idx;
    return s->data[idx].op;
}

/* ff_sws_op_chain_append() hard-asserts a non-NULL func (av_assert1),
 * since CPS jumps through chain->impl[].cont as its continuation-
 * passing mechanism. JIT is one fused function and never reads that
 * field -- this exists purely to satisfy the assert. */
static void jit_op_chain_dummy_func(void)
{
}

/* DITHER (Task 6): the matrix pointer is a compile-time-known 64-bit
 * address, too small to need its own const section but with no
 * dedicated 64-bit-immediate instruction sequence available in rasm.h
 * (no movz/movk helper) -- reuse jit_push_v128()'s adr+ldr data pool
 * mechanism (pointer in the low 8 bytes, zero-padded) instead of
 * inventing a second one, and remember which entry it landed in so
 * load_constants() can also read it back with a scalar (GPR) ldr, not
 * just the vector one every other data-pool entry gets. */
static void jit_push_dither_ptr(SwsAArch64Context *s, void *ptr)
{
    uint8_t padded[16] = { 0 };
    memcpy(padded, &ptr, sizeof(ptr));
    jit_push_v128(s, padded, &s->dither_data_idx);
}

/* Replicate a value up to a full 32-bit lane, based on its logical
 * element width -- e.g. a u8 value 0xab becomes 0xabababab, ready to
 * broadcast into any vector width via a plain 4S dup. */
static uint32_t expand_to_u32(SwsPixelType type, uint32_t val)
{
    switch (type) {
    case SWS_PIXEL_U8:
        val = val | (val <<  8);
        av_fallthrough;
    case SWS_PIXEL_U16:
        val = val | (val << 16);
        break;
    }
    return val;
}

static RasmOp jit_push_imm(SwsAArch64Context *s, SwsPixelType type, uint32_t val)
{
    val = expand_to_u32(type, val);

    /* Check whether we already have it */
    for (int i = 0; i < s->imm_count; i++) {
        if (s->imm[i].val == val)
            return s->imm[i].op;
    }

    /* Add it to our data and create a new vector -- auto-picked, same
     * reasoning as jit_push_v128() above. */
    av_assert0(s->imm_count < SWS_AARCH64_MAX_IMM);
    int idx = s->imm_count++;
    s->imm[idx].val = val;
    s->imm[idx].op  = a64reg_vec(&s->regstate, -1);

    return s->imm[idx].op;
}

/* CLEAR (Task 5): unlike jit_push_imm()/jit_push_v128(), the value must
 * land in a *specific* register (whatever the consumer already
 * expects -- see the SwsAArch64ClearHoist comment in ops_asmgen.h for
 * why), not a shared, deduped constant-pool slot. */
static void jit_push_clear_hoist(SwsAArch64Context *s, RasmOp target,
                                 SwsPixelType type, uint32_t val)
{
    if (rasm_op_type(target) != AARCH64_OP_VEC)
        return;

    av_assert0(s->clear_hoist_count < SWS_AARCH64_MAX_CLEAR_HOIST);
    int idx = s->clear_hoist_count++;
    s->clear_hoist[idx].target = target;
    s->clear_hoist[idx].val    = expand_to_u32(type, val);
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

/* Pass 1 of 2 (backward, over every op): decide value-flow registers
 * (sl/sh/dl/dh) only. Constants are deliberately NOT decided here
 * anymore -- see aarch64_jit_setup_constants() below for why. */
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

/* Pass 2 of 2 (order doesn't matter -- forward for simplicity): decide
 * constants. Runs as a genuinely separate loop over every op, called
 * only after aarch64_jit_setup_banks() has completed for the *entire*
 * chain (see aarch64_jit_compile()) -- so two things are guaranteed
 * true here that weren't true when this used to be interleaved into
 * the same per-op call as banks:
 *   1. Every op's dl/sl/sh/dh are already fully decided, so CLEAR's
 *      hoisting (which needs regs->dl[i], a banks decision) always
 *      finds it ready, regardless of this op's position in the chain.
 *   2. s->regstate's "used" bitmap reflects *exactly* what value-flow
 *      needs -- nothing more -- so jit_push_imm()/jit_push_v128()'s
 *      auto-picked registers (a64reg_vec(rs, -1)) only ever claim a
 *      register value-flow genuinely isn't using, instead of a fixed
 *      range reserved unconditionally regardless of actual usage. */

static uint32_t get_priv(const SwsOpPriv *priv, SwsPixelType type, int i)
{
    return (type == SWS_PIXEL_U8)  ? priv->u8[i]
         : (type == SWS_PIXEL_U16) ? priv->u16[i]
         :                           priv->u32[i];
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
        regs->vk[0] = jit_push_v128(s, res->priv.data, NULL);   /* shift_vec */
        regs->vk[1] = jit_push_imm(s, SWS_PIXEL_U8, 1);         /* bitmask_vec */
        break;
    case SWS_UOP_READ_NIBBLE:
        regs->vk[0] = jit_push_imm(s, SWS_PIXEL_U8, 0x0f);      /* nibble_mask */
        break;
    case SWS_UOP_WRITE_BIT:
        regs->vk[0] = jit_push_v128(s, res->priv.data, NULL);   /* shift_vec */
        break;
    case SWS_UOP_UNPACK:
        LOOP_MASK(p, i) {
            uint32_t val = (1u << p->par.pack.pattern[i]) - 1;
            regs->vk[i] = jit_push_imm(s, p->type, val);
        }
        break;
    case SWS_UOP_CLEAR: {
        /* Compile-time-known per component, same three cases
         * emit_clear() (ops_asmgen.c, CPS) tells apart at runtime --
         * matches ops_impl_conv.c's SWS_UOP_CLEAR translation, which
         * only ever sets .one for the type's exact all-ones pattern. */
        uint32_t maxval = (p->type == SWS_PIXEL_U8)  ? UINT8_MAX
                        : (p->type == SWS_PIXEL_U16) ? UINT16_MAX
                        :                              UINT32_MAX;
        LOOP_MASK(p, i) {
            uint32_t val = (p->par.clear.zero & SWS_COMP(i)) ? 0
                         : (p->par.clear.one  & SWS_COMP(i)) ? maxval
                         : (p->type == SWS_PIXEL_U8)  ? res->priv.u8[i]
                         : (p->type == SWS_PIXEL_U16) ? res->priv.u16[i]
                         :                              res->priv.u32[i]; /* U32 or F32: same union offset */
            jit_push_clear_hoist(s, regs->dl[i], p->type, val);
            if (s->use_vh)
                jit_push_clear_hoist(s, regs->dh[i], p->type, val);
        }
        break;
    }
    case SWS_UOP_MIN:
    case SWS_UOP_MAX:
        LOOP_MASK(p, i) {
            uint32_t val = get_priv(&res->priv, p->type, i);
            regs->vk[i] = jit_push_imm(s, p->type, val);
        }
        break;
    case SWS_UOP_SCALE: {
        uint32_t val = get_priv(&res->priv, p->type, 0);
        regs->vk[0] = jit_push_imm(s, p->type, val);   /* scale_vec */
        break;
    }
    case SWS_UOP_LINEAR:
    case SWS_UOP_LINEAR_FMA: {
        /* Coefficients are compile-time-known (computed by
         * aarch64_setup_linear(), shared with CPS, into a malloc'd
         * float array at res->priv.ptr) -- push each packed group of 4
         * into the constant data pool directly instead of CPS's
         * runtime impl->priv load, then free the now-fully-copied
         * malloc'd array immediately. */
        const int num_vregs = linear_num_vregs(p);
        av_assert0(num_vregs <= 4);
        const float *coeffs = (const float *) res->priv.ptr;
        for (int vi = 0; vi < num_vregs; vi++)
            regs->vk[vi] = jit_push_v128(s, (void *) &coeffs[vi * 4], NULL);
        res->free(&res->priv);
        break;
    }
    case SWS_UOP_DITHER: {
        /* The matrix is too large to inline as compile-time data (up to
         * (16+15)*16*4 bytes) -- unlike LINEAR's coefficients, the
         * malloc'd buffer (res->priv.ptr) must stay alive and get read
         * through a pointer every call, so it must NOT be freed here.
         * Ownership instead transfers to s->chain, which frees it later
         * via ff_sws_op_chain_free_cb() when the compiled op itself is
         * torn down (see aarch64_jit_compile()). */
        int ret = ff_sws_op_chain_append(s->chain, jit_op_chain_dummy_func,
                                         res->free, &res->priv);
        if (ret < 0)
            return ret;
        jit_push_dither_ptr(s, res->priv.ptr);
        break;
    }
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

    /* Persistent GPR for DITHER's runtime matrix pointer (Task 6) --
     * loaded once, before the loop, unlike CPS's per-call tmp0 reload.
     * x20/x21 stay spare (reserved, unused) for now. */
    s->dither_src_ptr = a64reg_gpx(rs, 19);
    a64reg_gpx(rs, 20);
    a64reg_gpx(rs, 21);

    /* No upfront vector-register reservation for constants anymore --
     * jit_push_imm()/jit_push_v128() auto-pick a register each *after*
     * the whole-chain "banks" (value-flow) pass has finished for every
     * op (see aarch64_jit_setup_constants()), so they only ever claim
     * what value-flow isn't using. A fixed range reserved here,
     * unconditionally, regardless of how many constants a given chain
     * actually needs, was real waste that could exhaust the register
     * file on wider chains (e.g. -src yuva444p -dst ayuv64le) even
     * though most of the 13 reserved registers went unused. */
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
        .chain           = chain,
        .dither_data_idx = -1,
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

    /* Pass 2: constants, over every op -- only now, with pass 1 fully
     * decided for the *entire* chain, so jit_push_imm()/jit_push_v128()
     * auto-pick registers value-flow genuinely isn't using instead of
     * reserving a fixed range upfront regardless of actual usage (see
     * aarch64_jit_setup_constants()'s own comment for the full reasoning). */
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
    /* Emit data pool, immediates, and hoisted CLEAR values -- always
     * (not gated on any specific count): load_constants() already
     * no-ops each section it finds empty, and gating on an explicit
     * list of counts here has already once silently dropped a whole
     * category (clear_hoist_count wasn't in this condition when Task 5
     * added it, so CLEAR-only chains never actually got their hoisted
     * mov+dup emitted at all -- caught while wiring up Task 6). */
    rasm_set_current_node(r, s.setup);
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
