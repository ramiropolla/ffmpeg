/**
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
AArch64Context *aarch64_alloc(void)
{
    AArch64Context *actx = av_mallocz(sizeof(AArch64Context));
    return actx;
}

void aarch64_free(AArch64Context **p_actx)
{
    AArch64Context *actx = *p_actx;
    if (!actx)
        return;

    for (int i = 0; i < actx->num_entries; i++) {
        AArch64Entry *entry = &actx->entries[i];
        AArch64Node *node = entry->start;
        while (node != NULL) {
            switch (node->type) {
            case AARCH64_NODE_COMMENT:
                av_freep(&node->comment.text);
                break;
            }
            av_freep(&node->inline_comment);
            AArch64Node *cur_node = node;
            node = node->next;
            av_free(cur_node);
        }
    }
    av_freep(&actx->entries);
    for (int i = 0; i < actx->num_labels; i++)
        av_freep(&actx->labels[i]);
    av_freep(&actx->labels);
    av_freep(p_actx);
}

/*********************************************************************/
int aarch64_func_begin(AArch64Context *actx, const char *name, bool export)
{
    if (actx->error)
        return actx->error;

    AArch64Entry *entry = av_dynarray2_add((void **) &actx->entries,
                                           &actx->num_entries,
                                           sizeof(*actx->entries), NULL);
    if (!entry) {
        actx->error = AVERROR(ENOMEM);
        return actx->error;
    }

    entry->type = AARCH64_ENTRY_FUNC;

    int id = aarch64_new_label(actx, name);
    actx->current_node = NULL;
    aarch64_add_func(actx, id, export);
    entry->start = actx->current_node;
    aarch64_add_endfunc(actx);
    entry->end = actx->current_node;

    entry->func.export   = export;
    entry->func.label_id = id;

    actx->current_node = entry->start;

    return id;
}

/*********************************************************************/
static AArch64Node *add_node(AArch64Context *actx, AArch64NodeType type)
{
    if (actx->error)
        return NULL;

    AArch64Node *node = av_mallocz(sizeof(AArch64Node));
    if (!node) {
        actx->error = AVERROR(ENOMEM);
        return NULL;
    }

    node->type = type;

    if (actx->current_node) {
        AArch64Node *next = actx->current_node->next;
        node->prev = actx->current_node;
        node->next = next;
        actx->current_node->next = node;
        if (next)
            next->prev = node;
    }

    actx->current_node = node;

    return node;
}

AArch64Node *aarch64_add_insn(AArch64Context *actx, AArch64InsnId id,
                              AArch64Op op0, AArch64Op op1, AArch64Op op2, AArch64Op op3)
{
    AArch64Node *node = add_node(actx, AARCH64_NODE_INSN);
    if (node) {
        node->insn.id        = id;
        node->insn.op[0]     = op0;
        node->insn.op[1]     = op1;
        node->insn.op[2]     = op2;
        node->insn.op[3]     = op3;
        node->inline_comment = actx->next_comment;
        actx->next_comment   = NULL;
    }
    return node;
}

AArch64Node *aarch64_add_comment(AArch64Context *actx, const char *comment)
{
    AArch64Node *node = add_node(actx, AARCH64_NODE_COMMENT);
    if (node) {
        node->comment.text = av_strdup(comment);
        if (!node->comment.text) {
            actx->error = AVERROR(ENOMEM);
            av_freep(&node);
        }
    }
    return node;
}

AArch64Node *aarch64_add_label(AArch64Context *actx, int id)
{
    AArch64Node *node = add_node(actx, AARCH64_NODE_LABEL);
    if (node) {
        node->label.id = id;
    }
    return node;
}

AArch64Node *aarch64_add_func(AArch64Context *actx, int id, bool export)
{
    AArch64Node *node = add_node(actx, AARCH64_NODE_FUNCTION);
    if (node) {
        assert(id >= 0 && id < actx->num_labels);
        node->func.name   = actx->labels[id];
        node->func.export = export;
    }
    return node;
}

AArch64Node *aarch64_add_endfunc(AArch64Context *actx)
{
    AArch64Node *node = add_node(actx, AARCH64_NODE_ENDFUNC);
    return node;
}

/*********************************************************************/
void aarch64_annotate(AArch64Context *actx, const char *comment)
{
    if (actx->error || !actx->current_node)
        return;
    AArch64Node *node = actx->current_node;
    av_freep(&node->inline_comment);
    node->inline_comment = av_strdup(comment);
    if (!node->inline_comment)
        actx->error = AVERROR(ENOMEM);
}

void aarch64_annotate_next(AArch64Context *actx, const char *comment)
{
    if (actx->error)
        return;
    actx->next_comment = av_strdup(comment);
    if (!actx->next_comment)
        actx->error = AVERROR(ENOMEM);
}

int aarch64_new_label(AArch64Context *actx, const char *name)
{
    char *dup = NULL;
    int ret = -1;

    if (actx->error)
        goto error;

    if (name) {
        dup = av_strdup(name);
        if (!dup) {
            actx->error = AVERROR(ENOMEM);
            goto error;
        }
    }

    ret = actx->num_labels;
    char **p = av_dynarray2_add((void **) &actx->labels, &actx->num_labels,
                                sizeof(*actx->labels), NULL);
    if (!p) {
        actx->error = AVERROR(ENOMEM);
        goto error;
    }
    *p = dup;

error:
    if (actx->error) {
        av_free(dup);
        ret = actx->error;
    }
    return ret;
}

int aarch64_new_labelf(AArch64Context *actx, char *s, size_t n, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(s, n, fmt, args);
    va_end(args);
    return aarch64_new_label(actx, s);
}
