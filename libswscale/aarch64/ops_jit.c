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

/*********************************************************************/
typedef struct SwsAArch64JITBackendContext {
    SwsContext *sws;
    int block_size;
} SwsAArch64JITBackendContext;

/*********************************************************************/
/* Emit JIT code. */
#define AARCH64_ASMGEN_JIT
#include "ops_asmgen.c"

/*********************************************************************/
/* Immediates. */

static int jit_push_imm(SwsAArch64Context *s, uint32_t val, int len)
{
    /* First check if we already have it */
    for (int i = 0; i < s->imm_count; i++) {
        if (s->imm[i].val == val) {
            return i;
        }
    }

    /* Check if data can be represented by repeating smaller value */
    int repeat_len;
    int small_value;
    union {
        uint32_t u32;
        uint16_t u16[2];
        uint8_t  u8[4];
    } u;
    u.u32 = val;
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
    int ret = s->imm_count;
    s->imm[s->imm_count++] = (SwsAArch64Immediate) {
        .op          = jit_vec(s, -1),
        .val         = val,
        .len         = len,
        .repeat_len  = repeat_len,
        .small_value = small_value,
    };

    return ret;
}

static int jit_push_imm32(SwsAArch64Context *s, uint32_t val)
{
    return jit_push_imm(s, val, 4);
}

static int jit_push_imm16(SwsAArch64Context *s, uint16_t val)
{
    uint32_t v32 = val | ((uint32_t) val << 16);
    return jit_push_imm(s, v32, 2);
}

static int jit_push_imm8(SwsAArch64Context *s, uint8_t val)
{
    uint16_t v16 = val | ((uint16_t) val <<  8);
    uint32_t v32 = v16 | ((uint32_t) v16 << 16);
    return jit_push_imm(s, v32, 1);
}

