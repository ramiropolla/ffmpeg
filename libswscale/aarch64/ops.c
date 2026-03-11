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

#include "../ops_chain.h"

#include "libswscale/ffasm/aarch64.h"

#include "libavutil/avstring.h"

typedef struct SwsAArch64Context {
    int block_size;
} SwsAArch64Context;

#define LOOP_ARRAY(idx, arr)          \
    for (int idx = 0; idx < 4; idx++) \
        if (arr[idx])
#define LOOP_OUT(idx) LOOP_ARRAY(idx, !next->comps.unused)
#define LOOP_IN(idx)  LOOP_ARRAY(idx, !op->comps.unused)
#define LOOP_OUT_VH(idx) LOOP_ARRAY(idx, use_vh && !next->comps.unused)
#define LOOP_IN_VH(idx)  LOOP_ARRAY(idx, use_vh && !op->comps.unused)
#define LOOP_N(idx, n) for (int idx = 0; idx < n; i++)

#if 0
/* Vector element type for given SwsOp */
typedef struct VectorElementType {
    VectorElementType(const SwsOp *op, int block_size)
    {
        e = (op->type == SWS_PIXEL_U8)  ? 1
          : (op->type == SWS_PIXEL_U16) ? 2
          :                               4;
        int full_size = e * block_size;
        size = FFMIN(full_size, 16);
    }

    a64::Vec type(const a64::Vec &vreg) const
    {
        switch ((e << 8) | size) {
        case 0x0110: return vreg.b16();
        case 0x0108: return vreg.b8();
        case 0x0210: return vreg.h8();
        case 0x0410: return vreg.s4();
        }
        printf("ERORROORORRORORO %d %d\n", e, size);
        return vreg.b16();
    }

    a64::Vec type_gpr(const a64::Vec &vreg) const
    {
        switch ((e << 8) | size) {
        case 0x0110: return vreg.q();
        case 0x0108: return vreg.d();
        case 0x0210: return vreg.q();
        case 0x0410: return vreg.q();
        }
        printf("ERORROORORRORORO gpr %d %d\n", e, size);
        return vreg.q();
    }

    uint8_t e;
    uint8_t size;
} VectorElementType;
#else
typedef struct VectorElementType {
    uint8_t e; /* element size */
    uint8_t v; /* vector size */
    uint8_t c; /* element count */
} VectorElementType;
static void aarch64_t_init(AArch64ArrangementSpecifier *t, const SwsOp *op, int block_size)
{
    t->s = ff_sws_pixel_type_size(op->type);
    t->c = FFMIN(t->s * block_size, 16) / t->s;
}
#endif

const char *used_mask(const bool unused[4], char buf[5]);
const char *used_mask(const bool unused[4], char buf[5])
{
    for (int i = 0; i < 4; i++)
        buf[i] = unused[i] ? '0' : '1';
    return buf;
}
#define USED_MASK(op) used_mask(next->comps.unused, (char[8]){0})

#define Q(N) ((AVRational) { N, 1 })
#define Q0   Q(0)
#define Q1   Q(1)

static av_printf_format(3, 4) void buf_appendf(char **pbuf, size_t *prem, const char *fmt, ...)
{
    char *buf = *pbuf;
    size_t rem = *prem;
    if (!rem)
        return;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, rem, fmt, ap);
    va_end(ap);

    if (n > 0) {
        size_t written = (size_t) n;
        if (written < rem) {
            buf += written;
            rem -= written;
        } else {
            buf += rem - 1;
            rem = 0;
        }
        *pbuf = buf;
        *prem = rem;
    }
}

/* NOTE does not write terminating '\0' */
static void buf_appendc(char **pbuf, size_t *prem, char c)
{
    char *buf = *pbuf;
    size_t rem = *prem;
    if (!rem)
        return;

    *buf++ = c;

    *pbuf = buf;
    *prem = rem - 1;
}

static void parse_used_mask(const char **pp, bool used[4], uint8_t *pused_count)
{
    uint8_t used_count = 0;
    const char *p = *pp;
    for (int i = 0; i < 4; i++) {
        used[i] = (*p++ == '1');
        if (used[i])
            used_count++;
    }
    *pp = p + 1;
    *pused_count = used_count;
}

