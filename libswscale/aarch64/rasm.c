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

#include "libavutil/bprint.h"
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

    for (int i = 0; i < actx->nodes.num_nodes; i++) {
        AArch64Node *node = &actx->nodes.nodes[i];
        switch (node->type) {
        case AARCH64_NODE_INSN:
            av_freep(&node->insn.comment);
            break;
        case AARCH64_NODE_COMMENT:
            av_freep(&node->comment.text);
            break;
        }
    }
    av_freep(&actx->nodes.nodes);
    for (int i = 0; i < actx->num_labels; i++)
        av_freep(&actx->labels[i]);
    av_freep(&actx->labels);
    av_freep(p_actx);
}

static AArch64Node *add_node(AArch64Context *actx, AArch64NodeType type)
{
    if (actx->error)
        return NULL;

    AArch64Node *node = av_dynarray2_add((void **) &actx->nodes.nodes,
                                         &actx->nodes.num_nodes,
                                         sizeof(*node), NULL);
    if (!node) {
        actx->error = AVERROR(ENOMEM);
        return NULL;
    }

    node->type = type;

    return node;
}

int aarch64_add_insn(AArch64Context *actx, AArch64InsnId id,
                     AArch64Op op0, AArch64Op op1, AArch64Op op2, AArch64Op op3)
{
    AArch64Node *node = add_node(actx, AARCH64_NODE_INSN);
    if (!node)
        return actx->error;

    node->insn.id      = id;
    node->insn.op[0]   = op0;
    node->insn.op[1]   = op1;
    node->insn.op[2]   = op2;
    node->insn.op[3]   = op3;
    node->insn.comment = NULL;

    return 0;
}

int aarch64_add_comment(AArch64Context *actx, const char *comment)
{
    AArch64Node *node = add_node(actx, AARCH64_NODE_COMMENT);
    if (!node)
        return actx->error;

    node->comment.text = av_strdup(comment);
    if (!node->comment.text) {
        actx->error = AVERROR(ENOMEM);
        return actx->error;
    }

    return 0;
}

void aarch64_annotate(AArch64Context *actx, const char *comment)
{
    if (actx->error || actx->nodes.num_nodes == 0)
        return;
    AArch64Node *node = &actx->nodes.nodes[actx->nodes.num_nodes - 1];
    if (node->type != AARCH64_NODE_INSN)
        return;
    av_freep(&node->insn.comment);
    node->insn.comment = av_strdup(comment);
    if (!node->insn.comment)
        actx->error = AVERROR(ENOMEM);
}

int aarch64_new_label(AArch64Context *actx, const char *name)
{
    if (actx->error)
        return actx->error;

    char *dup = av_strdup(name);
    if (!dup) {
        actx->error = AVERROR(ENOMEM);
        return actx->error;
    }

    int id = actx->num_labels;
    char **p = av_dynarray2_add((void **) &actx->labels, &actx->num_labels,
                                sizeof(*actx->labels), NULL);
    if (!p) {
        av_free(dup);
        actx->error = AVERROR(ENOMEM);
        return actx->error;
    }
    *p = dup;

    return id;
}

int aarch64_add_label(AArch64Context *actx, int id)
{
    AArch64Node *node = add_node(actx, AARCH64_NODE_LABEL);
    if (!node)
        return actx->error;

    assert(id >= 0 && id < actx->num_labels);
    node->label.name = actx->labels[id];

    return 0;
}

int aarch64_new_labelf(AArch64Context *actx, char *s, size_t n, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(s, n, fmt, args);
    va_end(args);
    return aarch64_new_label(actx, s);
}

int aarch64_add_func(AArch64Context *actx, int id, bool export)
{
    AArch64Node *node = add_node(actx, AARCH64_NODE_FUNCTION);
    if (!node)
        return actx->error;

    assert(id >= 0 && id < actx->num_labels);
    node->func.name   = actx->labels[id];
    node->func.export = export;

    return 0;
}

int aarch64_add_endfunc(AArch64Context *actx)
{
    AArch64Node *node = add_node(actx, AARCH64_NODE_ENDFUNC);
    if (!node)
        return actx->error;
    return 0;
}
