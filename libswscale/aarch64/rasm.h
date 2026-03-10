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

#ifndef AARCH64_RASM_H
#define AARCH64_RASM_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

// TODO prefixes:
// i_:  instructions
// v_:  vector operand modifiers
// ve_: vector element operand modifiers
// vv_: vector list modifiers

/*********************************************************************/
/* Currently supported AArch64 instruction IDs */

typedef enum AArch64InsnId {
    AARCH64_INSN_NONE = 0,

    AARCH64_INSN_ADD,
    AARCH64_INSN_ADDV,
    AARCH64_INSN_ADR,
    AARCH64_INSN_AND,
    AARCH64_INSN_B,
    AARCH64_INSN_BR,
    AARCH64_INSN_CMP,
    AARCH64_INSN_CSEL,
    AARCH64_INSN_DUP,
    AARCH64_INSN_FADD,
    AARCH64_INSN_FCVTZU,
    AARCH64_INSN_FMAX,
    AARCH64_INSN_FMIN,
    AARCH64_INSN_FMLA,
    AARCH64_INSN_FMUL,
    AARCH64_INSN_INS,
    AARCH64_INSN_LD1,
    AARCH64_INSN_LD1R,
    AARCH64_INSN_LD2,
    AARCH64_INSN_LD3,
    AARCH64_INSN_LD4,
    AARCH64_INSN_LDP,
    AARCH64_INSN_LDR,
    AARCH64_INSN_LDRB,
    AARCH64_INSN_LDRH,
    AARCH64_INSN_LSR,
    AARCH64_INSN_MOV,
    AARCH64_INSN_MOVI,
    AARCH64_INSN_MUL,
    AARCH64_INSN_ORR,
    AARCH64_INSN_RET,
    AARCH64_INSN_REV16,
    AARCH64_INSN_REV32,
    AARCH64_INSN_SHL,
    AARCH64_INSN_ST1,
    AARCH64_INSN_ST2,
    AARCH64_INSN_ST3,
    AARCH64_INSN_ST4,
    AARCH64_INSN_STP,
    AARCH64_INSN_STR,
    AARCH64_INSN_SUB,
    AARCH64_INSN_SUBS,
    AARCH64_INSN_TBL,
    AARCH64_INSN_UBFIZ,
    AARCH64_INSN_UCVTF,
    AARCH64_INSN_UMAX,
    AARCH64_INSN_UMIN,
    AARCH64_INSN_UQXTN,
    AARCH64_INSN_USHL,
    AARCH64_INSN_USHLL,
    AARCH64_INSN_USHLL2,
    AARCH64_INSN_USHR,
    AARCH64_INSN_UXTL,
    AARCH64_INSN_UXTL2,
    AARCH64_INSN_XTN,
    AARCH64_INSN_ZIP1,
    AARCH64_INSN_ZIP2,

    AARCH64_INSN_NB,
} AArch64InsnId;

/*********************************************************************/
/* AArch64 condition codes */

#define AARCH64_EQ  0x0
#define AARCH64_NE  0x1
#define AARCH64_HS  0x2
#define AARCH64_CS  AARCH64_HS
#define AARCH64_LO  0x3
#define AARCH64_CC  AARCH64_LO
#define AARCH64_MI  0x4
#define AARCH64_PL  0x5
#define AARCH64_VS  0x6
#define AARCH64_VC  0x7
#define AARCH64_HI  0x8
#define AARCH64_LS  0x9
#define AARCH64_GE  0xa
#define AARCH64_LT  0xb
#define AARCH64_GT  0xc
#define AARCH64_LE  0xd
#define AARCH64_AL  0xe
#define AARCH64_NV  0xf

/*********************************************************************/

typedef enum AArch64OpType {
    AARCH64_OP_NONE = 0,

    AARCH64_OP_GPR,
    AARCH64_OP_VEC,
    AARCH64_OP_IMM,
    AARCH64_OP_BASE,
    AARCH64_OP_LABEL,
    AARCH64_OP_COND,

    AARCH64_OP_NB,
} AArch64OpType;

