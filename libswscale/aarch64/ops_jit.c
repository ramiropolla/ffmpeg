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
#include "../uops_list.h"
#include "../jit.h"

#include "ops.h"
#include "ops_asmgen.h"
#include "ops_impl.h"
#include "ops_jit_llvm.h"
#include "rasm.h"

/*********************************************************************/
typedef struct SwsAArch64JITContext {
    SwsAArch64Context s;

    SwsAArch64OpImplParams params[SWS_MAX_OPS];
    SwsAArch64OpRegs regs[SWS_MAX_OPS];
    SwsImplResult res[SWS_MAX_OPS];

    void   *code;
    size_t  code_size;
} SwsAArch64JITContext;

static void aarch64_jit_free(void *priv)
{
    if (priv) {
        SwsAArch64JITContext *ctx = priv;
        rasm_free(&ctx->s.rctx);
        for (int i = 0; i < SWS_MAX_OPS; i++) {
            if (ctx->res[i].free)
                ctx->res[i].free(&ctx->res[i].priv);
        }
        if (ctx->code)
            ff_sws_jit_free(ctx->code, ctx->code_size);
        av_free(ctx);
    }
}

/*********************************************************************/
/**
 * Structured read and write instructions (ld2/ld3/ld4/st2/st3/st4),
 * used by the packed read and write operations, require contiguous
 * vectors. The read operation already has contiguous vectors because
 * it is the first operation to be emitted, but there is no guarantee
 * that the write operation will have contiguous vectors. We fix this
 * here by ensuring the vectors are contiguous, allocating and moving
 * to new registers if necessary.
 */

static void jit_make_contiguous_vecs(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                                     RasmOp *sx)
{
    bool contiguous = true;
    int n = 0;
    LOOP_MASK(p, i) {
        if (i && (a64op_vec_n(sx[i]) & 0x1f) != ((a64op_vec_n(sx[i - 1]) + 1) & 0x1f))
            contiguous = false;
        n++;
    }
    if (contiguous)
        return;

    RasmContext *r = s->rctx;
    AArch64RegState *rs = &s->regstate;
    RasmOp new_sx[4] = { 0 };
    a64reg_contiguous_vec(rs, n, new_sx);
    LOOP_MASK(p, i) {
        new_sx[i] = a64op_make_vec(a64op_vec_n(new_sx[i]), s->el_count, s->el_size);
        i_mov(r, new_sx[i], sx[i]);
        a64reg_vec_free(rs, sx[i]);
        sx[i] = new_sx[i];
    }
}

static void jit_write_packed_fixup(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                                   SwsAArch64OpRegs *regs)
{
    jit_make_contiguous_vecs    (s, p, regs->sl);
    if (s->use_vh)
        jit_make_contiguous_vecs(s, p, regs->sh);
}

/*********************************************************************/
static const char *const op_type_names[SWS_UOP_TYPE_NB] = {
#define UOP_NAME(OP, ABBR) [OP] = ABBR,
    UOPS_LIST(UOP_NAME)
#undef UOP_NAME
};

static int asmgen_op_jit(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                         SwsAArch64OpRegs *regs)
{
    RasmContext *r = s->rctx;

    ff_sws_aarch64_asmgen_setup_vecs(s, p, regs);

    rasm_add_commentf(r, (char[32]){0}, 32, "%s() {", op_type_names[p->uop]);

    /**
     * Ensure packed write operations work on contiguous vectors.
     * TODO fix this by implementing proper register tracking and automatic
     *      register allocation.
     */
    if (p->uop == SWS_UOP_WRITE_PACKED)
        jit_write_packed_fixup(s, p, regs);

    ff_sws_aarch64_asmgen_op(s, p, regs);

    rasm_add_comment(r, "}");

    return 0;
}

/*********************************************************************/
/* Constant data */

