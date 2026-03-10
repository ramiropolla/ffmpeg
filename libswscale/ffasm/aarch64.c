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

#include "aarch64.h"

#include <stdarg.h>

#include "libavutil/avassert.h"
#include "libavutil/bprint.h"
#include "libavutil/error.h"
#include "libavutil/macros.h"
#include "libavutil/mem.h"

/*********************************************************************/
static const char *insn_name(AArch64InsnId id)
{
    static const char insn_names[AARCH64_INSN_NB][8] = {
        [AARCH64_INSN_ADD   ] = "add",
        [AARCH64_INSN_ADDV  ] = "addv",
        [AARCH64_INSN_ADR   ] = "adr",
        [AARCH64_INSN_AND   ] = "and",
        [AARCH64_INSN_B     ] = "b",
        [AARCH64_INSN_BR    ] = "br",
        [AARCH64_INSN_CMP   ] = "cmp",
        [AARCH64_INSN_CSEL  ] = "csel",
        [AARCH64_INSN_DUP   ] = "dup",
        [AARCH64_INSN_FADD  ] = "fadd",
        [AARCH64_INSN_FCVTZU] = "fcvtzu",
        [AARCH64_INSN_FMAX  ] = "fmax",
        [AARCH64_INSN_FMIN  ] = "fmin",
        [AARCH64_INSN_FMLA  ] = "fmla",
        [AARCH64_INSN_FMUL  ] = "fmul",
        [AARCH64_INSN_INS   ] = "ins",
        [AARCH64_INSN_LD1   ] = "ld1",
        [AARCH64_INSN_LD2   ] = "ld2",
        [AARCH64_INSN_LD3   ] = "ld3",
        [AARCH64_INSN_LD4   ] = "ld4",
        [AARCH64_INSN_LDP   ] = "ldp",
        [AARCH64_INSN_LDR   ] = "ldr",
        [AARCH64_INSN_LDRH  ] = "ldrh",
        [AARCH64_INSN_LSR   ] = "lsr",
        [AARCH64_INSN_MOV   ] = "mov",
        [AARCH64_INSN_MOVI  ] = "movi",
        [AARCH64_INSN_MUL   ] = "mul",
        [AARCH64_INSN_ORR   ] = "orr",
        [AARCH64_INSN_RET   ] = "ret",
        [AARCH64_INSN_REV16 ] = "rev16",
        [AARCH64_INSN_REV32 ] = "rev32",
        [AARCH64_INSN_SHL   ] = "shl",
        [AARCH64_INSN_ST1   ] = "st1",
        [AARCH64_INSN_ST2   ] = "st2",
        [AARCH64_INSN_ST3   ] = "st3",
        [AARCH64_INSN_ST4   ] = "st4",
        [AARCH64_INSN_STP   ] = "stp",
        [AARCH64_INSN_STR   ] = "str",
        [AARCH64_INSN_SUB   ] = "sub",
        [AARCH64_INSN_SUBS  ] = "subs",
        [AARCH64_INSN_TBL   ] = "tbl",
        [AARCH64_INSN_UBFIZ ] = "ubfiz",
        [AARCH64_INSN_UCVTF ] = "ucvtf",
        [AARCH64_INSN_UMIN  ] = "umin",
        [AARCH64_INSN_UQXTN ] = "uqxtn",
        [AARCH64_INSN_USHL  ] = "ushl",
        [AARCH64_INSN_USHLL ] = "ushll",
        [AARCH64_INSN_USHLL2] = "ushll2",
        [AARCH64_INSN_USHR  ] = "ushr",
        [AARCH64_INSN_UXTL  ] = "uxtl",
        [AARCH64_INSN_UXTL2 ] = "uxtl2",
        [AARCH64_INSN_XTN   ] = "xtn",
        [AARCH64_INSN_ZIP1  ] = "zip1",
        [AARCH64_INSN_ZIP2  ] = "zip2",
    };
    if (id == AARCH64_INSN_NONE || id >= AARCH64_INSN_NB)
        return NULL;
    return insn_names[id];
}