#define AARCH64_EXTEND_NONE 0
#define AARCH64_EXTEND_UXTB 1
#define AARCH64_EXTEND_UXTH 2
#define AARCH64_EXTEND_UXTW 3
#define AARCH64_EXTEND_UXTX 4
#define AARCH64_EXTEND_SXTB 5
#define AARCH64_EXTEND_SXTH 6
#define AARCH64_EXTEND_SXTW 7
#define AARCH64_EXTEND_SXTX 8

typedef union AArch64Op {
    uint8_t  u8 [8];
    uint16_t u16[4];
    uint32_t u32[2];
    uint64_t u64;
} AArch64Op;

/*********************************************************************/
/* nodes */

typedef enum AArch64NodeType {
    AARCH64_NODE_INSN,
    AARCH64_NODE_COMMENT,
    AARCH64_NODE_LABEL,
    AARCH64_NODE_FUNCTION,
    AARCH64_NODE_ENDFUNC,
    AARCH64_NODE_DATA,
} AArch64NodeType;

typedef struct AArch64NodeInsn {
    AArch64InsnId id;
    AArch64Op op[4];
} AArch64NodeInsn;

typedef struct AArch64NodeComment {
    char *text;
} AArch64NodeComment;

typedef struct AArch64NodeLabel {
    int id;
} AArch64NodeLabel;

typedef struct AArch64NodeFunc {
    char *name;
    bool export;
} AArch64NodeFunc;

typedef struct AArch64Node {
    AArch64NodeType type;
    union {
        AArch64NodeInsn    insn;
        AArch64NodeComment comment;
        AArch64NodeLabel   label;
        AArch64NodeFunc    func;
    };
    char *inline_comment;
    struct AArch64Node *prev;
    struct AArch64Node *next;
} AArch64Node;

/*********************************************************************/
typedef struct AArch64Function {
    bool export;
    int label_id;
} AArch64Function;

typedef enum AArch64EntryType {
    AARCH64_ENTRY_FUNC,
    AARCH64_ENTRY_DATA,
} AArch64EntryType;

typedef struct AArch64Entry {
    AArch64EntryType type;
    AArch64Node *start;
    AArch64Node *end;
    union {
        AArch64Function func;
    };
} AArch64Entry;

/*********************************************************************/
typedef struct AArch64Context {
    AArch64Entry *entries;
    int num_entries;
    AArch64Node *current_node;
    int error;
    char **labels;
    int num_labels;
    char *next_comment;
} AArch64Context;

AArch64Context *aarch64_alloc(void);

void aarch64_free(AArch64Context **p_actx);

int aarch64_func_begin(AArch64Context *actx, const char *name, bool export);

AArch64Node *aarch64_add_insn(AArch64Context *actx, AArch64InsnId id,
                              AArch64Op op0, AArch64Op op1, AArch64Op op2, AArch64Op op3);
AArch64Node *aarch64_add_comment(AArch64Context *actx, const char *comment);
AArch64Node *aarch64_add_commentf(AArch64Context *actx, char *s, size_t n, const char *fmt, ...);
AArch64Node *aarch64_add_label(AArch64Context *actx, int id);
AArch64Node *aarch64_add_func(AArch64Context *actx, int id, bool export);
AArch64Node *aarch64_add_endfunc(AArch64Context *actx);

int aarch64_new_label(AArch64Context *actx, const char *name);
int aarch64_new_labelf(AArch64Context *actx, char *s, size_t n, const char *fmt, ...) /* av_printf_format(4, 5) */;

void aarch64_annotate(AArch64Context *actx, const char *comment);
void aarch64_annotatef(AArch64Context *actx, char *s, size_t n, const char *fmt, ...);
void aarch64_annotate_next(AArch64Context *actx, const char *comment);
void aarch64_annotate_nextf(AArch64Context *actx, char *s, size_t n, const char *fmt, ...);

#define inlcmt(actx, comment) aarch64_annotate(actx, comment)
#define inlcmtf(actx, fmt, ...) aarch64_annotatef(actx, (char[128]){0}, 128, fmt, __VA_ARGS__)

int aarch64_print(AArch64Context *actx, FILE *fp);