/* Returns a vector operand that holds the entire 128-bit sequence. */
static RasmOp jit_push_v128(SwsAArch64Context *s, void *val)
{
    /* Check whether we already have it. */
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

/* Returns a vector operand that holds the 32-bit value broadcast to all elements. */
static RasmOp jit_push_vimm(SwsAArch64Context *s, SwsPixelType type, uint32_t val)
{
    /* Expand to u32. */
    switch (type) {
    case SWS_PIXEL_U8:  val = val | (val <<  8); av_fallthrough;
    case SWS_PIXEL_U16: val = val | (val << 16); break;
    }

    /* TODO use movi for movi-encodable immediates. */
    /* TODO use mov+dup instead of taking up data. */

    SwsAArch64Vector vec = { .u32 = { val, val, val, val } };
    return jit_push_v128(s, &vec);
}

/* Returns a vector by-element operand that holds the 32-bit value. */
static RasmOp jit_push_elem(SwsAArch64Context *s, SwsPixelType type, uint32_t val)
{
    /* Check whether we already have it in data. */
    for (int i = 0; i < s->data_count; i++) {
        if (rasm_op_type(s->data[i].op) != AARCH64_OP_VEC)
            continue;
        for (int j = 0; j < s->data[i].op_idx; j++) {
            if (s->data[i].vec.u32[j] == val)
                return a64op_elem(v_4s(s->data[i].op), j);
        }
    }

    /* Check whether there's space left in a previously allocated vector. */
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

/* Returns a GPR that holds the 64-bit value. */
static RasmOp jit_push_u64(SwsAArch64Context *s, uint64_t val)
{
    /* Add it to our data and create a new GPR. */
    int idx = s->data_count++;
    s->data[idx].vec.u64[0] = val;
    s->data[idx].op     = a64reg_unclobbered_gpx(&s->regstate);
    s->data[idx].op_idx = 2;
    return s->data[idx].op;
}

/* Emit const data and load it into registers at setup time. */
static void jit_load_constants(SwsAArch64Context *s)
{
    RasmContext *r = s->rctx;
    AArch64RegState *rs = &s->regstate;

    /* Emit data. */
    int ldata = rasm_const_begin(r, "ldata");
    for (int i = 0; i < s->data_count; i++) {
        switch (rasm_op_type(s->data[i].op)) {
        case AARCH64_OP_GPR:
            rasm_add_data(r, &s->data[i].vec, 2, RASM_DATA_QUAD);
            break;
        case AARCH64_OP_VEC:
        default:
            rasm_add_data(r, &s->data[i].vec, 4, RASM_DATA_WORD);
            break;
        }
    }

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
/* Debug logging. */

static const char *print_reg(char buf[8], RasmOp op)
{
    switch (rasm_op_type(op)) {
    case AARCH64_OP_GPR:
        snprintf(buf, 8, "x%-2d", a64op_gpr_n(op));
        break;
    case AARCH64_OP_VEC:
        snprintf(buf, 8, "v%-2d", a64op_vec_n(op));
        break;
    default:
        snprintf(buf, 8, "___");
        break;
    }
    return buf;
}
#define PRINT_REG(op) print_reg((char[8]){0}, op)

static void print_io_regs(SwsContext *sws, const SwsAArch64OpImplParams *p, const SwsAArch64OpRegs *regs)
{
    av_log(sws, AV_LOG_TRACE, "[%-18s] { %s %s %s %s } { %s %s %s %s } -> { %s %s %s %s } { %s %s %s %s }\n",
           op_type_names[p->uop],
           PRINT_REG(regs->sl[0]), PRINT_REG(regs->sl[1]), PRINT_REG(regs->sl[2]), PRINT_REG(regs->sl[3]),
           PRINT_REG(regs->sh[0]), PRINT_REG(regs->sh[1]), PRINT_REG(regs->sh[2]), PRINT_REG(regs->sh[3]),
           PRINT_REG(regs->dl[0]), PRINT_REG(regs->dl[1]), PRINT_REG(regs->dl[2]), PRINT_REG(regs->dl[3]),
           PRINT_REG(regs->dh[0]), PRINT_REG(regs->dh[1]), PRINT_REG(regs->dh[2]), PRINT_REG(regs->dh[3]));
}

/*********************************************************************/
/* Setup helpers. */

/**
 * Recompute op_mask from SwsOp because SwsAArch64OpImplParams has dropped
 * passthrough information to prevent duplicates.
 */
static SwsCompMask recompute_op_mask(const SwsOp *op)
{
    SwsCompMask op_mask = 0;
    for (int i = 0; i < 4; i++) {
        if (SWS_OP_NEEDED(op, i))
            op_mask |= SWS_COMP(i);
    }
    return op_mask;
}

/* Get value from SwsOpPriv based on the pixel type. */
static uint32_t get_priv_val(const SwsOpPriv *priv, SwsPixelType type, int i)
{
    return (type == SWS_PIXEL_U8)  ? priv->u8[i]
         : (type == SWS_PIXEL_U16) ? priv->u16[i]
         :                           priv->u32[i];
}

/**
 * Allocate registers and immediately free them.
 * NOTE: this should be done as the last step in register allocation,
 *       to prevent these temporary registers from being reused.
 */
static void jit_alloc_vt(AArch64RegState *rs, int n, RasmOp *out)
{
    for (int i = 0; i < n; i++)
        out[i] = a64reg_vec(rs, -1);
    for (int i = 0; i < n; i++)
        a64reg_vec_free(rs, out[i]);
}

static void setup_mask_alloc(SwsAArch64Context *s, SwsCompMask mask,
                             SwsAArch64OpRegs *regs)
{
    AArch64RegState *rs = &s->regstate;
    LOOP      (mask, i) { regs->dl[i] = a64reg_vec(rs, -1); }
    LOOP_VH(s, mask, i) { regs->dh[i] = a64reg_vec(rs, -1); }
}

static void setup_mask_free(SwsAArch64Context *s, SwsCompMask mask,
                            SwsAArch64OpRegs *regs)
{
    AArch64RegState *rs = &s->regstate;
    LOOP      (mask, i) { a64reg_vec_free(rs, regs->sl[i]); }
    LOOP_VH(s, mask, i) { a64reg_vec_free(rs, regs->sh[i]); }
}

static void setup_mask_write(SwsAArch64Context *s, SwsCompMask mask,
                             const SwsAArch64OpRegs *prev, SwsAArch64OpRegs *regs)
{
    LOOP      (mask, i) { regs->sl[i] = prev->dl[i]; }
    LOOP_VH(s, mask, i) { regs->sh[i] = prev->dh[i]; }
}

static void setup_mask_passthrough(SwsAArch64Context *s, SwsCompMask mask,
                                   const SwsAArch64OpRegs *prev, SwsAArch64OpRegs *regs)
{
    LOOP      (mask, i) { regs->dl[i] = regs->sl[i] = prev->dl[i]; }
    LOOP_VH(s, mask, i) { regs->dh[i] = regs->sh[i] = prev->dh[i]; }
}

/*********************************************************************/
static void asmgen_setup_read_bit(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                                  const SwsAArch64OpRegs *prev, SwsAArch64OpRegs *regs,
                                  SwsImplResult *res)
{
    setup_mask_alloc(s, p->mask, regs);

    AArch64RegState *rs = &s->regstate;
    jit_alloc_vt(rs, 1, regs->vt);

    /* constants */
    regs->vk[0] = jit_push_v128(s, res->priv.data);
    regs->vk[1] = jit_push_vimm(s, SWS_PIXEL_U8, 1);
}

static void asmgen_setup_read_nibble(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                                     const SwsAArch64OpRegs *prev, SwsAArch64OpRegs *regs,
                                     SwsImplResult *res)
{
    setup_mask_alloc(s, p->mask, regs);

    AArch64RegState *rs = &s->regstate;
    jit_alloc_vt(rs, 1, regs->vt);

    /* constants */
    regs->vk[0] = jit_push_vimm(s, SWS_PIXEL_U8, 0x0f);
}

static void asmgen_setup_read_packed(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                                     const SwsAArch64OpRegs *prev, SwsAArch64OpRegs *regs,
                                     SwsImplResult *res)
{
    /* Count number of elems. */
    int n = 0;
    LOOP_MASK(p, i)
        n++;

    AArch64RegState *rs = &s->regstate;
    a64reg_contiguous_vec    (rs, n, regs->dl);
    if (s->use_vh)
        a64reg_contiguous_vec(rs, n, regs->dh);
}

static void asmgen_setup_read_planar(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                                     const SwsAArch64OpRegs *prev, SwsAArch64OpRegs *regs,
                                     SwsImplResult *res)
{
    setup_mask_alloc(s, p->mask, regs);
}

static void asmgen_setup_write_bit(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                                   const SwsAArch64OpRegs *prev, SwsAArch64OpRegs *regs,
                                   SwsImplResult *res)
{
    setup_mask_write(s, p->mask, prev, regs);

    AArch64RegState *rs = &s->regstate;
    jit_alloc_vt(rs, 2, regs->vt);

    /* constants */
    regs->vk[0] = jit_push_v128(s, res->priv.data);
}

static void asmgen_setup_write_nibble(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                                      const SwsAArch64OpRegs *prev, SwsAArch64OpRegs *regs,
                                      SwsImplResult *res)
{
    setup_mask_write(s, p->mask, prev, regs);

    AArch64RegState *rs = &s->regstate;
    jit_alloc_vt(rs, 2, regs->vt);
}

static void asmgen_setup_write_packed(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                                      const SwsAArch64OpRegs *prev, SwsAArch64OpRegs *regs,
                                      SwsImplResult *res)
{
    /* TODO See jit_write_packed_fixup(). */
    setup_mask_write(s, p->mask, prev, regs);
}

static void asmgen_setup_write_planar(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                                      const SwsAArch64OpRegs *prev, SwsAArch64OpRegs *regs,
                                      SwsImplResult *res)
{
    setup_mask_write(s, p->mask, prev, regs);
}

static void asmgen_setup_swap_bytes(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                                    const SwsAArch64OpRegs *prev, SwsAArch64OpRegs *regs,
                                    SwsImplResult *res)
{
    setup_mask_passthrough(s, p->mask, prev, regs);
}

static void asmgen_setup_swizzle(SwsAArch64Context *s, SwsAArch64OpImplParams *p,
                                 const SwsAArch64OpRegs *prev, SwsAArch64OpRegs *regs,
                                 SwsImplResult *res, const SwsOp *op)
{
    AArch64RegState *rs = &s->regstate;

    /* Split original swizzle into identity, renames, and copies. */
    SwsCompMask identity = 0;
    SwsMoveUOp rename = { 0 };
    SwsMoveUOp copy = { 0 };
    bool overwritten[4] = { false, false, false, false };
    SwsCompMask op_mask = recompute_op_mask(op);
    LOOP(op_mask, i) {
        int src = op->swizzle.in[i];
        if (src == i) {
            identity |= SWS_COMP(i);
        } else {
            SwsMoveUOp *list = overwritten[src] ? &copy : &rename;
            list->dst[list->num_moves] = i;
            list->src[list->num_moves] = src;
            list->num_moves++;
        }
        overwritten[src] = true;
    }

    /* Identity passthrough. */
    setup_mask_passthrough(s, identity, prev, regs);

    /* Perform simple renames. */
    for (int i = 0; i < rename.num_moves; i++) {
        int src = rename.src[i];
        int dst = rename.dst[i];
        regs->dl[dst] = regs->sl[src] = prev->dl[src];
        if (s->use_vh)
            regs->dh[dst] = regs->sh[src] = prev->dh[src];
    }

    /* Replace moves list with remaining copies. */
    p->par.move = copy;
    p->mask = 0;
    for (int i = 0; i < copy.num_moves; i++) {
        int dst = copy.dst[i];
        regs->dl[dst] = a64reg_vec(rs, -1);
        if (s->use_vh)
            regs->dh[dst] = a64reg_vec(rs, -1);
        p->mask |= SWS_COMP(dst);
    }
}

static void asmgen_setup_unpack(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                                const SwsAArch64OpRegs *prev, SwsAArch64OpRegs *regs,
                                SwsImplResult *res)
{
    AArch64RegState *rs = &s->regstate;

    regs->sl[0] = prev->dl[0];
    if (s->use_vh)
        regs->sh[0] = prev->dh[0];
    LOOP_MASK      (p, i) { regs->dl[i] = i ? a64reg_vec(rs, -1) : regs->sl[i]; }
    LOOP_MASK_VH(s, p, i) { regs->dh[i] = i ? a64reg_vec(rs, -1) : regs->sh[i]; }

    /* constants */
    LOOP_MASK      (p, i) {
        uint32_t val = (1u << p->par.pack.pattern[i]) - 1;
        regs->vk[i] = jit_push_vimm(s, p->type, val);
    }
}

static void asmgen_setup_pack(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                              const SwsAArch64OpRegs *prev, SwsAArch64OpRegs *regs,
                              SwsImplResult *res)
{
    setup_mask_passthrough(s, p->mask, prev, regs);
    setup_mask_free(s, p->mask & ~1u, regs);
}

static void asmgen_setup_shift(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                               const SwsAArch64OpRegs *prev, SwsAArch64OpRegs *regs,
                               SwsImplResult *res)
{
    setup_mask_passthrough(s, p->mask, prev, regs);
}

static void asmgen_setup_clear(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                               const SwsAArch64OpRegs *prev, SwsAArch64OpRegs *regs,
                               SwsImplResult *res, const SwsOp *op)
{
    /* TODO factor clear into setup instead of performing dup. */

    SwsCompMask op_mask = recompute_op_mask(op);
    SwsCompMask identity = op_mask & ~p->mask;

    if (prev) {
        LOOP_MASK(p, i) {
            if (rasm_op_type(prev->dl[i]) != RASM_OP_NONE) {
                identity |= SWS_COMP(i);
            }
        }
    }

    setup_mask_passthrough(s, identity, prev, regs);
    setup_mask_alloc(s, p->mask & ~identity, regs);

    /* constants */
    regs->vk[0] = jit_push_v128(s, res->priv.data);
}

static void asmgen_setup_convert(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                                 const SwsAArch64OpRegs *prev, SwsAArch64OpRegs *regs,
                                 SwsImplResult *res)
{
    AArch64RegState *rs = &s->regstate;

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

    setup_mask_passthrough(s, p->mask, prev, regs);
    if (src_use_vh && dst_use_vh) {
        LOOP_MASK(p, i) { regs->dh[i] = regs->sh[i] = prev->dh[i]; }
    } else if (!src_use_vh && dst_use_vh) {
        LOOP_MASK(p, i) { regs->dh[i] = a64reg_vec(rs, -1); }
    } else if (src_use_vh && !dst_use_vh) {
        LOOP_MASK(p, i) {
            regs->sh[i] = prev->dh[i];
            a64reg_vec_free(rs, regs->sh[i]);
        }
    }
}

static void asmgen_setup_clamp(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                               const SwsAArch64OpRegs *prev, SwsAArch64OpRegs *regs,
                               SwsImplResult *res)
{
    setup_mask_passthrough(s, p->mask, prev, regs);

    /* constants */
    LOOP_MASK(p, i) {
        uint32_t val = get_priv_val(&res->priv, p->type, i);
        regs->vk[i] = jit_push_vimm(s, p->type, val);
    }
}

static void asmgen_setup_scale(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                               const SwsAArch64OpRegs *prev, SwsAArch64OpRegs *regs,
                               SwsImplResult *res)
{
    setup_mask_passthrough(s, p->mask, prev, regs);

    /* constants */
    uint32_t val = get_priv_val(&res->priv, p->type, 0);
    regs->vk[0] = jit_push_vimm(s, p->type, val);
}

static void asmgen_setup_linear(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                                const SwsAArch64OpRegs *prev, SwsAArch64OpRegs *regs,
                                SwsImplResult *res)
{
    AArch64RegState *rs = &s->regstate;

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
    setup_mask_passthrough(s, p->mask, prev, regs);
    for (int i = 0; i < 4; i++) {
        if (SWS_COMP_TEST(p->mask, i))
            continue;
        if (rasm_op_type(prev->dl[i]) != RASM_OP_NONE)
            regs->dl[i] = regs->sl[i] = prev->dl[i];
        if (s->use_vh && rasm_op_type(prev->dh[i]) != RASM_OP_NONE)
            regs->dh[i] = regs->sh[i] = prev->dh[i];
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
    setup_mask_alloc(s, save_mask, regs);
    if (p->uop == SWS_UOP_LINEAR)
        jit_alloc_vt(rs, 4, &regs->vt[8]);
    setup_mask_free(s, save_mask, regs);

    /* constants */
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
}

static void asmgen_setup_dither(SwsAArch64Context *s, const SwsAArch64OpImplParams *p,
                                const SwsAArch64OpRegs *prev, SwsAArch64OpRegs *regs,
                                SwsImplResult *res, const SwsOp *op)
{
    SwsCompMask op_mask = recompute_op_mask(op);
    setup_mask_passthrough(s, op_mask, prev, regs);

    AArch64RegState *rs = &s->regstate;
    jit_alloc_vt(rs, 2, regs->vt);

    /* constants */
    regs->dither_ptr = jit_push_u64(s, (uint64_t) res->priv.ptr);
}

/* Set up registers for operation. */
static void aarch64_jit_setup(SwsAArch64JITContext *ctx, const SwsOpList *ops, int n)
{
    SwsAArch64Context      *s    = &ctx->s;
    SwsAArch64OpImplParams *p    = &ctx->params[n];
    const SwsAArch64OpRegs *prev = n ? &ctx->regs[n - 1] : NULL;
    SwsAArch64OpRegs       *regs = &ctx->regs[n];
    SwsImplResult          *res  = &ctx->res[n];
    const SwsOp            *op   = &ops->ops[n];

    /* TODO repeated. */
    size_t el_size = ff_sws_pixel_type_size(p->type);
    size_t total_size = p->block_size * el_size;

    s->vec_size = FFMIN(total_size, 16);
    s->use_vh = (s->vec_size != total_size);

    s->el_size = el_size;
    s->el_count = s->vec_size / el_size;

    switch (p->uop) {
    case SWS_UOP_READ_BIT:     asmgen_setup_read_bit(s, p, prev, regs, res);     break;
    case SWS_UOP_READ_NIBBLE:  asmgen_setup_read_nibble(s, p, prev, regs, res);  break;
    case SWS_UOP_READ_PACKED:  asmgen_setup_read_packed(s, p, prev, regs, res);  break;
    case SWS_UOP_READ_PLANAR:  asmgen_setup_read_planar(s, p, prev, regs, res);  break;
    case SWS_UOP_WRITE_BIT:    asmgen_setup_write_bit(s, p, prev, regs, res);    break;
    case SWS_UOP_WRITE_NIBBLE: asmgen_setup_write_nibble(s, p, prev, regs, res); break;
    case SWS_UOP_WRITE_PACKED: asmgen_setup_write_packed(s, p, prev, regs, res); break;
    case SWS_UOP_WRITE_PLANAR: asmgen_setup_write_planar(s, p, prev, regs, res); break;
    case SWS_UOP_SWAP_BYTES:   asmgen_setup_swap_bytes(s, p, prev, regs, res);   break;
    case SWS_UOP_PERMUTE:      asmgen_setup_swizzle(s, p, prev, regs, res, op);  break;
    case SWS_UOP_COPY:         asmgen_setup_swizzle(s, p, prev, regs, res, op);  break;
    case SWS_UOP_UNPACK:       asmgen_setup_unpack(s, p, prev, regs, res);       break;
    case SWS_UOP_PACK:         asmgen_setup_pack(s, p, prev, regs, res);         break;
    case SWS_UOP_LSHIFT:       asmgen_setup_shift(s, p, prev, regs, res);        break;
    case SWS_UOP_RSHIFT:       asmgen_setup_shift(s, p, prev, regs, res);        break;
    case SWS_UOP_CLEAR:        asmgen_setup_clear(s, p, prev, regs, res, op);    break;
    case SWS_UOP_TO_U8:        asmgen_setup_convert(s, p, prev, regs, res);      break;
    case SWS_UOP_TO_U16:       asmgen_setup_convert(s, p, prev, regs, res);      break;
    case SWS_UOP_TO_U32:       asmgen_setup_convert(s, p, prev, regs, res);      break;
    case SWS_UOP_TO_F32:       asmgen_setup_convert(s, p, prev, regs, res);      break;
    case SWS_UOP_EXPAND_PAIR:  asmgen_setup_convert(s, p, prev, regs, res);      break;
    case SWS_UOP_EXPAND_QUAD:  asmgen_setup_convert(s, p, prev, regs, res);      break;
    case SWS_UOP_MIN:          asmgen_setup_clamp(s, p, prev, regs, res);        break;
    case SWS_UOP_MAX:          asmgen_setup_clamp(s, p, prev, regs, res);        break;
    case SWS_UOP_SCALE:        asmgen_setup_scale(s, p, prev, regs, res);        break;
    case SWS_UOP_LINEAR:       asmgen_setup_linear(s, p, prev, regs, res);       break;
    case SWS_UOP_LINEAR_FMA:   asmgen_setup_linear(s, p, prev, regs, res);       break;
    case SWS_UOP_DITHER:       asmgen_setup_dither(s, p, prev, regs, res, op);   break;
    default:
        break;
    }
}

/*********************************************************************/
static void asmgen_process_frame(SwsAArch64Context *s, SwsCompMask imask, SwsCompMask omask)
{
    AArch64RegState *rs = &s->regstate;

    /* SwsOpFunc arguments. */
    s->exec      = a64reg_argx(rs, 0); // const SwsOpExec *exec
    s->impl      = a64reg_argx(rs, 1); // const void *priv
    s->bx_start  = a64reg_argw(rs, 2); // int bx_start
    s->y_start   = a64reg_argw(rs, 3); // int y_start
    s->bx_end    = a64reg_argw(rs, 4); // int bx_end
    s->y_end     = a64reg_argw(rs, 5); // int y_end

    /* Loop iterator variables. */
    s->bx        = a64reg_gpw(rs, 6);
    s->y         = a64reg_gpw(rs, 3);  /* Reused from SwsOpFunc.y_start argument. */

    /* Scratch registers. */
    s->tmp0      = a64reg_gpx(rs, 16); /* IP0 */
    s->tmp1      = a64reg_gpx(rs, 17); /* IP1 */

    /* GPRs */
    LOOP(imask, i) { s->in      [i] = a64reg_gpx(rs, -1); }
    LOOP(imask, i) { s->in_bump [i] = a64reg_gpx(rs, -1); }
    LOOP(omask, i) { s->out     [i] = a64reg_gpx(rs, -1); }
    LOOP(omask, i) { s->out_bump[i] = a64reg_gpx(rs, -1); }
}

static int aarch64_jit_process(SwsAArch64Context *s, const SwsOpList *ops, SwsCompMask imask, SwsCompMask omask)
{
    RasmContext *r = s->rctx;
    char func_name[128];

    snprintf(func_name, sizeof(func_name), "jit_process_%s_%s_neon",
             av_get_pix_fmt_name(ops->src.format),
             av_get_pix_fmt_name(ops->dst.format));
    rasm_func_begin(r, func_name, true, false);

    ff_sws_aarch64_asmgen_process(s, imask, omask);

    return 0;
}

/*********************************************************************/
static int aarch64_jit_compile(SwsContext *sws, const SwsOpList *ops,
                               SwsCompiledOp *out)
{
    int ret;

    const int cpu_flags = av_get_cpu_flags();
    if (!(cpu_flags & AV_CPU_FLAG_NEON))
        return AVERROR(ENOTSUP);

    SwsAArch64JITContext *ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return AVERROR(ENOMEM);

    ctx->s.rctx = rasm_alloc();
    if (!ctx->s.rctx) {
        ret = AVERROR(ENOMEM);
        goto error;
    }

    av_log(sws, AV_LOG_DEBUG, "JIT compile: %s -> %s\n",
           av_get_pix_fmt_name(ops->src.format),
           av_get_pix_fmt_name(ops->dst.format));

    /* Use at most two full vregs during the widest precision section */
    int block_size = (ff_sws_op_list_max_size(ops) == 4) ? 8 : 16;

    /* Allocate all registers. */

    const SwsOp *read      = ff_sws_op_list_input(ops);
    const SwsOp *write     = ff_sws_op_list_output(ops);
    const int read_planes  = read ? ff_sws_rw_op_planes(read) : 0;
    const int write_planes = ff_sws_rw_op_planes(write);
    SwsCompMask imask = SWS_COMP_ELEMS(read_planes);
    SwsCompMask omask = SWS_COMP_ELEMS(write_planes);

    asmgen_process_frame(&ctx->s, imask, omask);

    /* Translate all ops into implementation parameters and setup registers. */
    for (int i = 0; i < ops->num_ops; i++) {
        ret = ff_sws_aarch64_ops_translate(sws, ops, i, block_size, &ctx->params[i]);
        if (ret < 0)
            goto error;
        ret = ff_sws_aarch64_setup(ops, block_size, i, &ctx->params[i], &ctx->res[i]);
        if (ret < 0)
            goto error;
        aarch64_jit_setup(ctx, ops, i);
    }

    /* Debug print input/output vectors. */
    if (av_log_get_level() >= AV_LOG_TRACE) {
        av_log(sws, AV_LOG_TRACE, "JIT I/O register allocation:\n");
        for (int i = 0; i < ops->num_ops; i++)
            print_io_regs(sws, &ctx->params[i], &ctx->regs[i]);
    }

    /* create process */
    ret = aarch64_jit_process(&ctx->s, ops, imask, omask);
    if (ret < 0)
        goto error;

    /* add all ops */
    rasm_set_current_node(ctx->s.rctx, ctx->s.loop);
    for (int i = 0; i < ops->num_ops; i++) {
        ret = asmgen_op_jit(&ctx->s, &ctx->params[i], &ctx->regs[i]);
        if (ret < 0)
            goto error;
    }

    if (ctx->s.data_count)
        jit_load_constants(&ctx->s);

    AVBPrint bp;
    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
    rasm_print(ctx->s.rctx, &bp);

    if (1 || getenv("SWS_JIT_DUMP")) {
        fputs(bp.str, stdout);
    }

    uint8_t *text = NULL;
    size_t text_size = 0;
    ret = ff_sws_jit_assemble_llvm(bp.str, &text, &text_size);
    if (ret < 0) {
        // TODO
        printf("[%s][%d] %s() ASSEMBLE ERROR ret %d\n", __FILE__, __LINE__, __func__, ret);
    } else {
        *out = (SwsCompiledOp) {
            .priv        = ctx,
            .slice_align = 1,
            .free        = aarch64_jit_free,
            .block_size  = block_size,
            .func        = (SwsOpFunc) text,
            .cpu_flags   = AV_CPU_FLAG_NEON,
        };

        ctx->code      = text;
        ctx->code_size = text_size;
    }

    av_bprint_finalize(&bp, NULL);

error:
    if (ret < 0)
        aarch64_jit_free(ctx);
    return ret;
}

/*********************************************************************/
const SwsOpBackend backend_aarch64_jit = {
    .name      = "aarch64_jit",
    .flags     = SWS_BACKEND_AARCH64_JIT,
    .compile   = aarch64_jit_compile,
    .hw_format = AV_PIX_FMT_NONE,
};