static int jit_push_imm32_op(SwsAArch64Context *s, SwsPixelType type, uint32_t val)
{
    if (type == SWS_PIXEL_U8)
        return jit_push_imm8(s, val);
    if (type == SWS_PIXEL_U16)
        return jit_push_imm16(s, val);
    return jit_push_imm32(s, val);
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

    if (s->n_data) {
        /* Immediates above may have lazily grown the data pool (the
         * via_pool fallback just above), so the "ldata" label and its
         * contents can only be finalized now, with the final s->n_data.
         * Creating it switches the current node away from the function
         * body, so save/restore it around the const entry. */
        RasmNode *saved_node = rasm_get_current_node(r);
        int data_label = rasm_const_begin(r, "ldata");
        rasm_add_data(r, s->data, s->n_data * 4, RASM_DATA_WORD);
        rasm_set_current_node(r, saved_node);

        rasm_add_comment(r, "data pool");

        RasmOp ptr = s->tmp0;
        i_adr(r, ptr, rasm_op_label(data_label));
        for (int i = 0; i < s->n_data; i++)
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

/*********************************************************************/
/* Unified setup pass: collect all immediates and data pool entries
 * needed by one op, and fill in the pre-allocated register assignments
 * in *regs.  Called before () so that
 * load_constants() can pre-load all constants before the inner loop. */
static int aarch64_setup(SwsAArch64Context *s, const SwsOpList *ops, int n,
                         const SwsAArch64OpImplParams *p, SwsAArch64OpRegs *regs)
{
    SwsImplResult impl_result = { 0 };

    switch (p->uop) {
    case SWS_UOP_READ_BIT: {
        int bitmask_idx = jit_push_imm8(s, 1);
        regs->read_bit.bitmask = s->vimm[bitmask_idx];
        int ret = ff_sws_aarch64_setup(ops, s->block_size, n, p, &impl_result);
        if (ret < 0)
            return ret;
        int idx = jit_push_data(s, impl_result.priv.u32);
        regs->read_bit.shift_vec = s->vdata[idx];
        break;
    }
    case SWS_UOP_READ_NIBBLE: {
        int nibble_idx = jit_push_imm8(s, 0x0f);
        regs->read_nibble.nibble_mask = s->vimm[nibble_idx];
        break;
    }
    case SWS_UOP_WRITE_BIT: {
        int ret = ff_sws_aarch64_setup(ops, s->block_size, n, p, &impl_result);
        if (ret < 0)
            return ret;
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
            int ret = ff_sws_aarch64_setup(ops, s->block_size, n, p, &impl_result);
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
        int ret = ff_sws_aarch64_setup(ops, s->block_size, n, p, &impl_result);
        if (ret < 0)
            return ret;
        LOOP_MASK(p, i) {
            uint32_t val = (p->type == SWS_PIXEL_U8)  ? impl_result.priv.u8[i]
                         : (p->type == SWS_PIXEL_U16) ? impl_result.priv.u16[i]
                         :                              impl_result.priv.u32[i];
            int idx = jit_push_imm32_op(s, p->type, val);
            regs->min.vec[i] = s->vimm[idx];
        }
        break;
    }
    case SWS_UOP_MAX: {
        int ret = ff_sws_aarch64_setup(ops, s->block_size, n, p, &impl_result);
        if (ret < 0)
            return ret;
        LOOP_MASK(p, i) {
            uint32_t val = (p->type == SWS_PIXEL_U8)  ? impl_result.priv.u8[i]
                         : (p->type == SWS_PIXEL_U16) ? impl_result.priv.u16[i]
                         :                              impl_result.priv.u32[i];
            int idx = jit_push_imm32_op(s, p->type, val);
            regs->max.vec[i] = s->vimm[idx];
        }
        break;
    }
    case SWS_UOP_SCALE: {
        int ret = ff_sws_aarch64_setup(ops, s->block_size, n, p, &impl_result);
        if (ret < 0)
            return ret;
        int idx = jit_push_imm32_op(s, p->type, impl_result.priv.u32[0]);
        regs->scale.vec = s->vimm[idx];
        break;
    }
    case SWS_UOP_LINEAR:
    case SWS_UOP_LINEAR_FMA: {
        int ret = ff_sws_aarch64_setup(ops, s->block_size, n, p, &impl_result);
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
    case SWS_UOP_DITHER: {
        int ret = ff_sws_aarch64_setup(ops, s->block_size, n, p, &impl_result);
        if (ret < 0)
            return ret;
        s->chain->impl[n].priv = impl_result.priv;
        s->chain->free[n] = impl_result.free;
        s->chain->num_impl = FFMAX(s->chain->num_impl, n + 1);
        regs->dither.offset = n * sizeof_impl + offsetof_impl_priv;
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
    SwsAArch64JITBackendContext bctx;
    int ret;

    const int cpu_flags = av_get_cpu_flags();
    if (!(cpu_flags & AV_CPU_FLAG_NEON))
        return AVERROR(ENOTSUP);

    /* Use at most two full vregs during the widest precision section */
    bctx.block_size = (ff_sws_op_list_max_size(ops) == 4) ? 8 : 16;
    bctx.sws = ctx;

    SwsOpChain *chain = ff_sws_op_chain_alloc();
    if (!chain)
        return AVERROR(ENOMEM);
    chain->cpu_flags = AV_CPU_FLAG_NEON;

    *out = (SwsCompiledOp) {
        .priv        = chain,
        .slice_align = 1,
        .free        = ff_sws_op_chain_free_cb,
        .block_size  = block_size,
        .func        = /*process_func*/ NULL,
        .cpu_flags   = cpu_flags,
    };

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

    int chain_owned_by_out = 0;

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
    const SwsOp *read  = ff_sws_op_list_input(ops);
    const SwsOp *write = ff_sws_op_list_output(ops);
    SwsCompMask imask = read  ? read->mask  : 0;
    SwsCompMask omask = write ? write->mask : 0;
    ret = aarch64_jit_process(&s, &ops->src, &ops->dst, pin, &params[ops->num_ops - 1]);
    if (ret < 0)
        goto error;

    /* add all ops */
    rasm_set_current_node(r, s.loop);
    for (int i = 0; i < ops->num_ops; i++) {
        ret = asmgen_op_jit(&s, &params[i], &regs[i]);
        if (ret < 0)
            goto error;
    }

    /* emit data pool and immediates */
    if (s.n_data > 0 || s.n_imm > 0) {
        rasm_set_current_node(r, s.setup);
        load_constants(&s);
    }

    /* Function frame */
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

    /* From this point on, `*out` owns `s.chain`; don't free it below. */
    chain_owned_by_out = 1;

    // printf("gprs.used %08x\n", s.gprs.used);
    // printf("[%s][%d] %s() %d\n", __FILE__, __LINE__, __func__, SWS_MAX_OPS);
    AVBPrint bp;
    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
    rasm_print(s.rctx, &bp);

    if (getenv("SWS_JIT_DUMP")) {
        fputs(bp.str, stdout);
    }

    uint8_t *text;
    size_t text_size;
    ret = ff_sws_jit_assemble_llvm(bp.str, &text, &text_size);
    if (ret < 0) {
        printf("[%s][%d] %s() ret %d\n", __FILE__, __LINE__, __func__, ret);
        fputs(bp.str, stdout);
    }

    av_bprint_finalize(&bp, NULL);

    out->func = (SwsOpFunc) text;

error:
    if (ret < 0) {
        rasm_free(&s.rctx);
        if (!chain_owned_by_out)
            ff_sws_op_chain_free_cb(s.chain);
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