/*********************************************************************/
/* Accessors */

static inline uint8_t a64op_type(AArch64Op op) { return op.u8[0]; }

/*********************************************************************/
/* AARCH64_OP_NONE */

static inline AArch64Op a64op_none(void) { return (AArch64Op) { 0 }; }

#define OPN a64op_none()

/*********************************************************************/
/* AARCH64_OP_COND */

static inline AArch64Op a64op_cond(uint8_t cond)
{
    AArch64Op op = { 0 };
    op.u8[0] = AARCH64_OP_COND;
    op.u8[1] = cond;
    return op;
}

/* getter */
static inline uint8_t a64op_cond_val(AArch64Op op) { return op.u8[1]; }

static inline AArch64Op a64cond_eq(void) { return a64op_cond(AARCH64_EQ); }
static inline AArch64Op a64cond_ne(void) { return a64op_cond(AARCH64_NE); }
static inline AArch64Op a64cond_hs(void) { return a64op_cond(AARCH64_HS); }
static inline AArch64Op a64cond_cs(void) { return a64op_cond(AARCH64_CS); }
static inline AArch64Op a64cond_lo(void) { return a64op_cond(AARCH64_LO); }
static inline AArch64Op a64cond_cc(void) { return a64op_cond(AARCH64_CC); }
static inline AArch64Op a64cond_mi(void) { return a64op_cond(AARCH64_MI); }
static inline AArch64Op a64cond_pl(void) { return a64op_cond(AARCH64_PL); }
static inline AArch64Op a64cond_vs(void) { return a64op_cond(AARCH64_VS); }
static inline AArch64Op a64cond_vc(void) { return a64op_cond(AARCH64_VC); }
static inline AArch64Op a64cond_hi(void) { return a64op_cond(AARCH64_HI); }
static inline AArch64Op a64cond_ls(void) { return a64op_cond(AARCH64_LS); }
static inline AArch64Op a64cond_ge(void) { return a64op_cond(AARCH64_GE); }
static inline AArch64Op a64cond_lt(void) { return a64op_cond(AARCH64_LT); }
static inline AArch64Op a64cond_gt(void) { return a64op_cond(AARCH64_GT); }
static inline AArch64Op a64cond_le(void) { return a64op_cond(AARCH64_LE); }
static inline AArch64Op a64cond_al(void) { return a64op_cond(AARCH64_AL); }
static inline AArch64Op a64cond_nv(void) { return a64op_cond(AARCH64_NV); }

/*********************************************************************/
/* AARCH64_OP_LABEL */

static inline AArch64Op a64op_label(int id)
{
    AArch64Op op = { 0 };
    op.u8[0]  = AARCH64_OP_LABEL;
    op.u32[1] = (uint32_t)id;
    return op;
}

/* getter */
static inline int a64op_label_id(AArch64Op op) { return (int)op.u32[1]; }

/*********************************************************************/
/* AARCH64_OP_IMM */

static inline AArch64Op a64op_imm(int32_t imm)
{
    AArch64Op op = { 0 };
    op.u8[0]  = AARCH64_OP_IMM;
    op.u32[1] = (uint32_t)imm;
    return op;
}

/* getter */
static inline int32_t a64op_imm_val(AArch64Op op) { return (int32_t)op.u32[1]; }

/*********************************************************************/
/* AARCH64_OP_GPR */

static inline AArch64Op a64op_make_gpr(uint8_t n, uint8_t size)
{
    AArch64Op op = { 0 };
    op.u8[0] = AARCH64_OP_GPR;
    op.u8[1] = n;
    op.u8[2] = size;
    return op;
}

/* getters */
static inline uint8_t a64op_gpr_n   (AArch64Op op) { return op.u8[1]; }
static inline uint8_t a64op_gpr_size(AArch64Op op) { return op.u8[2]; }
static inline uint8_t a64op_gpr_ext (AArch64Op op) { return op.u8[3]; }
static inline uint8_t a64op_gpr_sh  (AArch64Op op) { return op.u8[4]; }

