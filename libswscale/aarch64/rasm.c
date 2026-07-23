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

#include "rasm.h"

#include <stdarg.h>

#include "libavutil/error.h"
#include "libavutil/intmath.h"
#include "libavutil/macros.h"
#include "libavutil/mem.h"

/*********************************************************************/
/* Main runtime assembler context */

RasmContext *rasm_alloc(void)
{
    return av_mallocz(sizeof(RasmContext));
}

void rasm_free(RasmContext **prctx)
{
    if (!prctx || !*prctx)
        return;

    RasmContext *rctx = *prctx;
    for (int i = 0; i < rctx->num_entries; i++) {
        RasmEntry *entry = &rctx->entries[i];
        RasmNode *node = entry->start;
        while (node != NULL) {
            switch (node->type) {
            case RASM_NODE_COMMENT:
                av_freep(&node->comment.text);
                break;
            case RASM_NODE_DIRECTIVE:
                av_freep(&node->directive.text);
                break;
            case RASM_NODE_DATA:
                av_freep(&node->data.data);
                break;
            default:
                break;
            }
            av_freep(&node->inline_comment);
            RasmNode *current_node = node;
            node = node->next;
            av_free(current_node);
        }
    }
    av_freep(&rctx->entries);
    for (int i = 0; i < rctx->num_labels; i++)
        av_freep(&rctx->labels[i]);
    av_freep(&rctx->labels);
    av_freep(&rctx->comment_next);
    av_freep(prctx);
}

/*********************************************************************/
/* IR Nodes */

static RasmNode *add_node(RasmContext *rctx, RasmNodeType type)
{
    if (rctx->error)
        return NULL;

    RasmNode *node = av_mallocz(sizeof(RasmNode));
    if (!node) {
        rctx->error = AVERROR(ENOMEM);
        return NULL;
    }

    node->type = type;

    if (rctx->current_node) {
        RasmNode *next = rctx->current_node->next;
        node->prev = rctx->current_node;
        node->next = next;
        rctx->current_node->next = node;
        if (next)
            next->prev = node;
    }

    rctx->current_node = node;

    return node;
}

RasmNode *rasm_add_insn(RasmContext *rctx, int id,
                        RasmOp op0, RasmOp op1, RasmOp op2, RasmOp op3)
{
    RasmNode *node = add_node(rctx, RASM_NODE_INSN);
    if (node) {
        node->insn.id        = id;
        node->insn.op[0]     = op0;
        node->insn.op[1]     = op1;
        node->insn.op[2]     = op2;
        node->insn.op[3]     = op3;
        node->inline_comment = rctx->comment_next;
        rctx->comment_next   = NULL;
    }
    return node;
}

RasmNode *rasm_add_comment(RasmContext *rctx, const char *comment)
{
    if (rctx->error)
        return NULL;

    char *dup = av_strdup(comment);
    if (!dup) {
        rctx->error = AVERROR(ENOMEM);
        return NULL;
    }

    RasmNode *node = add_node(rctx, RASM_NODE_COMMENT);
    if (node) {
        node->comment.text = dup;
    } else {
        av_freep(&dup);
    }
    return node;
}

RasmNode *rasm_add_commentf(RasmContext *rctx, char *s, size_t n, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(s, n, fmt, args);
    va_end(args);
    return rasm_add_comment(rctx, s);
}

RasmNode *rasm_add_label(RasmContext *rctx, int id)
{
    RasmNode *node = add_node(rctx, RASM_NODE_LABEL);
    if (node) {
        node->label.id = id;
    }
    return node;
}

RasmNode *rasm_add_func(RasmContext *rctx, int id, bool export,
                        bool jumpable)
{
    RasmNode *node = add_node(rctx, RASM_NODE_FUNCTION);
    if (node) {
        av_assert0(id >= 0 && id < rctx->num_labels);
        node->func.name     = rctx->labels[id];
        node->func.export   = export;
        node->func.jumpable = jumpable;
    }
    return node;
}

RasmNode *rasm_add_endfunc(RasmContext *rctx)
{
    RasmNode *node = add_node(rctx, RASM_NODE_ENDFUNC);
    return node;
}

