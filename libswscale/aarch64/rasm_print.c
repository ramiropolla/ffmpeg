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

#include <stdarg.h>

#include "rasm.h"

#if 0
#include "libavutil/macros.h"
#include "libavutil/mem.h"
#endif

/*********************************************************************/
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

static const char *insn_name(AArch64InsnId id)
{
    if (id == AARCH64_INSN_NONE || id >= AARCH64_INSN_NB)
        return NULL;
    return insn_names[id];
}

/*********************************************************************/
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

static const char *cond_name(uint8_t cond)
{
    return cond_names[cond & 0xf];
}

/*********************************************************************/
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
    default: return NULL;
    }
}

static void print_gpr(FILE *fp, AArch64Op op)
{
    uint8_t n = a64op_gpr_n(op);
    uint8_t size = a64op_gpr_size(op);

    if (n == 31) {
        fprintf(fp, "%s", size == sizeof(uint32_t) ? "wsp" : "sp");
        return;
    }

    switch (size) {
    case sizeof(uint32_t): fprintf(fp, "w%d", n); break;
    case sizeof(uint64_t): fprintf(fp, "x%d", n); break;
    default: assert(!"Invalid GPR size!");
    }

    uint8_t ext = a64op_gpr_ext(op);
    if (ext != AARCH64_EXTEND_NONE) {
        uint8_t sh = a64op_gpr_sh(op);
        if (sh)
            fprintf(fp, ", %s #%d", extend_name(ext), sh);
        else
            fprintf(fp, ", %s", extend_name(ext));
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
    assert(!"Invalid vector element type!");
    return '\0';
}

static void print_base_reg(FILE *fp, uint8_t n)
{
    if (n == 31)
        fprintf(fp, "sp");
    else
        fprintf(fp, "x%d", n);
}

static void print_base(FILE *fp, AArch64Op op)
{
    uint8_t n = a64op_base_n(op);
    uint8_t mode = a64op_base_mode(op);
    int16_t imm = a64op_base_imm(op);

    switch (mode) {
    case AARCH64_BASE_OFFSET: {
        fprintf(fp, "[");
        print_base_reg(fp, n);
        if (imm)
            fprintf(fp, ", #%d]", imm);
        else
            fprintf(fp, "]");
        break;
    }
    case AARCH64_BASE_PRE:
        fprintf(fp, "[");
        print_base_reg(fp, n);
        fprintf(fp, ", #%d]!", imm);
        break;
    case AARCH64_BASE_POST:
        fprintf(fp, "[");
        print_base_reg(fp, n);
        fprintf(fp, "], #%d", imm);
        break;
    case AARCH64_BASE_REG: {
        uint8_t m   = a64op_base_m(op);
        uint8_t ext = a64op_base_ext(op);
        uint8_t sh  = a64op_base_sh(op);
        fprintf(fp, "[");
        print_base_reg(fp, n);
        if (sh)
            fprintf(fp, ", x%d, %s #%d]", m, extend_name(ext), sh);
        else
            fprintf(fp, ", x%d, %s]", m, extend_name(ext));
        break;
    }
    }
}

static void print_vec_reg(FILE *fp, uint8_t n, uint8_t el_count, uint8_t el_size, uint8_t idx_p1)
{
    if (el_size == 0) {
        fprintf(fp, "v%u", n);
    } else if (el_count != 0) {
        fprintf(fp, "v%u.%d%c", n, el_count, elem_type_char(el_size));
    } else if (idx_p1) {
        fprintf(fp, "v%u.%c[%u]", n, elem_type_char(el_size), idx_p1 - 1);
    } else {
        fprintf(fp, "%c%u", elem_type_char(el_size), n);
    }
}

static void print_vec(FILE *fp, AArch64Op op)
{
    uint8_t n        = a64op_vec_n(op);
    uint8_t el_count = a64op_vec_el_count(op);
    uint8_t el_size  = a64op_vec_el_size(op);
    uint8_t num_regs = a64op_vec_num_regs(op);

    if (num_regs >= 2) {
        fprintf(fp, "{");
        for (int i = 0; i < num_regs; i++) {
            if (i > 0)
                fprintf(fp, ", ");
            print_vec_reg(fp, (n + i) & 0x1f, el_count, el_size, 0);
        }
        fprintf(fp, "}");
    } else {
        uint8_t idx_p1 = a64op_vec_idx_p1(op);
        print_vec_reg(fp, n, el_count, el_size, idx_p1);
    }
}

static void print_op(const AArch64Context *actx, FILE *fp, AArch64Op op)
{
    switch (a64op_type(op)) {
    case AARCH64_OP_GPR:
        print_gpr(fp, op);
        break;
    case AARCH64_OP_VEC:
        print_vec(fp, op);
        break;
    case AARCH64_OP_BASE:
        print_base(fp, op);
        break;
    case AARCH64_OP_IMM:
        fprintf(fp, "#%d", a64op_imm_val(op));
        break;
    case AARCH64_OP_COND:
        fprintf(fp, "%s", cond_name(a64op_cond_val(op)));
        break;
    case AARCH64_OP_LABEL: {
        int id = a64op_label_id(op);
        assert(id >= 0 && id < actx->num_labels);
        fprintf(fp, "%s", actx->labels[id]);
        break;
    }
    default:
        assert(0);
    }
}

static void indent_to(FILE *fp, int line_start, int col)
{
    int cur_col = ftell(fp) - line_start;
    fprintf(fp, "%*s", FFMAX(col - cur_col, 1), "");
}

int aarch64_print(AArch64Context *actx, FILE *fp)
{
    const int instr_indent = 8;
    const int comment_col = 56;

    for (int i = 0; i < actx->nodes.num_nodes; i++) {
        const AArch64Node *node = &actx->nodes.nodes[i];
        size_t line_start = ftell(fp);

        switch (node->type) {
        case AARCH64_NODE_COMMENT:
            indent_to(fp, line_start, instr_indent);
            fprintf(fp, "// %s\n", node->comment.text);
            break;
        case AARCH64_NODE_INSN: {
            indent_to(fp, line_start, instr_indent);

            int op_start = 0;
            if (node->insn.id == AARCH64_INSN_B && a64op_type(node->insn.op[0]) == AARCH64_OP_COND) {
                fprintf(fp, "b.%-14s", cond_name(a64op_cond_val(node->insn.op[0])));
                op_start = 1;
            } else if (node->insn.id == AARCH64_INSN_RET) {
                fprintf(fp, "%s", insn_name(node->insn.id));
            } else {
                fprintf(fp, "%-16s", insn_name(node->insn.id));
            }

            for (int j = op_start; j < 4; j++) {
                AArch64Op op = node->insn.op[j];
                if (a64op_type(op) == AARCH64_OP_NONE)
                    break;
                if (j != op_start)
                    fprintf(fp, "%s", ", ");
                print_op(actx, fp, op);
            }

            if (node->insn.comment) {
                indent_to(fp, line_start, comment_col);
                fprintf(fp, "// %s", node->insn.comment);
            }
            fprintf(fp, "\n");

            break;
        }
        case AARCH64_NODE_LABEL:
            fprintf(fp, "%s:\n", node->label.name);
            break;
        case AARCH64_NODE_FUNCTION:
            fprintf(fp, "function %s, export=%d\n", node->func.name, node->func.export);
            break;
        case AARCH64_NODE_ENDFUNC:
            fprintf(fp, "endfunc\n");
            fprintf(fp, "\n");
            break;
        default:
            break;
        }
    }

    return 0;
}