static void parse_t(const char **pp, AArch64ArrangementSpecifier *t)
{
    unsigned int tc_ts;
    sscanf(*pp, "%04x", &tc_ts);
    t->s = tc_ts & 0xff;
    t->c = (tc_ts >> 8) & 0xff;
    *pp += 5; /* 4 hex chars + trailing '_' */
}

static void aarch64_process_gen(AArch64Context *actx, const char *sig, uint8_t n)
{
    /* Input arguments */
    AArch64Op exec     = a64op_gpx(0);
    AArch64Op priv     = a64op_gpx(1);
    AArch64Op bx_start = a64op_gpw(2);
    AArch64Op y_start  = a64op_gpw(3);
    AArch64Op bx_end   = a64op_gpw(4);
    AArch64Op y_end    = a64op_gpw(5);

    AArch64Op y = y_start;
    AArch64Op bx = a64op_gpw(6);

    /* Vector banks */
    AArch64Op vl[4] = { a64op_vec(0), a64op_vec(1), a64op_vec(2), a64op_vec(3) };
    AArch64Op vh[4] = { a64op_vec(4), a64op_vec(5), a64op_vec(6), a64op_vec(7) };

    /* Data pointers */
    AArch64Op in[4] = { a64op_gpx(10), a64op_gpx(11), a64op_gpx(12), a64op_gpx(13) };
    AArch64Op out[4] = { a64op_gpx(14), a64op_gpx(15), a64op_gpx(16), a64op_gpx(17) };

    /* padding */
    AArch64Op in_padding[4] = {
        a64op_gpx(20),
        a64op_gpx(21),
        a64op_gpx(22),
        a64op_gpx(23),
    };
    AArch64Op out_padding[4] = {
        a64op_gpx(24),
        a64op_gpx(25),
        a64op_gpx(26),
        a64op_gpx(27),
    };

    aarch64_add_func(actx, sig, true);

    LOOP_N(i, n) a64insn_ldr(actx, in[i],          a64op_off(exec, offsetof(SwsOpExec, in)       + (i * sizeof(uint8_t *))));
    LOOP_N(i, n) a64insn_ldr(actx, out[i],         a64op_off(exec, offsetof(SwsOpExec, out)      + (i * sizeof(uint8_t *))));
    LOOP_N(i, n) a64insn_ldr(actx, in_padding[i],  a64op_off(exec, offsetof(SwsOpExec, in_bump)  + (i * sizeof(ptrdiff_t))));
    LOOP_N(i, n) a64insn_ldr(actx, out_padding[i], a64op_off(exec, offsetof(SwsOpExec, out_bump) + (i * sizeof(ptrdiff_t))));

    int loop = aarch64_new_label(actx, "loop");
    aarch64_add_label(actx, loop);

    /* FUNCTION GOES HERE */

    aarch64_add_comment(actx, "horizontal loop back");
    a64insn_add(actx, bx, bx, a64op_imm(1));
    a64insn_cmp(actx, bx, bx_end);
    a64insn_blo(actx, loop);

    aarch64_add_comment(actx, "reset bx");
    a64insn_mov(actx, bx, bx_start);

    aarch64_add_comment(actx, "padding");
    LOOP_N(i, n) a64insn_add(actx, in[i],  in[i],  in_padding[i]);
    LOOP_N(i, n) a64insn_add(actx, out[i], out[i], out_padding[i]);

    aarch64_add_comment(actx, "vertical loop back");
    a64insn_add(actx, y, y, a64op_imm(1));
    a64insn_cmp(actx, y, y_end);
    a64insn_blo(actx, loop);

#if 0
typedef void (*SwsOpFunc)(const SwsOpExec *exec, const void *priv,
                          int bx_start, int y_start, int bx_end, int y_end);

static void fn(process)(const SwsOpExec *exec, const void *priv,
                        const int bx_start, const int y_start,
                        int bx_end, int y_end)
{
    const SwsOpChain *chain = priv;
    const SwsOpImpl *impl = chain->impl;
    u32block_t x, y, z, w; /* allocate enough space for any intermediate */

    SwsOpIter iterdata;
    SwsOpIter *iter = &iterdata; /* for CONTINUE() macro to work */

    for (iter->y = y_start; iter->y < y_end; iter->y++) {
        for (int i = 0; i < 4; i++) {
            iter->in[i]  = exec->in[i]  + (iter->y - y_start) * exec->in_stride[i];
            iter->out[i] = exec->out[i] + (iter->y - y_start) * exec->out_stride[i];
        }

        for (int block = bx_start; block < bx_end; block++) {
            iter->x = block * SWS_BLOCK_SIZE;
            CONTINUE(block_t, (void *) x, (void *) y, (void *) z, (void *) w);
        }
    }
}

%macro process_fn 1 ; number of planes
cglobal sws_process%1_x86, 6, 6 + 2 * %1, 16
            ; Args:
            ;   execq, implq, bxd, yd as defined in ops_common.int
            ;   bx_end and y_end are initially in tmp0d / tmp1d
            ;   (see SwsOpFunc signature)
            ;
            ; Stack layout:
            ;   [rsp +  0] = [qword] impl->cont (address of first kernel)
            ;   [rsp +  8] = [qword] &impl[1]   (restore implq after chain)
            ;   [rsp + 16] = [dword] bx start   (restore after line finish)
            ;   [rsp + 20] = [dword] bx end     (loop counter limit)
            ;   [rsp + 24] = [dword] y end      (loop counter limit)
            sub rsp, 32
            mov [rsp + 16], bxd
            mov [rsp + 20], tmp0d ; bx_end
            mov [rsp + 24], tmp1d ; y_end
            mov tmp0q, [implq + SwsOpImpl.cont]
            add implq, SwsOpImpl.next
            mov [rsp +  0], tmp0q
            mov [rsp +  8], implq

            ; load plane pointers
            mov in0q,  [execq + SwsOpExec.in0]
IF %1 > 1,  mov in1q,  [execq + SwsOpExec.in1]
IF %1 > 2,  mov in2q,  [execq + SwsOpExec.in2]
IF %1 > 3,  mov in3q,  [execq + SwsOpExec.in3]
            mov out0q, [execq + SwsOpExec.out0]
IF %1 > 1,  mov out1q, [execq + SwsOpExec.out1]
IF %1 > 2,  mov out2q, [execq + SwsOpExec.out2]
IF %1 > 3,  mov out3q, [execq + SwsOpExec.out3]
            jmp [rsp] ; call into op chain

; Declare a separate global label for the return point, so that we can append
; it to the list of op function pointers from the C code, effectively ensuring
; that we end up here again after the op chain finishes processing a line.
; (See also: cglobal_label in x86inc.asm)
%if FORMAT_ELF
    global current_function %+ _return:function hidden
%elif FORMAT_MACHO && HAVE_PRIVATE_EXTERN
    global current_function %+ _return:private_extern
%else
    global current_function %+ _return
%endif
align function_align
current_function %+ _return:

            ; op chain always returns back here
            mov implq, [rsp + 8]
            inc bxd
            cmp bxd, [rsp + 20]
            jne .continue
            ; end of line
            inc yd
            cmp yd, [rsp + 24]
            je .end
            ; bump addresses to point to start of next line
            add in0q,  [execq + SwsOpExec.in_bump0]
IF %1 > 1,  add in1q,  [execq + SwsOpExec.in_bump1]
IF %1 > 2,  add in2q,  [execq + SwsOpExec.in_bump2]
IF %1 > 3,  add in3q,  [execq + SwsOpExec.in_bump3]
            add out0q, [execq + SwsOpExec.out_bump0]
IF %1 > 1,  add out1q, [execq + SwsOpExec.out_bump1]
IF %1 > 2,  add out2q, [execq + SwsOpExec.out_bump2]
IF %1 > 3,  add out3q, [execq + SwsOpExec.out_bump3]
            mov bxd, [rsp + 16]
.continue:
            jmp [rsp]
.end:
            add rsp, 32
            RET
%endmacro
#endif
}