static const char *cond_name(uint8_t cond)
{
    static const char cond_names[16][3] = {
        [AARCH64_EQ] = "eq",
        [AARCH64_NE] = "ne",
        [AARCH64_HS] = "hs",
        [AARCH64_LO] = "lo",
        [AARCH64_MI] = "mi",
        [AARCH64_PL] = "pl",
        [AARCH64_VS] = "vs",
        [AARCH64_VC] = "vc",
        [AARCH64_HI] = "hi",
        [AARCH64_LS] = "ls",
        [AARCH64_GE] = "ge",
        [AARCH64_LT] = "lt",
        [AARCH64_GT] = "gt",
        [AARCH64_LE] = "le",
        [AARCH64_AL] = "al",
        [AARCH64_NV] = "nv",
    };
    return cond_names[cond & 0xf];
}

static const char *extend_name(uint8_t extend)
{
    switch (extend) {
    case AARCH64_EXTEND_UXTB: return "uxtb";
    case AARCH64_EXTEND_UXTH: return "uxth";
    case AARCH64_EXTEND_UXTW: return "uxtw";
    case AARCH64_EXTEND_UXTX: return "lsl";
    case AARCH64_EXTEND_SXTB: return "sxtb";
    case AARCH64_EXTEND_SXTH: return "sxth";
    case AARCH64_EXTEND_SXTW: return "sxtw";
    case AARCH64_EXTEND_SXTX: return "sxtx";
    default:                  return NULL;
    }
}

static void aarch64_print_gpr(AVBPrint *bp, AArch64Op op)
{
    uint8_t n = a64op_gpr_n(op);
    uint8_t size = a64op_gpr_size(op);

    if (n == 31) {
        av_bprintf(bp, "%s", size == sizeof(uint32_t) ? "wsp" : "sp");
        return;
    }

    switch (size) {
    case sizeof(uint32_t): av_bprintf(bp, "w%d", n); break;
    case sizeof(uint64_t): av_bprintf(bp, "x%d", n); break;
    default: av_assert0(!"Invalid GPR size!");
    }

    uint8_t ext = a64op_gpr_ext(op);
    if (ext != AARCH64_EXTEND_NONE) {
        uint8_t sh = a64op_gpr_sh(op);
        if (sh)
            av_bprintf(bp, ", %s #%d", extend_name(ext), sh);
        else
            av_bprintf(bp, ", %s", extend_name(ext));
    }
}

static char elem_type_char(uint8_t elem_size)
{
    switch (elem_size) {
    case  1: return 'b';
    case  2: return 'h';
    case  4: return 's';
    case  8: return 'd';
    case 16: return 'q';
    }
    av_assert0(!"Invalid vector element type!");
    return '\0';
}

static void print_base_reg(AVBPrint *bp, uint8_t n)
{
    if (n == 31)
        av_bprintf(bp, "sp");
    else
        av_bprintf(bp, "x%d", n);
}

static void aarch64_print_base(AVBPrint *bp, AArch64Op op)
{
    uint8_t n = a64op_base_n(op);
    uint8_t mode = a64op_base_mode(op);
    int16_t imm = a64op_base_imm(op);

    switch (mode) {
    case AARCH64_BASE_OFFSET: {
        av_bprintf(bp, "[");
        print_base_reg(bp, n);
        if (imm)
            av_bprintf(bp, ", #%d]", imm);
        else
            av_bprintf(bp, "]");
        break;
    }
    case AARCH64_BASE_PRE:
        av_bprintf(bp, "[");
        print_base_reg(bp, n);
        av_bprintf(bp, ", #%d]!", imm);
        break;
    case AARCH64_BASE_POST:
        av_bprintf(bp, "[");
        print_base_reg(bp, n);
        av_bprintf(bp, "], #%d", imm);
        break;
    case AARCH64_BASE_REG: {
        uint8_t m   = a64op_base_m(op);
        uint8_t ext = a64op_base_ext(op);
        uint8_t sh  = a64op_base_sh(op);
        av_bprintf(bp, "[");
        print_base_reg(bp, n);
        if (sh)
            av_bprintf(bp, ", x%d, %s #%d]", m, extend_name(ext), sh);
        else
            av_bprintf(bp, ", x%d, %s]", m, extend_name(ext));
        break;
    }
    }
}

static void aarch64_print_vec_single(AVBPrint *bp, uint8_t n,
                                     uint8_t el_count, uint8_t el_size)
{
    if (el_size == 0)
        av_bprintf(bp, "v%u", n);
    else if (el_count == 0)
        av_bprintf(bp, "%c%u", elem_type_char(el_size), n);
    else
        av_bprintf(bp, "v%u.%d%c", n, el_count, elem_type_char(el_size));
}

