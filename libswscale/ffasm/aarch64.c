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

#include "libavutil/avassert.h"
#include "libavutil/bprint.h"
#include "libavutil/error.h"
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

static void print_gpr(AVBPrint *bp, AArch64GPR gpr)
{
    if (gpr.n == 31) {
        av_bprintf(bp, "%s", gpr.size == sizeof(uint32_t) ? "wsp" : "sp");
        return;
    }

    char c;
    switch (gpr.size) {
    case sizeof(uint32_t): c = 'w'; break;
    case sizeof(uint64_t): c = 'x'; break;
    default: av_unreachable("Invalid GPR size!");
    }
    av_bprintf(bp, "%c%d", c, gpr.n);
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
    av_unreachable("Invalid vector element type!");
}

static void print_vec(AVBPrint *bp, AArch64Vec vec)
{
    if (vec.elem_size == 0) {
        av_bprintf(bp, "v%d", vec.n);
    } else if (vec.elem_count == 0) {
        av_bprintf(bp, "%c%d", elem_type_char(vec.elem_size), vec.n);
    } else {
        av_bprintf(bp, "v%d.%d%c", vec.n, vec.elem_count, elem_type_char(vec.elem_size));
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

    av_freep(&actx->insns.insns);
    av_freep(p_actx);
}

int aarch64_add_insn(AArch64Context *actx, AArch64InsnId id,
                     AArch64Op op0, AArch64Op op1, AArch64Op op2, AArch64Op op3)
{
    if (actx->error)
        return actx->error;

    AArch64Insn *insn = av_dynarray2_add((void **) &actx->insns.insns,
                                         &actx->insns.num_insns,
                                         sizeof(*insn), NULL);
    if (!insn) {
        actx->error = AVERROR(ENOMEM);
        return actx->error;
    }

    insn->id    = id;
    insn->op[0] = op0;
    insn->op[1] = op1;
    insn->op[2] = op2;
    insn->op[3] = op3;

    return 0;
}

static void print_op(AVBPrint *bp, const AArch64Op *op)
{
    switch (op->type) {
    case AARCH64_OP_GPR:
        print_gpr(bp, op->gpr);
        break;
    case AARCH64_OP_VEC:
        print_vec(bp, op->vec);
        break;
    default:
        av_assert0(0);
    }
}

int aarch64_print(AArch64Context *actx, AVBPrint *bp)
{
    for (int i = 0; i < actx->insns.num_insns; i++) {
        const AArch64Insn *insn = &actx->insns.insns[i];
        av_bprintf(bp, "        %-16s", insn_name(insn->id));
        const char *sep = "";
        for (int j = 0; j < 4; j++) {
            const AArch64Op *op = &insn->op[j];
            if (op->type == AARCH64_OP_NONE)
                break;
            av_bprintf(bp, "%s", sep);
            sep = ", ";
            print_op(bp, op);
        }
        av_bprintf(bp, "\n");
    }

    return 0;
}
