/*
 * Copyright (c) 2012-2020, Arm Limited.
 * Copyright (c) 2025 Ramiro Polla
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "libavutil/aarch64/asm.S"

#define dstin   x0
#define val     x1
#define valw    w1
#define count   x2
#define dst     x3
#define dstend  x4

.macro MEMSET_LONG
        /* Set at least 96 bytes using q0. */
        bic             dst, dstin, #15
        stp             q0, q0, [dstin]
        sub             count, dstend, dst      /* Count is 16 too large. */
        sub             count, count, #96       /* Adjust count and bias for loop. */
0:      stp             q0, q0, [dst, #32]
        stp             q0, q0, [dst, #64]!
        subs            count, count, #64
        b.hi            0b
        stp             q0, q0, [dstend, #-64]
        stp             q0, q0, [dstend, #-32]
.endm

function ff_memset16_neon, export=1, align=6
        dup             v0.8h, valw
        add             dstend, dstin, count, lsl #1
        cmp             count, #48
        b.ls            .Lmemset16_small
        MEMSET_LONG
        ret
        .p2align 4
.Lmemset16_small:
        mov             val, v0.d[0]
        /* Set 0..48 halfwords. */
        tbz             count, #5, 4f
        stp             q0, q0, [dstin]
        stp             q0, q0, [dstend, #-32]
        stp             q0, q0, [dstin, #32]
        ret
        .p2align 4
4:      tbz             count, #4, 3f
        stp             q0, q0, [dstin]
        stp             q0, q0, [dstend, #-32]
        ret
3:      tbz             count, #3, 2f
        str             q0, [dstin]
        str             q0, [dstend, #-16]
        ret
2:      tbz             count, #2, 1f
        str             val, [dstin]
        str             val, [dstend, #-8]
        ret
1:      cbz             count, 0f
        strh            valw, [dstin]
        tbz             count, #1, 0f
        str             valw, [dstend, #-4]
0:      ret
endfunc

function ff_memset32_neon, export=1, align=6
        dup             v0.4s, valw
        add             dstend, dstin, count, lsl #2
        cmp             count, #24
        b.ls            .Lmemset32_small
        MEMSET_LONG
        ret
        .p2align 4
.Lmemset32_small:
        mov             val, v0.d[0]
        /* Set 0..24 words. */
        tbz             count, #4, 3f
        stp             q0, q0, [dstin]
        stp             q0, q0, [dstend, #-32]
        stp             q0, q0, [dstin, #32]
        ret
        .p2align 4
3:      tbz             count, #3, 2f
        stp             q0, q0, [dstin]
        stp             q0, q0, [dstend, #-32]
        ret
2:      tbz             count, #2, 1f
        str             q0, [dstin]
        str             q0, [dstend, #-16]
        ret
1:      cbz             count, 0f
        str             valw, [dstin]
        tbz             count, #1, 0f
        str             val, [dstend, #-8]
0:      ret
endfunc

#undef dstin
#undef val
#undef valw
#undef count
#undef dst
#undef dstend

#define dstin   x0
#define src     x1
#define count   x2
#define shift   x3
#define shiftw  w3
#define dst     x4
#define srcend  x5
#define dstend  x6
#define A_l     x7
#define A_lw    w7
#define B_l     x8
#define B_lw    w8
#define C_l     x9
#define C_lw    w9
#define tmp1    x10

.macro MEMSWAP_LONG revinsn
        /* Copy more than 96 bytes. */
        /* Copy 16 bytes and then align dst to 16-byte alignment. */
        ldr             q0, [src]
        and             tmp1, dstin, #15
        bic             dst, dstin, #15
        \revinsn        v0.16b, v0.16b
        sub             src, src, tmp1
        sub             count, dstend, dst      /* Count is 16 too large. */
        str             q0, [dstin]
        ldp             q0, q1, [src, #16]
        ldp             q2, q3, [src, #48]
        add             src, src, #64
        \revinsn        v0.16b, v0.16b
        \revinsn        v1.16b, v1.16b
        \revinsn        v2.16b, v2.16b
        \revinsn        v3.16b, v3.16b
        subs            count, count, #144      /* Test and readjust count. */
        b.ls            1f
        .p2align 4
0:      stp             q0, q1, [dst, #16]
        ldp             q0, q1, [src, #16]
        stp             q2, q3, [dst, #48]
        ldp             q2, q3, [src, #48]
        add             dst, dst, #64
        add             src, src, #64
        \revinsn        v0.16b, v0.16b
        \revinsn        v1.16b, v1.16b
        \revinsn        v2.16b, v2.16b
        \revinsn        v3.16b, v3.16b
        subs            count, count, #64
        b.hi            0b
        /* Write the last iteration and copy 64 bytes from the end. */
        .p2align 4
1:      stp             q0, q1, [dst, #16]
        ldp             q0, q1, [srcend, #-64]
        stp             q2, q3, [dst, #48]
        ldp             q2, q3, [srcend, #-32]
        \revinsn        v0.16b, v0.16b
        \revinsn        v1.16b, v1.16b
        \revinsn        v2.16b, v2.16b
        \revinsn        v3.16b, v3.16b
        stp             q0, q1, [dstend, #-64]
        stp             q2, q3, [dstend, #-32]
.endm

function ff_bswap16_neon, export=1, align=6
        add             srcend, src, count, lsl #1
        add             dstend, dstin, count, lsl #1
        cmp             count, #48
        b.ls            .Lbswap16_small
        MEMSWAP_LONG    rev16
        ret
.Lbswap16_small:
        tbz             count, #5, 4f
        ldp             q0, q1, [src]
        ldp             q2, q3, [src, #32]
        ldp             q4, q5, [srcend, #-32]
        rev16           v0.16b, v0.16b
        rev16           v1.16b, v1.16b
        rev16           v2.16b, v2.16b
        rev16           v3.16b, v3.16b
        rev16           v4.16b, v4.16b
        rev16           v5.16b, v5.16b
        stp             q0, q1, [dstin]
        stp             q2, q3, [dstin, #32]
        stp             q4, q5, [dstend, #-32]
        ret
        .p2align 4
4:      tbz             count, #4, 3f
        ldp             q0, q1, [src]
        ldp             q2, q3, [srcend, #-32]
        rev16           v0.16b, v0.16b
        rev16           v1.16b, v1.16b
        rev16           v2.16b, v2.16b
        rev16           v3.16b, v3.16b
        stp             q0, q1, [dstin]
        stp             q2, q3, [dstend, #-32]
        ret
        .p2align 4
3:      tbz             count, #3, 2f
        ldr             q0, [src]
        ldr             q1, [srcend, #-16]
        rev16           v0.16b, v0.16b
        rev16           v1.16b, v1.16b
        str             q0, [dstin]
        str             q1, [dstend, #-16]
        ret
        .p2align 4
2:      tbz             count, #2, 1f
        ldr             A_l, [src]
        ldr             B_l, [srcend, #-8]
        rev16           A_l, A_l
        rev16           B_l, B_l
        str             A_l, [dstin]
        str             B_l, [dstend, #-8]
        ret
        .p2align 4
1:      cbz             count, 0f
        bic             tmp1, count, #1
        ldrh            A_lw, [src]
        ldrh            B_lw, [src, tmp1]
        ldrh            C_lw, [srcend, #-2]
        rev16           A_lw, A_lw
        rev16           B_lw, B_lw
        rev16           C_lw, C_lw
        strh            A_lw, [dstin]
        strh            B_lw, [dstin, tmp1]
        strh            C_lw, [dstend, #-2]
0:      ret
endfunc

function ff_bswap32_neon, export=1, align=6
        add             srcend, src, count, lsl #2
        add             dstend, dstin, count, lsl #2
        cmp             count, #24
        b.ls            .Lbswap32_small
        MEMSWAP_LONG    rev32
        ret
.Lbswap32_small:
        tbz             count, #4, 3f
        ldp             q0, q1, [src]
        ldp             q2, q3, [src, #32]
        ldp             q4, q5, [srcend, #-32]
        rev32           v0.16b, v0.16b
        rev32           v1.16b, v1.16b
        rev32           v2.16b, v2.16b
        rev32           v3.16b, v3.16b
        rev32           v4.16b, v4.16b
        rev32           v5.16b, v5.16b
        stp             q0, q1, [dstin]
        stp             q2, q3, [dstin, #32]
        stp             q4, q5, [dstend, #-32]
        ret
        .p2align 4
3:      tbz             count, #3, 2f
        ldp             q0, q1, [src]
        ldp             q2, q3, [srcend, #-32]
        rev32           v0.16b, v0.16b
        rev32           v1.16b, v1.16b
        rev32           v2.16b, v2.16b
        rev32           v3.16b, v3.16b
        stp             q0, q1, [dstin]
        stp             q2, q3, [dstend, #-32]
        ret
        .p2align 4
2:      tbz             count, #2, 1f
        ldr             q0, [src]
        ldr             q1, [srcend, #-16]
        rev32           v0.16b, v0.16b
        rev32           v1.16b, v1.16b
        str             q0, [dstin]
        str             q1, [dstend, #-16]
        ret
        .p2align 4
1:      cbz             count, 0f
        bic             tmp1, count, #1
        lsl             tmp1, tmp1, #1
        ldr             A_lw, [src]
        ldr             B_lw, [src, tmp1]
        ldr             C_lw, [srcend, #-4]
        rev32           A_l, A_l
        rev32           B_l, B_l
        rev32           C_l, C_l
        str             A_lw, [dstin]
        str             B_lw, [dstin, tmp1]
        str             C_lw, [dstend, #-4]
0:      ret
endfunc

.macro MEMSHIFT_LONG vet
        /* Copy more than 96 bytes. */
        /* Copy 16 bytes and then align dst to 16-byte alignment. */
        ldr             q0, [src]
        and             tmp1, dstin, #15
        bic             dst, dstin, #15
        ushl            v0.\vet, v0.\vet, v10.\vet
        sub             src, src, tmp1
        sub             count, dstend, dst      /* Count is 16 too large. */
        str             q0, [dstin]
        ldp             q0, q1, [src, #16]
        ldp             q2, q3, [src, #48]
        add             src, src, #64
        ushl            v0.\vet, v0.\vet, v10.\vet
        ushl            v1.\vet, v1.\vet, v10.\vet
        ushl            v2.\vet, v2.\vet, v10.\vet
        ushl            v3.\vet, v3.\vet, v10.\vet
        subs            count, count, #144      /* Test and readjust count. */
        b.ls            1f
        .p2align 4
0:      stp             q0, q1, [dst, #16]
        ldp             q0, q1, [src, #16]
        stp             q2, q3, [dst, #48]
        ldp             q2, q3, [src, #48]
        add             dst, dst, #64
        add             src, src, #64
        ushl            v0.\vet, v0.\vet, v10.\vet
        ushl            v1.\vet, v1.\vet, v10.\vet
        ushl            v2.\vet, v2.\vet, v10.\vet
        ushl            v3.\vet, v3.\vet, v10.\vet
        subs            count, count, #64
        b.hi            0b
        /* Write the last iteration and copy 64 bytes from the end. */
        .p2align 4
1:      stp             q0, q1, [dst, #16]
        ldp             q0, q1, [srcend, #-64]
        stp             q2, q3, [dst, #48]
        ldp             q2, q3, [srcend, #-32]
        ushl            v0.\vet, v0.\vet, v10.\vet
        ushl            v1.\vet, v1.\vet, v10.\vet
        ushl            v2.\vet, v2.\vet, v10.\vet
        ushl            v3.\vet, v3.\vet, v10.\vet
        stp             q0, q1, [dstend, #-64]
        stp             q2, q3, [dstend, #-32]
.endm

function ff_lshift16_neon, export=1, align=6
        dup             v10.8h, shiftw
        add             srcend, src, count, lsl #1
        add             dstend, dstin, count, lsl #1
        cmp             count, #48
        b.ls            .Llshift16_small
        MEMSHIFT_LONG   8h
        ret
.Llshift16_small:
        tbz             count, #5, 4f
        ldp             q0, q1, [src]
        ldp             q2, q3, [src, #32]
        ldp             q4, q5, [srcend, #-32]
        ushl            v0.8h, v0.8h, v10.8h
        ushl            v1.8h, v1.8h, v10.8h
        ushl            v2.8h, v2.8h, v10.8h
        ushl            v3.8h, v3.8h, v10.8h
        ushl            v4.8h, v4.8h, v10.8h
        ushl            v5.8h, v5.8h, v10.8h
        stp             q0, q1, [dstin]
        stp             q2, q3, [dstin, #32]
        stp             q4, q5, [dstend, #-32]
        ret
        .p2align 4
4:      tbz             count, #4, 3f
        ldp             q0, q1, [src]
        ldp             q2, q3, [srcend, #-32]
        ushl            v0.8h, v0.8h, v10.8h
        ushl            v1.8h, v1.8h, v10.8h
        ushl            v2.8h, v2.8h, v10.8h
        ushl            v3.8h, v3.8h, v10.8h
        stp             q0, q1, [dstin]
        stp             q2, q3, [dstend, #-32]
        ret
        .p2align 4
3:      tbz             count, #3, 2f
        ldr             q0, [src]
        ldr             q1, [srcend, #-16]
        ushl            v0.8h, v0.8h, v10.8h
        ushl            v1.8h, v1.8h, v10.8h
        str             q0, [dstin]
        str             q1, [dstend, #-16]
        ret
        .p2align 4
2:      tbz             count, #2, 1f
        ldr             d0, [src]
        ldr             d1, [srcend, #-8]
        ushl            v0.4h, v0.4h, v10.4h
        ushl            v1.4h, v1.4h, v10.4h
        str             d0, [dstin]
        str             d1, [dstend, #-8]
        ret
        .p2align 4
1:      cbz             count, 0f
        bic             tmp1, count, #1
        ldrh            A_lw, [src]
        ldrh            B_lw, [src, tmp1]
        ldrh            C_lw, [srcend, #-2]
        lsl             A_lw, A_lw, shiftw
        lsl             B_lw, B_lw, shiftw
        lsl             C_lw, C_lw, shiftw
        strh            A_lw, [dstin]
        strh            B_lw, [dstin, tmp1]
        strh            C_lw, [dstend, #-2]
0:      ret
endfunc

function ff_lshift32_neon, export=1, align=6
        dup             v10.8h, shiftw
        add             srcend, src, count, lsl #2
        add             dstend, dstin, count, lsl #2
        cmp             count, #24
        b.ls            .Llshift32_small
        MEMSHIFT_LONG   4s
        ret
.Llshift32_small:
        tbz             count, #4, 3f
        ldp             q0, q1, [src]
        ldp             q2, q3, [src, #32]
        ldp             q4, q5, [srcend, #-32]
        ushl            v0.4s, v0.4s, v10.4s
        ushl            v1.4s, v1.4s, v10.4s
        ushl            v2.4s, v2.4s, v10.4s
        ushl            v3.4s, v3.4s, v10.4s
        ushl            v4.4s, v4.4s, v10.4s
        ushl            v5.4s, v5.4s, v10.4s
        stp             q0, q1, [dstin]
        stp             q2, q3, [dstin, #32]
        stp             q4, q5, [dstend, #-32]
        ret
        .p2align 4
3:      tbz             count, #3, 2f
        ldp             q0, q1, [src]
        ldp             q2, q3, [srcend, #-32]
        ushl            v0.4s, v0.4s, v10.4s
        ushl            v1.4s, v1.4s, v10.4s
        ushl            v2.4s, v2.4s, v10.4s
        ushl            v3.4s, v3.4s, v10.4s
        stp             q0, q1, [dstin]
        stp             q2, q3, [dstend, #-32]
        ret
        .p2align 4
2:      tbz             count, #2, 1f
        ldr             q0, [src]
        ldr             q1, [srcend, #-16]
        ushl            v0.4s, v0.4s, v10.4s
        ushl            v1.4s, v1.4s, v10.4s
        str             q0, [dstin]
        str             q1, [dstend, #-16]
        ret
        .p2align 4
1:      cbz             count, 0f
        bic             tmp1, count, #1
        lsl             tmp1, tmp1, #1
        ldr             A_lw, [src]
        ldr             B_lw, [src, tmp1]
        ldr             C_lw, [srcend, #-4]
        lsl             A_l, A_l, shift
        lsl             B_l, B_l, shift
        lsl             C_l, C_l, shift
        str             A_lw, [dstin]
        str             B_lw, [dstin, tmp1]
        str             C_lw, [dstend, #-4]
0:      ret
endfunc