RasmNode *rasm_add_directive(RasmContext *rctx, const char *text)
{
    if (rctx->error)
        return NULL;

    char *dup = av_strdup(text);
    if (!dup) {
        rctx->error = AVERROR(ENOMEM);
        return NULL;
    }

    RasmNode *node = add_node(rctx, RASM_NODE_DIRECTIVE);
    if (node) {
        node->directive.text = dup;
    } else {
        av_freep(&dup);
    }
    return node;
}

RasmNode *rasm_add_data(RasmContext *rctx, const void *data, unsigned count,
                        RasmDataType type)
{
    if (rctx->error)
        return NULL;

    size_t size = count * rasm_data_type_size(type);
    void *dup = av_memdup(data, size);
    if (!dup) {
        rctx->error = AVERROR(ENOMEM);
        return NULL;
    }

    RasmNode *node = add_node(rctx, RASM_NODE_DATA);
    if (node) {
        node->data.data  = dup;
        node->data.count = count;
        node->data.type  = type;
    } else {
        av_freep(&dup);
    }
    return node;
}

RasmNode *rasm_add_const(RasmContext *rctx, int id)
{
    RasmNode *node = add_node(rctx, RASM_NODE_CONST);
    if (node) {
        av_assert0(id >= 0 && id < rctx->num_labels);
        node->konst.name = rctx->labels[id];
    }
    return node;
}

RasmNode *rasm_add_endconst(RasmContext *rctx)
{
    RasmNode *node = add_node(rctx, RASM_NODE_ENDCONST);
    return node;
}

RasmNode *rasm_get_current_node(RasmContext *rctx)
{
    return rctx->current_node;
}

RasmNode *rasm_set_current_node(RasmContext *rctx, RasmNode *node)
{
    RasmNode *current_node = rctx->current_node;
    rctx->current_node = node;
    return current_node;
}

/*********************************************************************/
/* Top-level IR entries */

int rasm_func_begin(RasmContext *rctx, const char *name, bool export,
                    bool jumpable)
{
    if (rctx->error)
        return rctx->error;

    /* Grow entries array. */
    RasmEntry *entry = av_dynarray2_add((void **) &rctx->entries,
                                        &rctx->num_entries,
                                        sizeof(*rctx->entries), NULL);
    if (!entry) {
        rctx->error = AVERROR(ENOMEM);
        return rctx->error;
    }

    entry->type = RASM_ENTRY_FUNC;

    int id = rasm_new_label(rctx, name);

    rasm_set_current_node(rctx, NULL);
    entry->start = rasm_add_func(rctx, id, export, jumpable);
    entry->end   = rasm_add_endfunc(rctx);
    rasm_set_current_node(rctx, entry->start);

    entry->func.export   = export;
    entry->func.label_id = id;

    if (rctx->error)
        return rctx->error;

    return id;
}

int rasm_const_begin(RasmContext *rctx, const char *name)
{
    if (rctx->error)
        return rctx->error;

    /* Grow entries array. */
    RasmEntry *entry = av_dynarray2_add((void **) &rctx->entries,
                                        &rctx->num_entries,
                                        sizeof(*rctx->entries), NULL);
    if (!entry) {
        rctx->error = AVERROR(ENOMEM);
        return rctx->error;
    }

    entry->type = RASM_ENTRY_CONST;

    int id = rasm_new_label(rctx, name);

    rasm_set_current_node(rctx, NULL);
    entry->start = rasm_add_const(rctx, id);
    entry->end   = rasm_add_endconst(rctx);
    rasm_set_current_node(rctx, entry->start);

    entry->konst.label_id = id;

    if (rctx->error)
        return rctx->error;

    return id;
}

/*********************************************************************/
void rasm_annotate(RasmContext *rctx, const char *comment)
{
    if (rctx->error || !rctx->current_node)
        return;
    RasmNode *current_node = rctx->current_node;
    av_freep(&current_node->inline_comment);
    current_node->inline_comment = av_strdup(comment);
    if (!current_node->inline_comment)
        rctx->error = AVERROR(ENOMEM);
}

void rasm_annotatef(RasmContext *rctx, char *s, size_t n, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(s, n, fmt, args);
    va_end(args);
    rasm_annotate(rctx, s);
}

