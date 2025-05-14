/*
 * memset16 - fill memory with a constant halfword
 *
 * Copyright (c) 2012-2020, Arm Limited.
 * Copyright (c) 2025 Ramiro Polla
 * SPDX-License-Identifier: MIT
 */

/* Assumptions:
 *
 * ARMv8-a, AArch64, Advanced SIMD, unaligned accesses.
 *
 */

#define dstin   x0
#define val     x1
#define valw    w1
#define count   x2
#define dst     x3
#define dstend  x4

.global ff_memset16_neon
.type ff_memset16_neon,%function
ff_memset16_neon:

        dup     v0.8h, valw
        add     dstend, dstin, count, lsl #1

        cmp     count, 96
        b.hi    .Lset_long
        cmp     count, 16
        b.hs    .Lset_medium
        mov     val, v0.D[0]

        /* Set 0..15 halfwords.  */
        tbz     count, 3, 1f
        str     q0, [dstin]
        str     q0, [dstend, -16]
        ret
        nop
1:      tbz     count, 2, 2f
        str     val, [dstin]
        str     val, [dstend, -8]
        ret
2:      cbz     count, 3f
        strh    valw, [dstin]
        tbz     count, 1, 3f
        str     valw, [dstend, -4]
3:      ret

        /* Set 17..96 halfwords.  */
.Lset_medium:
        stp     q0, q0, [dstin]
        tbnz    count, 6, .Lset96
        stp     q0, q0, [dstend, -32]
        tbz     count, 5, 1f
        stp     q0, q0, [dstin, 32]
        stp     q0, q0, [dstend, -64]
1:      ret

        .p2align 4
        /* Set 64..96 halfwords.  Write 64 halfwords from the start and
           32 halfwords from the end.  */
.Lset96:
        stp     q0, q0, [dstin, 32]
        stp     q0, q0, [dstin, 64]
        stp     q0, q0, [dstin, 96]
        stp     q0, q0, [dstend, -64]
        stp     q0, q0, [dstend, -32]
        ret

        .p2align 4
.Lset_long:
        bic     dst, dstin, 15
        str     q0, [dstin]

.Lno_zva:
        sub     count, dstend, dst      /* Count is 16 too large.  */
        sub     dst, dst, 16            /* Dst is biased by -32.  */
        sub     count, count, 64 + 16   /* Adjust count and bias for loop.  */
.Lno_zva_loop:
        stp     q0, q0, [dst, 32]
        stp     q0, q0, [dst, 64]!
        subs    count, count, 64
        b.hi    .Lno_zva_loop
        stp     q0, q0, [dstend, -64]
        stp     q0, q0, [dstend, -32]
        ret

.size ff_memset16_neon,.-ff_memset16_neon
