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
#include "libavutil/macros.h"
#include "libavutil/mem.h"

/*********************************************************************/
/* Main runtime assembler context */

RasmContext *rasm_alloc(void)
{
    return av_mallocz(sizeof(RasmContext));
}

void rasm_free(RasmContext **pactx)
{
    if (!pactx || !*pactx)
        return;

    RasmContext *actx = *pactx;
    for (int i = 0; i < actx->num_entries; i++) {
        RasmEntry *entry = &actx->entries[i];
        RasmNode *node = entry->start;
        while (node != NULL) {
            switch (node->type) {
            case RASM_NODE_COMMENT:
                av_freep(&node->comment.text);
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
    av_freep(&actx->entries);
    for (int i = 0; i < actx->num_labels; i++)
        av_freep(&actx->labels[i]);
    av_freep(&actx->labels);
    av_freep(pactx);
}

/*********************************************************************/
/* IR Nodes */

static RasmNode *add_node(RasmContext *actx, RasmNodeType type)
{
    if (actx->error)
        return NULL;

    RasmNode *node = av_mallocz(sizeof(RasmNode));
    if (!node) {
        actx->error = AVERROR(ENOMEM);
        return NULL;
    }

    node->type = type;

    if (actx->current_node) {
        RasmNode *next = actx->current_node->next;
        node->prev = actx->current_node;
        node->next = next;
        actx->current_node->next = node;
        if (next)
            next->prev = node;
    }

    actx->current_node = node;

    return node;
}

RasmNode *rasm_add_insn(RasmContext *actx, int id,
                        RasmOp op0, RasmOp op1, RasmOp op2, RasmOp op3)
{
    RasmNode *node = add_node(actx, RASM_NODE_INSN);
    if (node) {
        node->insn.id        = id;
        node->insn.op[0]     = op0;
        node->insn.op[1]     = op1;
        node->insn.op[2]     = op2;
        node->insn.op[3]     = op3;
        node->inline_comment = actx->comment_next;
        actx->comment_next   = NULL;
    }
    return node;
}

RasmNode *rasm_add_comment(RasmContext *actx, const char *comment)
{
    if (actx->error)
        return NULL;

    char *dup = av_strdup(comment);
    if (!dup) {
        actx->error = AVERROR(ENOMEM);
        return NULL;
    }

    RasmNode *node = add_node(actx, RASM_NODE_COMMENT);
    if (node) {
        node->comment.text = dup;
    } else {
        av_freep(&dup);
    }
    return node;
}

RasmNode *rasm_add_commentf(RasmContext *actx, char *s, size_t n, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(s, n, fmt, args);
    va_end(args);
    return rasm_add_comment(actx, s);
}

RasmNode *rasm_add_label(RasmContext *actx, int id)
{
    RasmNode *node = add_node(actx, RASM_NODE_LABEL);
    if (node) {
        node->label.id = id;
    }
    return node;
}

RasmNode *rasm_add_func(RasmContext *actx, int id, bool export)
{
    RasmNode *node = add_node(actx, RASM_NODE_FUNCTION);
    if (node) {
        assert(id >= 0 && id < actx->num_labels);
        node->func.name   = actx->labels[id];
        node->func.export = export;
    }
    return node;
}

RasmNode *rasm_add_endfunc(RasmContext *actx)
{
    RasmNode *node = add_node(actx, RASM_NODE_ENDFUNC);
    return node;
}

RasmNode *rasm_add_directive(RasmContext *actx, const char *text)
{
    if (actx->error)
        return NULL;

    char *dup = av_strdup(text);
    if (!dup) {
        actx->error = AVERROR(ENOMEM);
        return NULL;
    }

    RasmNode *node = add_node(actx, RASM_NODE_DIRECTIVE);
    if (node) {
        node->directive.text = dup;
    } else {
        av_freep(&dup);
    }
    return node;
}

RasmNode *rasm_set_current_node(RasmContext *actx, RasmNode *node)
{
    RasmNode *current_node = actx->current_node;
    actx->current_node = node;
    return current_node;
}

/*********************************************************************/
/* Top-level IR entries */

int rasm_func_begin(RasmContext *actx, const char *name, bool export)
{
    if (actx->error)
        return actx->error;

    /* Grow entries array. */
    RasmEntry *entry = av_dynarray2_add((void **) &actx->entries,
                                        &actx->num_entries,
                                        sizeof(*actx->entries), NULL);
    if (!entry) {
        actx->error = AVERROR(ENOMEM);
        return actx->error;
    }

    entry->type = RASM_ENTRY_FUNC;

    int id = rasm_new_label(actx, name);

    rasm_set_current_node(actx, NULL);
    entry->start = rasm_add_func(actx, id, export);
    entry->end   = rasm_add_endfunc(actx);
    rasm_set_current_node(actx, entry->start);

    entry->func.export   = export;
    entry->func.label_id = id;

    if (actx->error)
        return actx->error;

    return id;
}

/*********************************************************************/
void rasm_annotate(RasmContext *actx, const char *comment)
{
    if (actx->error || !actx->current_node)
        return;
    RasmNode *current_node = actx->current_node;
    av_freep(&current_node->inline_comment);
    current_node->inline_comment = av_strdup(comment);
    if (!current_node->inline_comment)
        actx->error = AVERROR(ENOMEM);
}

void rasm_annotatef(RasmContext *actx, char *s, size_t n, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(s, n, fmt, args);
    va_end(args);
    return rasm_annotate(actx, s);
}

void rasm_annotate_next(RasmContext *actx, const char *comment)
{
    if (actx->error)
        return;
    av_freep(&actx->comment_next);
    actx->comment_next = av_strdup(comment);
    if (!actx->comment_next)
        actx->error = AVERROR(ENOMEM);
}

void rasm_annotate_nextf(RasmContext *actx, char *s, size_t n, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(s, n, fmt, args);
    va_end(args);
    return rasm_annotate_next(actx, s);
}

int rasm_new_label(RasmContext *actx, const char *name)
{
    if (actx->error)
        return actx->error;

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
    ret = actx->num_labels;

    /* Grow labels array. */
    char **p = av_dynarray2_add((void **) &actx->labels, &actx->num_labels,
                                sizeof(*actx->labels), NULL);
    if (!p) {
        ret = AVERROR(ENOMEM);
        goto error;
    }
    *p = dup;

error:
    if (ret < 0) {
        av_free(dup);
        actx->error = ret;
    }
    return ret;
}

int rasm_new_labelf(RasmContext *actx, char *s, size_t n, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(s, n, fmt, args);
    va_end(args);
    return rasm_new_label(actx, s);
}

/*********************************************************************/
/* AArch64-specific */

void a64op_vec_views(RasmOp op, AArch64VecViews *out)
{
    uint8_t n = a64op_vec_n(op);
    out->b   = a64op_vecb  (n);
    out->h   = a64op_vech  (n);
    out->s   = a64op_vecs  (n);
    out->d   = a64op_vecd  (n);
    out->q   = a64op_vecq  (n);
    out->b8  = a64op_vec8b (n);
    out->b16 = a64op_vec16b(n);
    out->h4  = a64op_vec4h (n);
    out->h8  = a64op_vec8h (n);
    out->s2  = a64op_vec2s (n);
    out->s4  = a64op_vec4s (n);
    out->d2  = a64op_vec2d (n);
    for (int i = 0; i < 2; i++)
        out->be[i] = a64op_elem(out->b, i);
    for (int i = 0; i < 2; i++)
        out->de[i] = a64op_elem(out->d, i);
}