void rasm_annotate_next(RasmContext *rctx, const char *comment)
{
    if (rctx->error)
        return;
    av_freep(&rctx->comment_next);
    rctx->comment_next = av_strdup(comment);
    if (!rctx->comment_next)
        rctx->error = AVERROR(ENOMEM);
}

void rasm_annotate_nextf(RasmContext *rctx, char *s, size_t n, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(s, n, fmt, args);
    va_end(args);
    rasm_annotate_next(rctx, s);
}

int rasm_new_label(RasmContext *rctx, const char *name)
{
    if (rctx->error)
        return rctx->error;

    char *dup = NULL;
    int ret;

    if (name) {
        dup = av_strdup(name);
        if (!dup) {
            ret = AVERROR(ENOMEM);
            goto error;
        }
    }

    /* Get current label number. */
    ret = rctx->num_labels;

    /* Grow labels array. */
    char **p = av_dynarray2_add((void **) &rctx->labels, &rctx->num_labels,
                                sizeof(*rctx->labels), NULL);
    if (!p) {
        ret = AVERROR(ENOMEM);
        goto error;
    }
    *p = dup;

error:
    if (ret < 0) {
        av_free(dup);
        rctx->error = ret;
    }
    return ret;
}

int rasm_new_labelf(RasmContext *rctx, char *s, size_t n, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(s, n, fmt, args);
    va_end(args);
    return rasm_new_label(rctx, s);
}

/*********************************************************************/
/* AArch64-specific */

AArch64VecViews a64op_vec_views(RasmOp op)
{
    uint8_t n = a64op_vec_n(op);
    AArch64VecViews out = {
        .b   = a64op_vecb  (n),
        .h   = a64op_vech  (n),
        .s   = a64op_vecs  (n),
        .d   = a64op_vecd  (n),
        .q   = a64op_vecq  (n),
        .b8  = a64op_vec8b (n),
        .b16 = a64op_vec16b(n),
        .h4  = a64op_vec4h (n),
        .h8  = a64op_vec8h (n),
        .s2  = a64op_vec2s (n),
        .s4  = a64op_vec4s (n),
        .d2  = a64op_vec2d (n),
    };
    for (int i = 0; i < 2; i++)
        out.be[i] = a64op_elem(out.b, i);
    for (int i = 0; i < 2; i++)
        out.de[i] = a64op_elem(out.d, i);
    return out;
}

/*********************************************************************/
/* AArch64 register state tracker */

#define AARCH64_GPR_PR  (1 << 18u)  /* Platform Register */
#define AARCH64_GPR_SP  (1 << 31u)  /* Stack Pointer */

/* Callee-saved GPRs (r19-r28, fp, and lr). */
#define AARCH64_GPR_CALLEE_SAVED        0x7ff80000u
#define AARCH64_GPR_CALLEE_SAVED_COUNT  12

/* Callee-saved vector registers (bottom 64-bit of v8-v15). */
#define AARCH64_VEC_CALLEE_SAVED        0x0000ff00u
#define AARCH64_VEC_CALLEE_SAVED_COUNT  8

static int a64reg_pick_gpr(uint32_t mask)
{
    uint32_t avail = ~(mask | AARCH64_GPR_PR | AARCH64_GPR_SP);
    av_assert0(avail);
    return ff_ctz(avail);
}

int a64reg_unused_gpr(AArch64RegState *rs, int r)
{
    if (r < 0) {
        r = a64reg_pick_gpr(rs->gpr_used);
    } else {
        av_assert0(r >= 0 && r <= 30);
    }
    rs->gpr_used      |= 1u << r;
    rs->gpr_clobbered |= 1u << r;
    return r;
}

int a64reg_unclobbered_gpr(AArch64RegState *rs)
{
    int r = a64reg_pick_gpr(rs->gpr_clobbered);
    rs->gpr_used      |= 1u << r;
    rs->gpr_clobbered |= 1u << r;
    return r;
}

static int a64reg_pick_vec(uint32_t mask)
{
    uint32_t avail = ~mask;
    av_assert0(avail);
    /* Use callee-saved registers last. */
    if (avail & ~AARCH64_VEC_CALLEE_SAVED)
        return ff_ctz(avail & ~AARCH64_VEC_CALLEE_SAVED);
    return ff_ctz(avail & AARCH64_VEC_CALLEE_SAVED);
}