static inline AArch64Op a64op_gpw(uint8_t n) { return a64op_make_gpr(n, sizeof(uint32_t)); }
static inline AArch64Op a64op_gpx(uint8_t n) { return a64op_make_gpr(n, sizeof(uint64_t)); }
static inline AArch64Op a64op_sp (void)       { return a64op_make_gpr(31, sizeof(uint64_t)); }

/* modifiers */
static inline AArch64Op a64op_w(AArch64Op op) { return a64op_gpw(a64op_gpr_n(op)); }
static inline AArch64Op a64op_x(AArch64Op op) { return a64op_gpx(a64op_gpr_n(op)); }

/*********************************************************************/
/* AARCH64_OP_VEC */

static inline AArch64Op a64op_make_vec(uint8_t n, uint8_t el_count, uint8_t el_size)
{
    AArch64Op op = { 0 };
    op.u8[0] = AARCH64_OP_VEC;
    op.u8[1] = n;
    op.u8[2] = el_count;
    op.u8[3] = el_size;
    return op;
}

/* getters */
static inline uint8_t a64op_vec_n       (AArch64Op op) { return op.u8[1]; }
static inline uint8_t a64op_vec_el_count(AArch64Op op) { return op.u8[2]; }
static inline uint8_t a64op_vec_el_size (AArch64Op op) { return op.u8[3]; }
static inline uint8_t a64op_vec_num_regs(AArch64Op op) { return op.u8[4]; }
static inline uint8_t a64op_vec_idx_p1  (AArch64Op op) { return op.u8[5]; }

static inline AArch64Op a64op_vec   (uint8_t n) { return a64op_make_vec(n,  0,  0); }
static inline AArch64Op a64op_vecb  (uint8_t n) { return a64op_make_vec(n,  0,  1); }
static inline AArch64Op a64op_vech  (uint8_t n) { return a64op_make_vec(n,  0,  2); }
static inline AArch64Op a64op_vecs  (uint8_t n) { return a64op_make_vec(n,  0,  4); }
static inline AArch64Op a64op_vecd  (uint8_t n) { return a64op_make_vec(n,  0,  8); }
static inline AArch64Op a64op_vecq  (uint8_t n) { return a64op_make_vec(n,  0, 16); }
static inline AArch64Op a64op_vec8b (uint8_t n) { return a64op_make_vec(n,  8,  1); }
static inline AArch64Op a64op_vec16b(uint8_t n) { return a64op_make_vec(n, 16,  1); }
static inline AArch64Op a64op_vec4h (uint8_t n) { return a64op_make_vec(n,  4,  2); }
static inline AArch64Op a64op_vec8h (uint8_t n) { return a64op_make_vec(n,  8,  2); }
static inline AArch64Op a64op_vec2s (uint8_t n) { return a64op_make_vec(n,  2,  4); }
static inline AArch64Op a64op_vec4s (uint8_t n) { return a64op_make_vec(n,  4,  4); }
static inline AArch64Op a64op_vec2d (uint8_t n) { return a64op_make_vec(n,  2,  8); }

static inline AArch64Op a64op_veclist(AArch64Op op0, AArch64Op op1, AArch64Op op2, AArch64Op op3)
{
    assert(a64op_type(op0) != AARCH64_OP_NONE);
    uint8_t num_regs = 1;
    if (a64op_type(op1) != AARCH64_OP_NONE) {
        assert(((a64op_vec_n(op0) + 1) & 0x1f) == a64op_vec_n(op1));
        num_regs++;
        if (a64op_type(op2) != AARCH64_OP_NONE) {
            assert(((a64op_vec_n(op1) + 1) & 0x1f) == a64op_vec_n(op2));
            num_regs++;
            if (a64op_type(op3) != AARCH64_OP_NONE) {
                assert(((a64op_vec_n(op2) + 1) & 0x1f) == a64op_vec_n(op3));
                num_regs++;
            }
        }
    }
    op0.u8[4] = num_regs;
    return op0;
}

static inline AArch64Op a64op_elem(AArch64Op op, uint8_t idx)
{
    op.u8[5] = idx + 1;
    return op;
}

