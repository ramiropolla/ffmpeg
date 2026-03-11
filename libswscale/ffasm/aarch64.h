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

#ifndef AARCH64_H
#define AARCH64_H

#include <assert.h>
#include <stdint.h>

#include "libavutil/bprint.h"

/*********************************************************************/
/* Currently supported AArch64 instruction IDs */

typedef enum AArch64InsnId {
    AARCH64_INSN_NONE = 0,

    AARCH64_INSN_ADD,
    AARCH64_INSN_ADDV,
    AARCH64_INSN_ADR,
    AARCH64_INSN_AND,
    AARCH64_INSN_B,
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
    AARCH64_INSN_LD2,
    AARCH64_INSN_LD3,
    AARCH64_INSN_LD4,
    AARCH64_INSN_LDP,
    AARCH64_INSN_LDR,
    AARCH64_INSN_LDRH,
    AARCH64_INSN_LSR,
    AARCH64_INSN_MOV,
    AARCH64_INSN_MOVI,
    AARCH64_INSN_MUL,
    AARCH64_INSN_ORR,
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
/* Structures TODO */

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

typedef struct AArch64GPR {
    uint8_t n;
    uint8_t s; /* gpr size */
} AArch64GPR;

typedef struct AArch64ArrangementSpecifier {
    uint8_t c; /* element count */
    uint8_t s; /* element size */
} AArch64ArrangementSpecifier;

typedef struct AArch64Vec {
    uint8_t n;
    AArch64ArrangementSpecifier t;
} AArch64Vec;

typedef struct AArch64Op {
    uint8_t type;
    union {
        uint8_t xxx[7];
        AArch64GPR gpr;
        AArch64Vec vec;
    };
    /* TODO I have to be able to add names as well :/ */
} AArch64Op;

static_assert(sizeof(AArch64Op) == 8, "AArch64Op must fit in uint64_t exactly");

typedef struct AArch64Insn {
    AArch64InsnId id;
    AArch64Op op[4];
    char *comment;
} AArch64Insn;

typedef struct AArch64InsnList {
    AArch64Insn *insns;
    int num_insns;
} AArch64InsnList;

typedef struct AArch64Context {
    AArch64InsnList insns;
    int error;
} AArch64Context;

AArch64Context *aarch64_alloc(void);

void aarch64_free(AArch64Context **p_actx);

int aarch64_add_insn(AArch64Context *actx, AArch64InsnId id,
                       AArch64Op op0, AArch64Op op1, AArch64Op op2, AArch64Op op3);

void aarch64_annotate(AArch64Context *actx, const char *comment);

int aarch64_print(AArch64Context *actx, AVBPrint *bp);

static inline AArch64Op aarch64_opn   (void)  { return (AArch64Op) { AARCH64_OP_NONE }; }
static inline AArch64Op aarch64_gpw   (int n) { return (AArch64Op) { AARCH64_OP_GPR, .gpr = { n, sizeof(uint32_t) } }; }
static inline AArch64Op aarch64_gpx   (int n) { return (AArch64Op) { AARCH64_OP_GPR, .gpr = { n, sizeof(uint64_t) } }; }
static inline AArch64Op aarch64_vec   (int n) { return (AArch64Op) { AARCH64_OP_VEC, .vec = { n } }; }

static inline AArch64Op aarch64_vecb  (int n) { return (AArch64Op) { AARCH64_OP_VEC, .vec = { n, .t = {  0,  1 } } }; }
static inline AArch64Op aarch64_vech  (int n) { return (AArch64Op) { AARCH64_OP_VEC, .vec = { n, .t = {  0,  2 } } }; }
static inline AArch64Op aarch64_vecs  (int n) { return (AArch64Op) { AARCH64_OP_VEC, .vec = { n, .t = {  0,  4 } } }; }
static inline AArch64Op aarch64_vecd  (int n) { return (AArch64Op) { AARCH64_OP_VEC, .vec = { n, .t = {  0,  8 } } }; }
static inline AArch64Op aarch64_vecq  (int n) { return (AArch64Op) { AARCH64_OP_VEC, .vec = { n, .t = {  0, 16 } } }; }
static inline AArch64Op aarch64_vec8b (int n) { return (AArch64Op) { AARCH64_OP_VEC, .vec = { n, .t = {  8,  1 } } }; }
static inline AArch64Op aarch64_vec16b(int n) { return (AArch64Op) { AARCH64_OP_VEC, .vec = { n, .t = { 16,  1 } } }; }
static inline AArch64Op aarch64_vec4h (int n) { return (AArch64Op) { AARCH64_OP_VEC, .vec = { n, .t = {  4,  2 } } }; }
static inline AArch64Op aarch64_vec8h (int n) { return (AArch64Op) { AARCH64_OP_VEC, .vec = { n, .t = {  8,  2 } } }; }
static inline AArch64Op aarch64_vec2s (int n) { return (AArch64Op) { AARCH64_OP_VEC, .vec = { n, .t = {  2,  4 } } }; }
static inline AArch64Op aarch64_vec4s (int n) { return (AArch64Op) { AARCH64_OP_VEC, .vec = { n, .t = {  4,  4 } } }; }
static inline AArch64Op aarch64_vec2d (int n) { return (AArch64Op) { AARCH64_OP_VEC, .vec = { n, .t = {  2,  8 } } }; }

/*********************************************************************/
static inline AArch64Op aarch64_8b (AArch64Op op) { return aarch64_vec8b (op.vec.n); }
static inline AArch64Op aarch64_16b(AArch64Op op) { return aarch64_vec16b(op.vec.n); }
static inline AArch64Op aarch64_4h (AArch64Op op) { return aarch64_vec4h (op.vec.n); }
static inline AArch64Op aarch64_8h (AArch64Op op) { return aarch64_vec8h (op.vec.n); }
static inline AArch64Op aarch64_2s (AArch64Op op) { return aarch64_vec2s (op.vec.n); }
static inline AArch64Op aarch64_4s (AArch64Op op) { return aarch64_vec4s (op.vec.n); }
static inline AArch64Op aarch64_2d (AArch64Op op) { return aarch64_vec2d (op.vec.n); }

/*********************************************************************/
static inline AArch64Op aarch64_w0 (void) { return aarch64_gpw( 0); }
static inline AArch64Op aarch64_w1 (void) { return aarch64_gpw( 1); }
static inline AArch64Op aarch64_w2 (void) { return aarch64_gpw( 2); }
static inline AArch64Op aarch64_w3 (void) { return aarch64_gpw( 3); }
static inline AArch64Op aarch64_w4 (void) { return aarch64_gpw( 4); }
static inline AArch64Op aarch64_w5 (void) { return aarch64_gpw( 5); }
static inline AArch64Op aarch64_w6 (void) { return aarch64_gpw( 6); }
static inline AArch64Op aarch64_w7 (void) { return aarch64_gpw( 7); }
static inline AArch64Op aarch64_w8 (void) { return aarch64_gpw( 8); }
static inline AArch64Op aarch64_w9 (void) { return aarch64_gpw( 9); }
static inline AArch64Op aarch64_w10(void) { return aarch64_gpw(10); }
static inline AArch64Op aarch64_w11(void) { return aarch64_gpw(11); }
static inline AArch64Op aarch64_w12(void) { return aarch64_gpw(12); }
static inline AArch64Op aarch64_w13(void) { return aarch64_gpw(13); }
static inline AArch64Op aarch64_w14(void) { return aarch64_gpw(14); }
static inline AArch64Op aarch64_w15(void) { return aarch64_gpw(15); }
static inline AArch64Op aarch64_w16(void) { return aarch64_gpw(16); }
static inline AArch64Op aarch64_w17(void) { return aarch64_gpw(17); }
static inline AArch64Op aarch64_w18(void) { return aarch64_gpw(18); }
static inline AArch64Op aarch64_w19(void) { return aarch64_gpw(19); }
static inline AArch64Op aarch64_w20(void) { return aarch64_gpw(20); }
static inline AArch64Op aarch64_w21(void) { return aarch64_gpw(21); }
static inline AArch64Op aarch64_w22(void) { return aarch64_gpw(22); }
static inline AArch64Op aarch64_w23(void) { return aarch64_gpw(23); }
static inline AArch64Op aarch64_w24(void) { return aarch64_gpw(24); }
static inline AArch64Op aarch64_w25(void) { return aarch64_gpw(25); }
static inline AArch64Op aarch64_w26(void) { return aarch64_gpw(26); }
static inline AArch64Op aarch64_w27(void) { return aarch64_gpw(27); }
static inline AArch64Op aarch64_w28(void) { return aarch64_gpw(28); }
static inline AArch64Op aarch64_w29(void) { return aarch64_gpw(29); }
static inline AArch64Op aarch64_w30(void) { return aarch64_gpw(30); }
static inline AArch64Op aarch64_w31(void) { return aarch64_gpw(31); }

/*********************************************************************/
static inline AArch64Op aarch64_x0 (void) { return aarch64_gpx( 0); }
static inline AArch64Op aarch64_x1 (void) { return aarch64_gpx( 1); }
static inline AArch64Op aarch64_x2 (void) { return aarch64_gpx( 2); }
static inline AArch64Op aarch64_x3 (void) { return aarch64_gpx( 3); }
static inline AArch64Op aarch64_x4 (void) { return aarch64_gpx( 4); }
static inline AArch64Op aarch64_x5 (void) { return aarch64_gpx( 5); }
static inline AArch64Op aarch64_x6 (void) { return aarch64_gpx( 6); }
static inline AArch64Op aarch64_x7 (void) { return aarch64_gpx( 7); }
static inline AArch64Op aarch64_x8 (void) { return aarch64_gpx( 8); }
static inline AArch64Op aarch64_x9 (void) { return aarch64_gpx( 9); }
static inline AArch64Op aarch64_x10(void) { return aarch64_gpx(10); }
static inline AArch64Op aarch64_x11(void) { return aarch64_gpx(11); }
static inline AArch64Op aarch64_x12(void) { return aarch64_gpx(12); }
static inline AArch64Op aarch64_x13(void) { return aarch64_gpx(13); }
static inline AArch64Op aarch64_x14(void) { return aarch64_gpx(14); }
static inline AArch64Op aarch64_x15(void) { return aarch64_gpx(15); }
static inline AArch64Op aarch64_x16(void) { return aarch64_gpx(16); }
static inline AArch64Op aarch64_x17(void) { return aarch64_gpx(17); }
static inline AArch64Op aarch64_x18(void) { return aarch64_gpx(18); }
static inline AArch64Op aarch64_x19(void) { return aarch64_gpx(19); }
static inline AArch64Op aarch64_x20(void) { return aarch64_gpx(20); }
static inline AArch64Op aarch64_x21(void) { return aarch64_gpx(21); }
static inline AArch64Op aarch64_x22(void) { return aarch64_gpx(22); }
static inline AArch64Op aarch64_x23(void) { return aarch64_gpx(23); }
static inline AArch64Op aarch64_x24(void) { return aarch64_gpx(24); }
static inline AArch64Op aarch64_x25(void) { return aarch64_gpx(25); }
static inline AArch64Op aarch64_x26(void) { return aarch64_gpx(26); }
static inline AArch64Op aarch64_x27(void) { return aarch64_gpx(27); }
static inline AArch64Op aarch64_x28(void) { return aarch64_gpx(28); }
static inline AArch64Op aarch64_x29(void) { return aarch64_gpx(29); }
static inline AArch64Op aarch64_x30(void) { return aarch64_gpx(30); }
static inline AArch64Op aarch64_x31(void) { return aarch64_gpx(31); }

/*********************************************************************/
static inline AArch64Op aarch64_v0 (void) { return aarch64_vec( 0); }
static inline AArch64Op aarch64_v1 (void) { return aarch64_vec( 1); }
static inline AArch64Op aarch64_v2 (void) { return aarch64_vec( 2); }
static inline AArch64Op aarch64_v3 (void) { return aarch64_vec( 3); }
static inline AArch64Op aarch64_v4 (void) { return aarch64_vec( 4); }
static inline AArch64Op aarch64_v5 (void) { return aarch64_vec( 5); }
static inline AArch64Op aarch64_v6 (void) { return aarch64_vec( 6); }
static inline AArch64Op aarch64_v7 (void) { return aarch64_vec( 7); }
static inline AArch64Op aarch64_v8 (void) { return aarch64_vec( 8); }
static inline AArch64Op aarch64_v9 (void) { return aarch64_vec( 9); }
static inline AArch64Op aarch64_v10(void) { return aarch64_vec(10); }
static inline AArch64Op aarch64_v11(void) { return aarch64_vec(11); }
static inline AArch64Op aarch64_v12(void) { return aarch64_vec(12); }
static inline AArch64Op aarch64_v13(void) { return aarch64_vec(13); }
static inline AArch64Op aarch64_v14(void) { return aarch64_vec(14); }
static inline AArch64Op aarch64_v15(void) { return aarch64_vec(15); }
static inline AArch64Op aarch64_v16(void) { return aarch64_vec(16); }
static inline AArch64Op aarch64_v17(void) { return aarch64_vec(17); }
static inline AArch64Op aarch64_v18(void) { return aarch64_vec(18); }
static inline AArch64Op aarch64_v19(void) { return aarch64_vec(19); }
static inline AArch64Op aarch64_v20(void) { return aarch64_vec(20); }
static inline AArch64Op aarch64_v21(void) { return aarch64_vec(21); }
static inline AArch64Op aarch64_v22(void) { return aarch64_vec(22); }
static inline AArch64Op aarch64_v23(void) { return aarch64_vec(23); }
static inline AArch64Op aarch64_v24(void) { return aarch64_vec(24); }
static inline AArch64Op aarch64_v25(void) { return aarch64_vec(25); }
static inline AArch64Op aarch64_v26(void) { return aarch64_vec(26); }
static inline AArch64Op aarch64_v27(void) { return aarch64_vec(27); }
static inline AArch64Op aarch64_v28(void) { return aarch64_vec(28); }
static inline AArch64Op aarch64_v29(void) { return aarch64_vec(29); }
static inline AArch64Op aarch64_v30(void) { return aarch64_vec(30); }
static inline AArch64Op aarch64_v31(void) { return aarch64_vec(31); }

/*********************************************************************/
static inline AArch64Op aarch64_b0 (void) { return aarch64_vecb( 0); }
static inline AArch64Op aarch64_b1 (void) { return aarch64_vecb( 1); }
static inline AArch64Op aarch64_b2 (void) { return aarch64_vecb( 2); }
static inline AArch64Op aarch64_b3 (void) { return aarch64_vecb( 3); }
static inline AArch64Op aarch64_b4 (void) { return aarch64_vecb( 4); }
static inline AArch64Op aarch64_b5 (void) { return aarch64_vecb( 5); }
static inline AArch64Op aarch64_b6 (void) { return aarch64_vecb( 6); }
static inline AArch64Op aarch64_b7 (void) { return aarch64_vecb( 7); }
static inline AArch64Op aarch64_b8 (void) { return aarch64_vecb( 8); }
static inline AArch64Op aarch64_b9 (void) { return aarch64_vecb( 9); }
static inline AArch64Op aarch64_b10(void) { return aarch64_vecb(10); }
static inline AArch64Op aarch64_b11(void) { return aarch64_vecb(11); }
static inline AArch64Op aarch64_b12(void) { return aarch64_vecb(12); }
static inline AArch64Op aarch64_b13(void) { return aarch64_vecb(13); }
static inline AArch64Op aarch64_b14(void) { return aarch64_vecb(14); }
static inline AArch64Op aarch64_b15(void) { return aarch64_vecb(15); }
static inline AArch64Op aarch64_b16(void) { return aarch64_vecb(16); }
static inline AArch64Op aarch64_b17(void) { return aarch64_vecb(17); }
static inline AArch64Op aarch64_b18(void) { return aarch64_vecb(18); }
static inline AArch64Op aarch64_b19(void) { return aarch64_vecb(19); }
static inline AArch64Op aarch64_b20(void) { return aarch64_vecb(20); }
static inline AArch64Op aarch64_b21(void) { return aarch64_vecb(21); }
static inline AArch64Op aarch64_b22(void) { return aarch64_vecb(22); }
static inline AArch64Op aarch64_b23(void) { return aarch64_vecb(23); }
static inline AArch64Op aarch64_b24(void) { return aarch64_vecb(24); }
static inline AArch64Op aarch64_b25(void) { return aarch64_vecb(25); }
static inline AArch64Op aarch64_b26(void) { return aarch64_vecb(26); }
static inline AArch64Op aarch64_b27(void) { return aarch64_vecb(27); }
static inline AArch64Op aarch64_b28(void) { return aarch64_vecb(28); }
static inline AArch64Op aarch64_b29(void) { return aarch64_vecb(29); }
static inline AArch64Op aarch64_b30(void) { return aarch64_vecb(30); }
static inline AArch64Op aarch64_b31(void) { return aarch64_vecb(31); }

/*********************************************************************/
static inline AArch64Op aarch64_h0 (void) { return aarch64_vech( 0); }
static inline AArch64Op aarch64_h1 (void) { return aarch64_vech( 1); }
static inline AArch64Op aarch64_h2 (void) { return aarch64_vech( 2); }
static inline AArch64Op aarch64_h3 (void) { return aarch64_vech( 3); }
static inline AArch64Op aarch64_h4 (void) { return aarch64_vech( 4); }
static inline AArch64Op aarch64_h5 (void) { return aarch64_vech( 5); }
static inline AArch64Op aarch64_h6 (void) { return aarch64_vech( 6); }
static inline AArch64Op aarch64_h7 (void) { return aarch64_vech( 7); }
static inline AArch64Op aarch64_h8 (void) { return aarch64_vech( 8); }
static inline AArch64Op aarch64_h9 (void) { return aarch64_vech( 9); }
static inline AArch64Op aarch64_h10(void) { return aarch64_vech(10); }
static inline AArch64Op aarch64_h11(void) { return aarch64_vech(11); }
static inline AArch64Op aarch64_h12(void) { return aarch64_vech(12); }
static inline AArch64Op aarch64_h13(void) { return aarch64_vech(13); }
static inline AArch64Op aarch64_h14(void) { return aarch64_vech(14); }
static inline AArch64Op aarch64_h15(void) { return aarch64_vech(15); }
static inline AArch64Op aarch64_h16(void) { return aarch64_vech(16); }
static inline AArch64Op aarch64_h17(void) { return aarch64_vech(17); }
static inline AArch64Op aarch64_h18(void) { return aarch64_vech(18); }
static inline AArch64Op aarch64_h19(void) { return aarch64_vech(19); }
static inline AArch64Op aarch64_h20(void) { return aarch64_vech(20); }
static inline AArch64Op aarch64_h21(void) { return aarch64_vech(21); }
static inline AArch64Op aarch64_h22(void) { return aarch64_vech(22); }
static inline AArch64Op aarch64_h23(void) { return aarch64_vech(23); }
static inline AArch64Op aarch64_h24(void) { return aarch64_vech(24); }
static inline AArch64Op aarch64_h25(void) { return aarch64_vech(25); }
static inline AArch64Op aarch64_h26(void) { return aarch64_vech(26); }
static inline AArch64Op aarch64_h27(void) { return aarch64_vech(27); }
static inline AArch64Op aarch64_h28(void) { return aarch64_vech(28); }
static inline AArch64Op aarch64_h29(void) { return aarch64_vech(29); }
static inline AArch64Op aarch64_h30(void) { return aarch64_vech(30); }
static inline AArch64Op aarch64_h31(void) { return aarch64_vech(31); }

/*********************************************************************/
static inline AArch64Op aarch64_s0 (void) { return aarch64_vecs( 0); }
static inline AArch64Op aarch64_s1 (void) { return aarch64_vecs( 1); }
static inline AArch64Op aarch64_s2 (void) { return aarch64_vecs( 2); }
static inline AArch64Op aarch64_s3 (void) { return aarch64_vecs( 3); }
static inline AArch64Op aarch64_s4 (void) { return aarch64_vecs( 4); }
static inline AArch64Op aarch64_s5 (void) { return aarch64_vecs( 5); }
static inline AArch64Op aarch64_s6 (void) { return aarch64_vecs( 6); }
static inline AArch64Op aarch64_s7 (void) { return aarch64_vecs( 7); }
static inline AArch64Op aarch64_s8 (void) { return aarch64_vecs( 8); }
static inline AArch64Op aarch64_s9 (void) { return aarch64_vecs( 9); }
static inline AArch64Op aarch64_s10(void) { return aarch64_vecs(10); }
static inline AArch64Op aarch64_s11(void) { return aarch64_vecs(11); }
static inline AArch64Op aarch64_s12(void) { return aarch64_vecs(12); }
static inline AArch64Op aarch64_s13(void) { return aarch64_vecs(13); }
static inline AArch64Op aarch64_s14(void) { return aarch64_vecs(14); }
static inline AArch64Op aarch64_s15(void) { return aarch64_vecs(15); }
static inline AArch64Op aarch64_s16(void) { return aarch64_vecs(16); }
static inline AArch64Op aarch64_s17(void) { return aarch64_vecs(17); }
static inline AArch64Op aarch64_s18(void) { return aarch64_vecs(18); }
static inline AArch64Op aarch64_s19(void) { return aarch64_vecs(19); }
static inline AArch64Op aarch64_s20(void) { return aarch64_vecs(20); }
static inline AArch64Op aarch64_s21(void) { return aarch64_vecs(21); }
static inline AArch64Op aarch64_s22(void) { return aarch64_vecs(22); }
static inline AArch64Op aarch64_s23(void) { return aarch64_vecs(23); }
static inline AArch64Op aarch64_s24(void) { return aarch64_vecs(24); }
static inline AArch64Op aarch64_s25(void) { return aarch64_vecs(25); }
static inline AArch64Op aarch64_s26(void) { return aarch64_vecs(26); }
static inline AArch64Op aarch64_s27(void) { return aarch64_vecs(27); }
static inline AArch64Op aarch64_s28(void) { return aarch64_vecs(28); }
static inline AArch64Op aarch64_s29(void) { return aarch64_vecs(29); }
static inline AArch64Op aarch64_s30(void) { return aarch64_vecs(30); }
static inline AArch64Op aarch64_s31(void) { return aarch64_vecs(31); }

/*********************************************************************/
static inline AArch64Op aarch64_d0 (void) { return aarch64_vecd( 0); }
static inline AArch64Op aarch64_d1 (void) { return aarch64_vecd( 1); }
static inline AArch64Op aarch64_d2 (void) { return aarch64_vecd( 2); }
static inline AArch64Op aarch64_d3 (void) { return aarch64_vecd( 3); }
static inline AArch64Op aarch64_d4 (void) { return aarch64_vecd( 4); }
static inline AArch64Op aarch64_d5 (void) { return aarch64_vecd( 5); }
static inline AArch64Op aarch64_d6 (void) { return aarch64_vecd( 6); }
static inline AArch64Op aarch64_d7 (void) { return aarch64_vecd( 7); }
static inline AArch64Op aarch64_d8 (void) { return aarch64_vecd( 8); }
static inline AArch64Op aarch64_d9 (void) { return aarch64_vecd( 9); }
static inline AArch64Op aarch64_d10(void) { return aarch64_vecd(10); }
static inline AArch64Op aarch64_d11(void) { return aarch64_vecd(11); }
static inline AArch64Op aarch64_d12(void) { return aarch64_vecd(12); }
static inline AArch64Op aarch64_d13(void) { return aarch64_vecd(13); }
static inline AArch64Op aarch64_d14(void) { return aarch64_vecd(14); }
static inline AArch64Op aarch64_d15(void) { return aarch64_vecd(15); }
static inline AArch64Op aarch64_d16(void) { return aarch64_vecd(16); }
static inline AArch64Op aarch64_d17(void) { return aarch64_vecd(17); }
static inline AArch64Op aarch64_d18(void) { return aarch64_vecd(18); }
static inline AArch64Op aarch64_d19(void) { return aarch64_vecd(19); }
static inline AArch64Op aarch64_d20(void) { return aarch64_vecd(20); }
static inline AArch64Op aarch64_d21(void) { return aarch64_vecd(21); }
static inline AArch64Op aarch64_d22(void) { return aarch64_vecd(22); }
static inline AArch64Op aarch64_d23(void) { return aarch64_vecd(23); }
static inline AArch64Op aarch64_d24(void) { return aarch64_vecd(24); }
static inline AArch64Op aarch64_d25(void) { return aarch64_vecd(25); }
static inline AArch64Op aarch64_d26(void) { return aarch64_vecd(26); }
static inline AArch64Op aarch64_d27(void) { return aarch64_vecd(27); }
static inline AArch64Op aarch64_d28(void) { return aarch64_vecd(28); }
static inline AArch64Op aarch64_d29(void) { return aarch64_vecd(29); }
static inline AArch64Op aarch64_d30(void) { return aarch64_vecd(30); }
static inline AArch64Op aarch64_d31(void) { return aarch64_vecd(31); }

/*********************************************************************/
static inline AArch64Op aarch64_q0 (void) { return aarch64_vecq( 0); }
static inline AArch64Op aarch64_q1 (void) { return aarch64_vecq( 1); }
static inline AArch64Op aarch64_q2 (void) { return aarch64_vecq( 2); }
static inline AArch64Op aarch64_q3 (void) { return aarch64_vecq( 3); }
static inline AArch64Op aarch64_q4 (void) { return aarch64_vecq( 4); }
static inline AArch64Op aarch64_q5 (void) { return aarch64_vecq( 5); }
static inline AArch64Op aarch64_q6 (void) { return aarch64_vecq( 6); }
static inline AArch64Op aarch64_q7 (void) { return aarch64_vecq( 7); }
static inline AArch64Op aarch64_q8 (void) { return aarch64_vecq( 8); }
static inline AArch64Op aarch64_q9 (void) { return aarch64_vecq( 9); }
static inline AArch64Op aarch64_q10(void) { return aarch64_vecq(10); }
static inline AArch64Op aarch64_q11(void) { return aarch64_vecq(11); }
static inline AArch64Op aarch64_q12(void) { return aarch64_vecq(12); }
static inline AArch64Op aarch64_q13(void) { return aarch64_vecq(13); }
static inline AArch64Op aarch64_q14(void) { return aarch64_vecq(14); }
static inline AArch64Op aarch64_q15(void) { return aarch64_vecq(15); }
static inline AArch64Op aarch64_q16(void) { return aarch64_vecq(16); }
static inline AArch64Op aarch64_q17(void) { return aarch64_vecq(17); }
static inline AArch64Op aarch64_q18(void) { return aarch64_vecq(18); }
static inline AArch64Op aarch64_q19(void) { return aarch64_vecq(19); }
static inline AArch64Op aarch64_q20(void) { return aarch64_vecq(20); }
static inline AArch64Op aarch64_q21(void) { return aarch64_vecq(21); }
static inline AArch64Op aarch64_q22(void) { return aarch64_vecq(22); }
static inline AArch64Op aarch64_q23(void) { return aarch64_vecq(23); }
static inline AArch64Op aarch64_q24(void) { return aarch64_vecq(24); }
static inline AArch64Op aarch64_q25(void) { return aarch64_vecq(25); }
static inline AArch64Op aarch64_q26(void) { return aarch64_vecq(26); }
static inline AArch64Op aarch64_q27(void) { return aarch64_vecq(27); }
static inline AArch64Op aarch64_q28(void) { return aarch64_vecq(28); }
static inline AArch64Op aarch64_q29(void) { return aarch64_vecq(29); }
static inline AArch64Op aarch64_q30(void) { return aarch64_vecq(30); }
static inline AArch64Op aarch64_q31(void) { return aarch64_vecq(31); }

/*********************************************************************/
/* Helper functions to add instructions */

#define OPN aarch64_opn()
#define aarch64_none(actx                      ) aarch64_add_insn(actx, AARCH64_INSN_NONE,   OPN, OPN, OPN, OPN)

#define aarch64_add(actx,    op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_ADD,    op0, op1, op2, OPN)
#define aarch64_addv(actx,   op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_ADDV,   op0, op1, OPN, OPN)
#define aarch64_adr(actx,    op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_ADR,    op0, op1, OPN, OPN)
#define aarch64_and(actx,    op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_AND,    op0, op1, op2, OPN)
#define aarch64_b(actx,      op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_B,      op0, op1, OPN, OPN)
#define aarch64_cmp(actx,    op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_CMP,    op0, op1, OPN, OPN)
#define aarch64_csel(actx,   op0, op1, op2, op3) aarch64_add_insn(actx, AARCH64_INSN_CSEL,   op0, op1, op2, op3)
#define aarch64_dup(actx,    op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_DUP,    op0, op1, OPN, OPN)
#define aarch64_fadd(actx,   op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_FADD,   op0, op1, op2, OPN)
#define aarch64_fcvtzu(actx, op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_FCVTZU, op0, op1, OPN, OPN)
#define aarch64_fmax(actx,   op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_FMAX,   op0, op1, op2, OPN)
#define aarch64_fmin(actx,   op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_FMIN,   op0, op1, op2, OPN)
#define aarch64_fmla(actx,   op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_FMLA,   op0, op1, op2, OPN)
#define aarch64_fmul(actx,   op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_FMUL,   op0, op1, op2, OPN)
#define aarch64_ins(actx,    op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_INS,    op0, op1, OPN, OPN)
#define aarch64_ld1(actx,    op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_LD1,    op0, op1, OPN, OPN)
#define aarch64_ld2(actx,    op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_LD2,    op0, op1, OPN, OPN)
#define aarch64_ld3(actx,    op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_LD3,    op0, op1, OPN, OPN)
#define aarch64_ld4(actx,    op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_LD4,    op0, op1, OPN, OPN)
#define aarch64_ldp(actx,    op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_LDP,    op0, op1, op2, OPN)
#define aarch64_ldr(actx,    op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_LDR,    op0, op1, OPN, OPN)
#define aarch64_ldrh(actx,   op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_LDRH,   op0, op1, OPN, OPN)
#define aarch64_lsr(actx,    op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_LSR,    op0, op1, op2, OPN)
#define aarch64_mov(actx,    op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_MOV,    op0, op1, OPN, OPN)
#define aarch64_movi(actx,   op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_MOVI,   op0, op1, OPN, OPN)
#define aarch64_mul(actx,    op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_MUL,    op0, op1, op2, OPN)
#define aarch64_orr(actx,    op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_ORR,    op0, op1, op2, OPN)
#define aarch64_rev16(actx,  op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_REV16,  op0, op1, OPN, OPN)
#define aarch64_rev32(actx,  op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_REV32,  op0, op1, OPN, OPN)
#define aarch64_shl(actx,    op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_SHL,    op0, op1, op2, OPN)
#define aarch64_st1(actx,    op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_ST1,    op0, op1, OPN, OPN)
#define aarch64_st2(actx,    op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_ST2,    op0, op1, OPN, OPN)
#define aarch64_st3(actx,    op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_ST3,    op0, op1, OPN, OPN)
#define aarch64_st4(actx,    op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_ST4,    op0, op1, OPN, OPN)
#define aarch64_stp(actx,    op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_STP,    op0, op1, op2, OPN)
#define aarch64_str(actx,    op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_STR,    op0, op1, OPN, OPN)
#define aarch64_sub(actx,    op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_SUB,    op0, op1, op2, OPN)
#define aarch64_subs(actx,   op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_SUBS,   op0, op1, op2, OPN)
#define aarch64_tbl(actx,    op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_TBL,    op0, op1, op2, OPN)
#define aarch64_ubfiz(actx,  op0, op1, op2, op3) aarch64_add_insn(actx, AARCH64_INSN_UBFIZ,  op0, op1, op2, op3)
#define aarch64_ucvtf(actx,  op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_UCVTF,  op0, op1, OPN, OPN)
#define aarch64_umin(actx,   op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_UMIN,   op0, op1, op2, OPN)
#define aarch64_uqxtn(actx,  op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_UQXTN,  op0, op1, OPN, OPN)
#define aarch64_ushl(actx,   op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_USHL,   op0, op1, op2, OPN)
#define aarch64_ushll(actx,  op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_USHLL,  op0, op1, op2, OPN)
#define aarch64_ushll2(actx, op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_USHLL2, op0, op1, op2, OPN)
#define aarch64_ushr(actx,   op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_USHR,   op0, op1, op2, OPN)
#define aarch64_uxtl(actx,   op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_UXTL,   op0, op1, OPN, OPN)
#define aarch64_uxtl2(actx,  op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_UXTL2,  op0, op1, OPN, OPN)
#define aarch64_xtn(actx,    op0, op1          ) aarch64_add_insn(actx, AARCH64_INSN_XTN,    op0, op1, OPN, OPN)
#define aarch64_zip1(actx,   op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_ZIP1,   op0, op1, op2, OPN)
#define aarch64_zip2(actx,   op0, op1, op2     ) aarch64_add_insn(actx, AARCH64_INSN_ZIP2,   op0, op1, op2, OPN)

#define aarch64_comment(actx, comment) do { \
    aarch64_none(actx);                     \
    aarch64_annotate(actx, comment);        \
} while (0)

#endif /* AARCH64_H */