static void aarch64_func_gen(const char *sig)
{
    AArch64Context *actx = aarch64_alloc();

    const char *p;
    if (!av_strstart(sig, "ff_sws_", &p))
        goto done;

    if (av_strstart(p, "process_", &p)) {
        aarch64_process_gen(actx, sig, *p - '0');
        goto done;
    }

    int op = -1;
    if      (av_strstart(p, "read_",    &p)) op = SWS_OP_READ;
    else if (av_strstart(p, "write_",   &p)) op = SWS_OP_WRITE;
    else if (av_strstart(p, "bswap_",   &p)) op = SWS_OP_SWAP_BYTES;
    else if (av_strstart(p, "swizzle_", &p)) op = SWS_OP_SWIZZLE;
    else if (av_strstart(p, "unpack_",  &p)) op = SWS_OP_UNPACK;
    else if (av_strstart(p, "pack_",    &p)) op = SWS_OP_PACK;
    else if (av_strstart(p, "lshift_",  &p)) op = SWS_OP_LSHIFT;
    else if (av_strstart(p, "rshift_",  &p)) op = SWS_OP_RSHIFT;
    else if (av_strstart(p, "clear_",   &p)) op = SWS_OP_CLEAR;
    else if (av_strstart(p, "convert_", &p)) op = SWS_OP_CONVERT;
    else if (av_strstart(p, "min_",     &p)) op = SWS_OP_MIN;
    else if (av_strstart(p, "max_",     &p)) op = SWS_OP_MAX;
    else if (av_strstart(p, "scale_",   &p)) op = SWS_OP_SCALE;
    else if (av_strstart(p, "linear_",  &p)) op = SWS_OP_LINEAR;
    else if (av_strstart(p, "dither_",  &p)) op = SWS_OP_DITHER;

    AArch64Op vl[4] = {
        a64op_vec(0),
        a64op_vec(1),
        a64op_vec(2),
        a64op_vec(3),
    };
    AArch64Op vh[4] = {
        a64op_vec(4),
        a64op_vec(5),
        a64op_vec(6),
        a64op_vec(7),
    };
    AArch64Op in[4] = {
        a64op_gpx(0),
        a64op_gpx(1),
        a64op_gpx(2),
        a64op_gpx(3),
    };

    aarch64_add_func(actx, sig, true);

    bool used[4] = { false, false, false, false };
    uint8_t used_count;
    bool use_vh = false;
    AArch64ArrangementSpecifier t;

    switch (op) {
    case SWS_OP_READ: {
        uint8_t frac;
        bool packed;

        parse_used_mask(&p, used, &used_count);

        use_vh = (*p++ == '2');
        p++;

        parse_t(&p, &t);
        int vsize = t.c * t.s;

        if (av_strstart(p, "frac_", &p)) {
            frac   = atoi(p);
            packed = false;
        } else if (av_strstart(p, "packed", NULL)) {
            frac   = 0;
            packed = true;
        } else { /* planar */
            frac   = 0;
            packed = false;
        }

        if (frac) {
            // TODO
        } else if (packed) {
            if (used_count == 1) {
                switch ((use_vh ? 0x100 : 0) | vsize) {
                case 0x008: a64insn_ldr(actx, a64op_d(vl[0]),                 a64op_post(in[0], vsize * 1)); break;
                case 0x010: a64insn_ldr(actx, a64op_q(vl[0]),                 a64op_post(in[0], vsize * 1)); break;
                case 0x108: a64insn_ldp(actx, a64op_d(vl[0]), a64op_d(vh[0]), a64op_post(in[0], vsize * 2)); break;
                case 0x110: a64insn_ldp(actx, a64op_q(vl[0]), a64op_q(vh[0]), a64op_post(in[0], vsize * 2)); break;
                }
            } else {
                switch (used_count) {
                case 2: a64insn_ld2(actx, a64op_veclist2(vl[0], vl[1]),               a64op_post(in[0], vsize * 2)); break;
                case 3: a64insn_ld3(actx, a64op_veclist3(vl[0], vl[1], vl[2]),        a64op_post(in[0], vsize * 3)); break;
                case 4: a64insn_ld4(actx, a64op_veclist4(vl[0], vl[1], vl[2], vl[3]), a64op_post(in[0], vsize * 4)); break;
                }
                if (!use_vh)
                    break;
                switch (used_count) {
                case 2: a64insn_ld2(actx, a64op_veclist2(vh[0], vh[1]),               a64op_post(in[0], vsize * 2)); break;
                case 3: a64insn_ld3(actx, a64op_veclist3(vh[0], vh[1], vh[2]),        a64op_post(in[0], vsize * 3)); break;
                case 4: a64insn_ld4(actx, a64op_veclist4(vh[0], vh[1], vh[2], vh[3]), a64op_post(in[0], vsize * 4)); break;
                }
            }
        } else {
            LOOP_ARRAY(i, used) {
                switch ((use_vh ? 0x100 : 0) | vsize) {
                case 0x008: a64insn_ldr(actx, a64op_d(vl[i]),                 a64op_post(in[i], vsize * 1)); break;
                case 0x010: a64insn_ldr(actx, a64op_q(vl[i]),                 a64op_post(in[i], vsize * 1)); break;
                case 0x108: a64insn_ldp(actx, a64op_d(vl[i]), a64op_d(vh[i]), a64op_post(in[i], vsize * 2)); break;
                case 0x110: a64insn_ldp(actx, a64op_q(vl[i]), a64op_q(vh[i]), a64op_post(in[i], vsize * 2)); break;
                }
            }
        }
#if 0
CONTINUE
mov     r8, [rsi]
add     r10, 10h
add     rbx, 10h
add     rsi, 20h ; ' '
jmp     r8
#endif
        break;
    }
    case SWS_OP_WRITE:
        break;
    case SWS_OP_SWAP_BYTES:
        break;
    case SWS_OP_SWIZZLE:
        break;
    case SWS_OP_UNPACK:
        break;
    case SWS_OP_PACK:
        break;
    case SWS_OP_LSHIFT:
        break;
    case SWS_OP_RSHIFT:
        break;
    case SWS_OP_CLEAR:
        break;
    case SWS_OP_CONVERT:
        break;
    case SWS_OP_MIN:
        break;
    case SWS_OP_MAX:
        break;
    case SWS_OP_SCALE:
        break;
    case SWS_OP_LINEAR:
        break;
    case SWS_OP_DITHER:
        break;
    }

    aarch64_add_endfunc(actx);

done:
    AVBPrint bp;
    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);
    aarch64_print(actx, &bp);
    printf("%s", bp.str);
    av_bprint_finalize(&bp, NULL);

    aarch64_free(&actx);
    // exit(0);
}