RasmOp a64reg_vec(AArch64RegState *rs, int r)
{
    if (r < 0) {
        r = a64reg_pick_vec(rs->vec_used);
    } else {
        av_assert0(r >= 0 && r <= 31);
    }
    rs->vec_used      |= 1u << r;
    rs->vec_clobbered |= 1u << r;
    return a64op_vec(r);
}

RasmOp a64reg_unclobbered_vec(AArch64RegState *rs)
{
    int r = a64reg_pick_vec(rs->vec_clobbered);
    rs->vec_used      |= 1u << r;
    rs->vec_clobbered |= 1u << r;
    return a64op_vec(r);
}

static int find_contiguous(uint32_t avail, int n)
{
    uint32_t mask = (1u << n) - 1;
    for (int i = 0; i < 32 - n; i++) {
        if ((avail & (mask << i)) == (mask << i))
            return i;
    }
    return -1;
}

void a64reg_contiguous_vec(AArch64RegState *rs, int n, RasmOp *ops)
{
    uint32_t avail = ~rs->vec_used;
    av_assert0(avail);
    /* Use callee-saved registers last. */
    int r = find_contiguous(avail & ~AARCH64_VEC_CALLEE_SAVED, n);
    if (r < 0)
        r = find_contiguous(avail, n);
    av_assert0(r >= 0);
    for (int i = 0; i < n; i++)
        ops[i] = a64reg_vec(rs, r + i);
}

void a64reg_emit(RasmContext *rctx, const AArch64RegState *rs,
                 RasmNode *prologue, RasmNode *epilogue)
{
    /* Collect clobbered registers and compute frame size. */
    RasmOp regs[AARCH64_GPR_CALLEE_SAVED_COUNT + AARCH64_VEC_CALLEE_SAVED_COUNT];
    unsigned n = 0;
    for (unsigned i = 0; i <= 30; i++) {
        if (rs->gpr_clobbered & AARCH64_GPR_CALLEE_SAVED & (1u << i))
            regs[n++] = a64op_gpx(i);
    }
    if (n & 1)
        regs[n++] = rasm_op_none();
    for (unsigned i = 0; i <= 31; i++) {
        if (rs->vec_clobbered & AARCH64_VEC_CALLEE_SAVED & (1u << i))
            regs[n++] = a64op_vecd(i);
    }
    if (n & 1)
        regs[n++] = rasm_op_none();
    if (!n)
        return;
    unsigned frame_size = n * sizeof(uint64_t);

    RasmNode *saved = rasm_get_current_node(rctx);
    RasmOp sp      = a64op_sp();
    RasmOp sp_pre  = a64op_pre(sp, -frame_size);
    RasmOp sp_post = a64op_post(sp, frame_size);

    /* Emit prologue. */
    rasm_set_current_node(rctx, prologue);
    rasm_add_comment(rctx, "prologue");
    if (rasm_op_type(regs[1]) == RASM_OP_NONE)
        i_str(rctx, regs[0], sp_pre);
    else
        i_stp(rctx, regs[0], regs[1], sp_pre);
    for (unsigned i = 2; i < n; i += 2) {
        if (rasm_op_type(regs[i + 1]) == RASM_OP_NONE)
            i_str(rctx, regs[i],              a64op_off(sp, i * sizeof(uint64_t)));
        else
            i_stp(rctx, regs[i], regs[i + 1], a64op_off(sp, i * sizeof(uint64_t)));
    }

    /* Emit epilogue. */
    rasm_set_current_node(rctx, epilogue);
    rasm_add_comment(rctx, "epilogue");
    for (unsigned i = n - 2; i >= 2; i -= 2) {
        if (rasm_op_type(regs[i + 1]) == RASM_OP_NONE)
            i_ldr(rctx, regs[i],              a64op_off(sp, i * sizeof(uint64_t)));
        else
            i_ldp(rctx, regs[i], regs[i + 1], a64op_off(sp, i * sizeof(uint64_t)));
    }
    if (rasm_op_type(regs[1]) == RASM_OP_NONE)
        i_ldr(rctx, regs[0],          sp_post);
    else
        i_ldp(rctx, regs[0], regs[1], sp_post);

    rasm_set_current_node(rctx, saved);
}