/* scalar modifiers */
static inline AArch64Op v_b(AArch64Op op) { return a64op_vecb(a64op_vec_n(op)); }
static inline AArch64Op v_h(AArch64Op op) { return a64op_vech(a64op_vec_n(op)); }
static inline AArch64Op v_s(AArch64Op op) { return a64op_vecs(a64op_vec_n(op)); }
static inline AArch64Op v_d(AArch64Op op) { return a64op_vecd(a64op_vec_n(op)); }
static inline AArch64Op v_q(AArch64Op op) { return a64op_vecq(a64op_vec_n(op)); }

/* scalar by element modifiers */
static inline AArch64Op ve_b(AArch64Op op, uint8_t idx) { return a64op_elem(v_b(op), idx); }
static inline AArch64Op ve_h(AArch64Op op, uint8_t idx) { return a64op_elem(v_h(op), idx); }
static inline AArch64Op ve_s(AArch64Op op, uint8_t idx) { return a64op_elem(v_s(op), idx); }
static inline AArch64Op ve_d(AArch64Op op, uint8_t idx) { return a64op_elem(v_d(op), idx); }
static inline AArch64Op ve_q(AArch64Op op, uint8_t idx) { return a64op_elem(v_q(op), idx); }

/* arrangement specifier modifiers */
static inline AArch64Op v_8b (AArch64Op op) { return a64op_vec8b (a64op_vec_n(op)); }
static inline AArch64Op v_16b(AArch64Op op) { return a64op_vec16b(a64op_vec_n(op)); }
static inline AArch64Op v_4h (AArch64Op op) { return a64op_vec4h (a64op_vec_n(op)); }
static inline AArch64Op v_8h (AArch64Op op) { return a64op_vec8h (a64op_vec_n(op)); }
static inline AArch64Op v_2s (AArch64Op op) { return a64op_vec2s (a64op_vec_n(op)); }
static inline AArch64Op v_4s (AArch64Op op) { return a64op_vec4s (a64op_vec_n(op)); }
static inline AArch64Op v_2d (AArch64Op op) { return a64op_vec2d (a64op_vec_n(op)); }

/* vector list modifiers */
static inline AArch64Op vv_1(AArch64Op op0)                                              { return a64op_veclist(op0, OPN, OPN, OPN); }
static inline AArch64Op vv_2(AArch64Op op0, AArch64Op op1)                               { return a64op_veclist(op0, op1, OPN, OPN); }
static inline AArch64Op vv_3(AArch64Op op0, AArch64Op op1, AArch64Op op2)                { return a64op_veclist(op0, op1, op2, OPN); }
static inline AArch64Op vv_4(AArch64Op op0, AArch64Op op1, AArch64Op op2, AArch64Op op3) { return a64op_veclist(op0, op1, op2, op3); }

/*********************************************************************/
/* AARCH64_OP_BASE */

#define AARCH64_BASE_OFFSET 0
#define AARCH64_BASE_PRE    1
#define AARCH64_BASE_POST   2
#define AARCH64_BASE_REG    3

static inline AArch64Op a64op_make_base(uint8_t n, uint8_t mode, int16_t imm)
{
    AArch64Op op = { 0 };
    op.u8[0]  = AARCH64_OP_BASE;
    op.u8[1]  = n;
    op.u8[2]  = mode;
    op.u16[2] = (uint16_t) imm;
    return op;
}

static inline AArch64Op a64op_make_base_reg(uint8_t n, uint8_t m,
                                            uint8_t ext, uint8_t sh)
{
    AArch64Op op = { 0 };
    op.u8[0] = AARCH64_OP_BASE;
    op.u8[1] = n;
    op.u8[2] = AARCH64_BASE_REG;
    op.u8[3] = m;
    op.u8[4] = ext;
    op.u8[5] = sh;
    return op;
}

/* getters */
static inline uint8_t a64op_base_n   (AArch64Op op) { return op.u8[1]; }
static inline uint8_t a64op_base_mode(AArch64Op op) { return op.u8[2]; }
static inline int16_t a64op_base_imm (AArch64Op op) { return (int16_t)op.u16[2]; }
static inline uint8_t a64op_base_m   (AArch64Op op) { return op.u8[3]; }
static inline uint8_t a64op_base_ext (AArch64Op op) { return op.u8[4]; }
static inline uint8_t a64op_base_sh  (AArch64Op op) { return op.u8[5]; }