static int aarch64_sig_gen(SwsAArch64Context *actx, const SwsOpList *ops, int n)
{
    int block_size = actx->block_size;

    const SwsOp *op = &ops->ops[n];
    const SwsOp *next = n + 1 < ops->num_ops ? &ops->ops[n + 1] : op;

    bool use_vh = ((op->type == SWS_PIXEL_U16) && block_size == 16)
               || ((op->type == SWS_PIXEL_U32) && block_size == 8)
               || ((op->type == SWS_PIXEL_F32) && block_size == 8);

    AArch64ArrangementSpecifier t;
    aarch64_t_init(&t, op, block_size);
    // int num_bytes = vet.v << (use_vh ? 1 : 0); // TODO check if we can use num_bytes instead of (use_vh ? 0x100 : 0) | vet.v

    char sig[256];
    char *p = sig;
    size_t rem = sizeof(sig);

#if 0
    const char *op_type_name = ff_sws_op_type_name(op->op);
    size_t op_type_name_len = strlen(op_type_name);
    for (size_t i = 0; i < op_type_name_len; i++)
        buf_appendc(&p, &rem, av_tolower(op_type_name[i]));
#endif

#if 0
    buf_appendf(&p, &rem, "_%c", ff_sws_pixel_type_is_int(op->type) ? 'i' : 'f');
#endif

    buf_appendf(&p, &rem, "ff_sws_aarch64");

    switch (op->op) {
    case SWS_OP_READ:
        buf_appendf(&p, &rem, "_read");
        buf_appendf(&p, &rem, "_%s", USED_MASK(next));
        buf_appendf(&p, &rem, "_%d", (use_vh ? 2 : 1));
        buf_appendf(&p, &rem, "_%04x", (t.c << 8) | t.s);
        if (op->rw.frac) {
            buf_appendf(&p, &rem, "_frac_%d", op->rw.frac);
        } else if (op->rw.packed) {
            buf_appendf(&p, &rem, "_packed");
        } else {
            buf_appendf(&p, &rem, "_planar");
        }
        aarch64_func_gen(sig);
        break;
    case SWS_OP_WRITE:
        buf_appendf(&p, &rem, "_write");
#if 0
        if (op->rw.frac) {
            buf_appendf(&p, &rem, "_frac_%d", op->rw.frac);
        } else if (op->rw.packed) {
            buf_appendf(&p, &rem, "_packed");
        } else {
            buf_appendf(&p, &rem, "_planar");
        }
#endif
        break;
    case SWS_OP_SWAP_BYTES:
        buf_appendf(&p, &rem, "_bswap");
        break;
    case SWS_OP_SWIZZLE:
        buf_appendf(&p, &rem, "_swizzle");
        break;
    case SWS_OP_UNPACK:
        buf_appendf(&p, &rem, "_unpack");
        break;
    case SWS_OP_PACK:
        buf_appendf(&p, &rem, "_pack");
#if 0
        buf_appendf(&p, &rem, "_%08x",
                    MKBETAG(op->pack.pattern[0],
                            op->pack.pattern[1],
                            op->pack.pattern[2],
                            op->pack.pattern[3]));
#endif
        break;
    case SWS_OP_LSHIFT:
        buf_appendf(&p, &rem, "_lshift");
        buf_appendf(&p, &rem, "_%s", USED_MASK(next));
        buf_appendf(&p, &rem, "_%02x", op->c.u);
        break;
    case SWS_OP_RSHIFT:
        buf_appendf(&p, &rem, "_rshift");
        buf_appendf(&p, &rem, "_%s", USED_MASK(next));
        buf_appendf(&p, &rem, "_%02x", op->c.u);
        break;
    case SWS_OP_CLEAR:
        buf_appendf(&p, &rem, "_clear");
        buf_appendf(&p, &rem, "_%s", USED_MASK(next));
        break;
    case SWS_OP_CONVERT:
        buf_appendf(&p, &rem, "_convert");
#if 0
        buf_appendf(&p, &rem, "_%c_%02x_%d",
                    ff_sws_pixel_type_is_int(op->convert.to) ? 'i' : 'f',
                    ff_sws_pixel_type_size(op->convert.to),
                    op->convert.expand);
#endif
        break;
    case SWS_OP_MIN:
        buf_appendf(&p, &rem, "_min");
        break;
    case SWS_OP_MAX:
        buf_appendf(&p, &rem, "_max");
        break;
    case SWS_OP_SCALE:
        buf_appendf(&p, &rem, "_scale");
        break;
    case SWS_OP_LINEAR:
        buf_appendf(&p, &rem, "_linear");
#if 0
        {
            /* Check which vectors are used after this operation */
            bool used[4] = { false, false, false, false };
            LOOP_IN(i) used[i] = true;
            LOOP_ARRAY(i, used) {
                bool is_identity = true;
                for (int j = 0; j < 5; j++) {
                    if (i == j) {
                        if (op->lin.m[i][j].num != 1 || op->lin.m[i][j].den != 1) {
                            is_identity = false;
                            break;
                        }
                    } else {
                        if (op->lin.m[i][j].num != 0) {
                            is_identity = false;
                            break;
                        }
                    }
                }
                if (is_identity)
                    used[i] = false;
            }

            buf_appendc(&p, &rem, '_');
            for (int i = 0; i < 4; i++) {
                if (!used[i]) {
                    buf_appendf(&p, &rem, "00000");
                    continue;
                }
                for (int j = 0; j < 5; j++) {
                    if (!av_cmp_q(op->lin.m[i][j], Q0)) {
                        buf_appendc(&p, &rem, '0');
                    } else if (!av_cmp_q(op->lin.m[i][j], Q1)) {
                        buf_appendc(&p, &rem, '1');
                    } else {
                        buf_appendc(&p, &rem, 'x');
                    }
                }
            }
        }
#endif
        break;
    case SWS_OP_DITHER:
        buf_appendf(&p, &rem, "_dither");
        break;
    }
    buf_appendf(&p, &rem, "_neon");
    buf_appendc(&p, &rem, '\0');
    if (rem == 0) {
        fprintf(stderr, "BUFFER TOO SMALL %zu\n", sizeof(sig));
    } else {
        printf("%s\n", sig);
    }

    return 0;
}