static void aarch64_print_vec(AVBPrint *bp, AArch64Op op)
{
    uint8_t n        = a64op_vec_n(op);
    uint8_t el_count = a64op_vec_el_count(op);
    uint8_t el_size  = a64op_vec_el_size(op);
    uint8_t num_regs = a64op_vec_num_regs(op);

    if (num_regs >= 2) {
        av_bprintf(bp, "{");
        for (int i = 0; i < num_regs; i++) {
            if (i > 0)
                av_bprintf(bp, ", ");
            aarch64_print_vec_single(bp, (n + i) % 32, el_count, el_size);
        }
        av_bprintf(bp, "}");
    } else {
        aarch64_print_vec_single(bp, n, el_count, el_size);
    }
}

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

    av_assert0(id >= 0 && id < actx->num_labels);
    node->label.name = actx->labels[id];

    return 0;
}

#if 0
int aarch64_new_labelf(AArch64Context *actx, char *s, size_t n, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(s, n, fmt, args);
    va_end(args);
    return aarch64_new_label(actx, s);
}
#endif

int aarch64_add_func(AArch64Context *actx, int id, bool export)
{
    AArch64Node *node = add_node(actx, AARCH64_NODE_FUNCTION);
    if (!node)
        return actx->error;

    av_assert0(id >= 0 && id < actx->num_labels);
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

static void print_op(const AArch64Context *actx, AVBPrint *bp, AArch64Op op)
{
    switch (a64op_type(op)) {
    case AARCH64_OP_GPR:
        aarch64_print_gpr(bp, op);
        break;
    case AARCH64_OP_VEC:
        aarch64_print_vec(bp, op);
        break;
    case AARCH64_OP_BASE:
        aarch64_print_base(bp, op);
        break;
    case AARCH64_OP_IMM:
        av_bprintf(bp, "#%d", a64op_imm_val(op));
        break;
    case AARCH64_OP_COND:
        av_bprintf(bp, "%s", cond_name(a64op_cond_val(op)));
        break;
    case AARCH64_OP_LABEL: {
        int id = a64op_label_id(op);
        av_assert0(id >= 0 && id < actx->num_labels);
        av_bprintf(bp, "%s", actx->labels[id]);
        break;
    }
    default:
        av_assert0(0);
    }
}

static void indent_to(AVBPrint *bp, int line_start, int col)
{
    int cur_col = bp->len - line_start;
    av_bprintf(bp, "%*s", FFMAX(col - cur_col, 1), "");
}

int aarch64_print(AArch64Context *actx, AVBPrint *bp)
{
    const int instr_indent = 8;
    const int comment_col = 56;

    for (int i = 0; i < actx->nodes.num_nodes; i++) {
        const AArch64Node *node = &actx->nodes.nodes[i];
        size_t line_start = bp->len;

        switch (node->type) {
        case AARCH64_NODE_COMMENT:
            indent_to(bp, line_start, instr_indent);
            av_bprintf(bp, "// %s\n", node->comment.text);
            break;
        case AARCH64_NODE_INSN: {
            indent_to(bp, line_start, instr_indent);

            int op_start = 0;
            if (node->insn.id == AARCH64_INSN_B && a64op_type(node->insn.op[0]) == AARCH64_OP_COND) {
                av_bprintf(bp, "b.%-14s", cond_name(a64op_cond_val(node->insn.op[0])));
                op_start = 1;
            } else {
                av_bprintf(bp, "%-16s", insn_name(node->insn.id));
            }

            for (int j = op_start; j < 4; j++) {
                AArch64Op op = node->insn.op[j];
                if (a64op_type(op) == AARCH64_OP_NONE)
                    break;
                if (j != op_start)
                    av_bprintf(bp, "%s", ", ");
                print_op(actx, bp, op);
            }

            if (node->insn.comment) {
                indent_to(bp, line_start, comment_col);
                av_bprintf(bp, "// %s", node->insn.comment);
            }
            av_bprintf(bp, "\n");

            break;
        }
        case AARCH64_NODE_LABEL:
            av_bprintf(bp, "%s:\n", node->label.name);
            break;
        case AARCH64_NODE_FUNCTION:
            av_bprintf(bp, "function %s, export=%d\n", node->func.name, node->func.export);
            break;
        case AARCH64_NODE_ENDFUNC:
            av_bprintf(bp, "endfunc\n");
            break;
        default:
            break;
        }
    }

    return 0;
}