static inline AArch64Op a64op_base(AArch64Op op)              { return a64op_make_base(a64op_gpr_n(op), AARCH64_BASE_OFFSET,   0); }
static inline AArch64Op a64op_off (AArch64Op op, int16_t imm) { return a64op_make_base(a64op_gpr_n(op), AARCH64_BASE_OFFSET, imm); }
static inline AArch64Op a64op_pre (AArch64Op op, int16_t imm) { return a64op_make_base(a64op_gpr_n(op), AARCH64_BASE_PRE,    imm); }
static inline AArch64Op a64op_post(AArch64Op op, int16_t imm) { return a64op_make_base(a64op_gpr_n(op), AARCH64_BASE_POST,   imm); }
static inline AArch64Op a64op_reg (AArch64Op base, AArch64Op off, uint8_t ext, uint8_t sh)
{
    return a64op_make_base_reg(a64op_gpr_n(base), a64op_gpr_n(off), ext, sh);
}

/*********************************************************************/
/* Helper functions to add instructions */

#define i_none(actx                      ) aarch64_add_insn(actx, AARCH64_INSN_NONE,   OPN, OPN, OPN, OPN)

#define i_add(actx,    op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_ADD,    op0, op1, op2, OPN)
#define i_addv(actx,   op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_ADDV,   op0, op1, OPN, OPN)
#define i_adr(actx,    op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_ADR,    op0, op1, OPN, OPN)
#define i_and(actx,    op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_AND,    op0, op1, op2, OPN)
#define i_b(actx,      op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_B,      op0, op1, OPN, OPN)
#define i_br(actx,     op0               ) aarch64_add_insn(actx, AARCH64_INSN_BR,     op0, OPN, OPN, OPN)
#define i_cmp(actx,    op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_CMP,    op0, op1, OPN, OPN)
#define i_csel(actx,   op0, op1, op2, op3) aarch64_add_insn(actx, AARCH64_INSN_CSEL,   op0, op1, op2, op3)
#define i_dup(actx,    op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_DUP,    op0, op1, OPN, OPN)
#define i_fadd(actx,   op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_FADD,   op0, op1, op2, OPN)
#define i_fcvtzu(actx, op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_FCVTZU, op0, op1, OPN, OPN)
#define i_fmax(actx,   op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_FMAX,   op0, op1, op2, OPN)
#define i_fmin(actx,   op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_FMIN,   op0, op1, op2, OPN)
#define i_fmla(actx,   op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_FMLA,   op0, op1, op2, OPN)
#define i_fmul(actx,   op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_FMUL,   op0, op1, op2, OPN)
#define i_ins(actx,    op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_INS,    op0, op1, OPN, OPN)
#define i_ld1(actx,    op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_LD1,    op0, op1, OPN, OPN)
#define i_ld1r(actx,   op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_LD1R,   op0, op1, OPN, OPN)
#define i_ld2(actx,    op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_LD2,    op0, op1, OPN, OPN)
#define i_ld3(actx,    op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_LD3,    op0, op1, OPN, OPN)
#define i_ld4(actx,    op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_LD4,    op0, op1, OPN, OPN)
#define i_ldp(actx,    op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_LDP,    op0, op1, op2, OPN)
#define i_ldr(actx,    op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_LDR,    op0, op1, OPN, OPN)
#define i_ldrb(actx,   op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_LDRB,   op0, op1, OPN, OPN)
#define i_ldrh(actx,   op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_LDRH,   op0, op1, OPN, OPN)
#define i_lsr(actx,    op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_LSR,    op0, op1, op2, OPN)
#define i_mov(actx,    op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_MOV,    op0, op1, OPN, OPN)
#define i_movi(actx,   op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_MOVI,   op0, op1, OPN, OPN)
#define i_mul(actx,    op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_MUL,    op0, op1, op2, OPN)
#define i_orr(actx,    op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_ORR,    op0, op1, op2, OPN)
#define i_ret(actx                       ) aarch64_add_insn(actx, AARCH64_INSN_RET,    OPN, OPN, OPN, OPN)
#define i_rev16(actx,  op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_REV16,  op0, op1, OPN, OPN)
#define i_rev32(actx,  op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_REV32,  op0, op1, OPN, OPN)
#define i_shl(actx,    op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_SHL,    op0, op1, op2, OPN)
#define i_st1(actx,    op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_ST1,    op0, op1, OPN, OPN)
#define i_st2(actx,    op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_ST2,    op0, op1, OPN, OPN)
#define i_st3(actx,    op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_ST3,    op0, op1, OPN, OPN)
#define i_st4(actx,    op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_ST4,    op0, op1, OPN, OPN)
#define i_stp(actx,    op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_STP,    op0, op1, op2, OPN)
#define i_str(actx,    op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_STR,    op0, op1, OPN, OPN)
#define i_sub(actx,    op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_SUB,    op0, op1, op2, OPN)
#define i_subs(actx,   op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_SUBS,   op0, op1, op2, OPN)
#define i_tbl(actx,    op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_TBL,    op0, op1, op2, OPN)
#define i_ubfiz(actx,  op0, op1, op2, op3) aarch64_add_insn(actx, AARCH64_INSN_UBFIZ,  op0, op1, op2, op3)
#define i_ucvtf(actx,  op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_UCVTF,  op0, op1, OPN, OPN)
#define i_umax(actx,   op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_UMAX,   op0, op1, op2, OPN)
#define i_umin(actx,   op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_UMIN,   op0, op1, op2, OPN)
#define i_uqxtn(actx,  op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_UQXTN,  op0, op1, OPN, OPN)
#define i_ushl(actx,   op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_USHL,   op0, op1, op2, OPN)
#define i_ushll(actx,  op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_USHLL,  op0, op1, op2, OPN)
#define i_ushll2(actx, op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_USHLL2, op0, op1, op2, OPN)
#define i_ushr(actx,   op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_USHR,   op0, op1, op2, OPN)
#define i_uxtl(actx,   op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_UXTL,   op0, op1, OPN, OPN)
#define i_uxtl2(actx,  op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_UXTL2,  op0, op1, OPN, OPN)
#define i_xtn(actx,    op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_XTN,    op0, op1, OPN, OPN)
#define i_zip1(actx,   op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_ZIP1,   op0, op1, op2, OPN)
#define i_zip2(actx,   op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_ZIP2,   op0, op1, op2, OPN)