static int aarch64_compile(SwsContext *ctx, SwsOpList *ops, SwsCompiledOp *out,
                           int flags)
{
    SwsAArch64Context actx;

    /* Use at most two full vregs during the widest precision section */
    actx.block_size = (ff_sws_op_list_max_size(ops) == 4) ? 8 : 16;

    const SwsOp *read  = ff_sws_op_list_input(ops);
    const SwsOp *write = ff_sws_op_list_output(ops);
    const int read_planes  = read ? (read->rw.packed ? 1 : read->rw.elems) : 0;
    const int write_planes = write->rw.packed ? 1 : write->rw.elems;
    const int nb_planes = FFMAX(read_planes, write_planes);

    char sig[256];
    char *p = sig;
    size_t rem = sizeof(sig);
    buf_appendf(&p, &rem, "ff_sws_process_%u", nb_planes);
    aarch64_func_gen(sig);

    // printf("num_ops %d\n", ops->num_ops);
    if (flags & SWS_OP_FLAG_SIG_GEN) {
        for (int n = 0; n < ops->num_ops; n++)
            aarch64_sig_gen(&actx, ops, n);
   }

    return AVERROR(ENOTSUP);

#if 0
    const int cpu_flags = av_get_cpu_flags();
    const int mmsize = get_mmsize(cpu_flags);
    if (mmsize < 0)
        return mmsize;

    const SwsOp *read  = ff_sws_op_list_input(ops);
    const SwsOp *write = ff_sws_op_list_output(ops);
    av_assert1(write);
    int ret;

    /* Special fast path for in-place packed shuffle */
    ret = solve_shuffle(ops, mmsize, out);
    if (ret != AVERROR(ENOTSUP))
        return ret;

    SwsOpChain *chain = ff_sws_op_chain_alloc();
    if (!chain)
        return AVERROR(ENOMEM);

    *out = (SwsCompiledOp) {
        .priv = chain,
        .slice_align = 1,
        .free = ff_sws_op_chain_free_cb,

        /* Use at most two full YMM regs during the widest precision section */
        .block_size = 2 * FFMIN(mmsize, 32) / ff_sws_op_list_max_size(ops),
    };

    /* 3-component reads/writes process one extra garbage word */
    if (read && read->rw.packed && read->rw.elems == 3)
        out->over_read = sizeof(uint32_t);
    if (write->rw.packed && write->rw.elems == 3)
        out->over_write = sizeof(uint32_t);


    /* Make on-stack copy of `ops` to iterate over */
    SwsOpList rest = *ops;
    do {
        int op_block_size = out->block_size;
        SwsOp *op = &rest.ops[0];

        if (op_is_type_invariant(op)) {
            if (op->op == SWS_OP_CLEAR)
                normalize_clear(op);
            op_block_size *= ff_sws_pixel_type_size(op->type);
            op->type = SWS_PIXEL_U8;
        }

        ret = ff_sws_op_compile_tables(tables, FF_ARRAY_ELEMS(tables), &rest,
                                       op_block_size, chain);
    } while (ret == AVERROR(EAGAIN));

    if (ret < 0) {
        ff_sws_op_chain_free(chain);
        if (rest.num_ops < ops->num_ops) {
            av_log(ctx, AV_LOG_TRACE, "Uncompiled remainder:\n");
            ff_sws_op_list_print(ctx, AV_LOG_TRACE, AV_LOG_TRACE, &rest);
        }
        return ret;
    }

#define ASSIGN_PROCESS_FUNC(NAME)                               \
    do {                                                        \
        SWS_DECL_FUNC(NAME);                                    \
        void NAME##_return(void);                               \
        ret = ff_sws_op_chain_append(chain, NAME##_return,      \
                                     NULL, &(SwsOpPriv) {0});   \
        out->func = NAME;                                       \
    } while (0)

    const int read_planes  = read ? (read->rw.packed ? 1 : read->rw.elems) : 0;
    const int write_planes = write->rw.packed ? 1 : write->rw.elems;
    switch (FFMAX(read_planes, write_planes)) {
    case 1: ASSIGN_PROCESS_FUNC(ff_sws_process1_x86); break;
    case 2: ASSIGN_PROCESS_FUNC(ff_sws_process2_x86); break;
    case 3: ASSIGN_PROCESS_FUNC(ff_sws_process3_x86); break;
    case 4: ASSIGN_PROCESS_FUNC(ff_sws_process4_x86); break;
    }

    if (ret < 0) {
        ff_sws_op_chain_free(chain);
        return ret;
    }

    out->cpu_flags = chain->cpu_flags;

    return 0;
#endif
}

const SwsOpBackend backend_aarch64 = {
    .name       = "aarch64",
    .compile    = aarch64_compile,
    /* TODO call compile(flag_sig_gen) or add sig_gen() function? */
    .hw_format  = AV_PIX_FMT_NONE,
};