/* branch helpers */
#define i_beq(actx, id) i_b(actx, a64cond_eq(), a64op_label(id))
#define i_bne(actx, id) i_b(actx, a64cond_ne(), a64op_label(id))
#define i_bhs(actx, id) i_b(actx, a64cond_hs(), a64op_label(id))
#define i_bcs(actx, id) i_b(actx, a64cond_cs(), a64op_label(id))
#define i_blo(actx, id) i_b(actx, a64cond_lo(), a64op_label(id))
#define i_bcc(actx, id) i_b(actx, a64cond_cc(), a64op_label(id))
#define i_bmi(actx, id) i_b(actx, a64cond_mi(), a64op_label(id))
#define i_bpl(actx, id) i_b(actx, a64cond_pl(), a64op_label(id))
#define i_bvs(actx, id) i_b(actx, a64cond_vs(), a64op_label(id))
#define i_bvc(actx, id) i_b(actx, a64cond_vc(), a64op_label(id))
#define i_bhi(actx, id) i_b(actx, a64cond_hi(), a64op_label(id))
#define i_bls(actx, id) i_b(actx, a64cond_ls(), a64op_label(id))
#define i_bge(actx, id) i_b(actx, a64cond_ge(), a64op_label(id))
#define i_blt(actx, id) i_b(actx, a64cond_lt(), a64op_label(id))
#define i_bgt(actx, id) i_b(actx, a64cond_gt(), a64op_label(id))
#define i_ble(actx, id) i_b(actx, a64cond_le(), a64op_label(id))

#endif /* AARCH64_RASM_H */
