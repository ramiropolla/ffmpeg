/*
 * FDCT AArch64 NEON optimisations
 *
 * Copyright (c) 2017 Google Inc.
 *
 * Author: Skal (pascal.massimino@gmail.com)
 * Converted to assembly by Ramiro Polla
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

#include "libavutil/aarch64/asm.S"
#include "neon.S"

.macro butterfly dst_m, dst_p, src1, src2
        add             \dst_p, \src1, \src2
        sub             \dst_m, \src1, \src2
.endm

.macro mult_s32 dst_l, dst_h, src1, src2
        smull           \dst_l\().4s, \src1\().4h, \src2\().4h
        smull2          \dst_h\().4s, \src1\().8h, \src2\().8h
.endm

.macro madd_s32 dst_l, dst_h, src1, src2
        smlal           \dst_l\().4s, \src1\().4h, \src2\().4h
        smlal2          \dst_h\().4s, \src1\().8h, \src2\().8h
.endm

.macro msub_s32 dst_l, dst_h, src1, src2
        smlsl           \dst_l\().4s, \src1\().4h, \src2\().4h
        smlsl2          \dst_h\().4s, \src1\().8h, \src2\().8h
.endm

# Constants
.macro MK_TABLE_CST c0, c1, c2, c3
        .short \c0, \c1, \c2, \c3, \c0, \c3, \c2, \c1
.endm

#define kTan1   (13036)   // = tan(pi/16)
#define kTan2   (27146)   // = tan(2.pi/16) = sqrt(2) - 1.
#define kTan3m1 (-21746)  // = tan(3.pi/16) - 1
#define k2Sqrt2 (23170)   // = 1 / 2.sqrt(2)
const fdct_consts, align=4
        # vertical pass
        .short kTan1   / 2
        .short kTan2   / 2
        .short kTan3m1 / 2
        .short k2Sqrt2 / 2
        .short 0, 0, 0, 0 // padding
        # horizontal pass (even part)
        MK_TABLE_CST 21407, 29692, 27969, 25172
        MK_TABLE_CST 16384, 22725, 21407, 19266
        MK_TABLE_CST 8867, 12299, 11585, 10426
        # horizontal pass (odd part)
        MK_TABLE_CST 22725, 31521, 29692, 26722
        MK_TABLE_CST 19266, 26722, 25172, 22654
        MK_TABLE_CST 12873, 17855, 16819, 15137
        MK_TABLE_CST 4520, 6270, 5906, 5315
endconst

function ff_fdct_neon, export=1

        # Vertical pass

        mov             x7, x0
        ld1             { v0.8h-v3.8h }, [x7], #64
        ld1             { v4.8h-v7.8h }, [x7]

        butterfly       v16.8h, v27.8h, v0.8h, v7.8h
        butterfly       v17.8h, v26.8h, v1.8h, v6.8h
        butterfly       v18.8h, v25.8h, v2.8h, v5.8h
        butterfly       v19.8h, v24.8h, v3.8h, v4.8h
        butterfly       v23.8h, v20.8h, v27.8h, v24.8h
        butterfly       v22.8h, v21.8h, v26.8h, v25.8h

        # see comment in COLUMN_DCT8
        shl             v16.8h, v16.8h, #2
        shl             v17.8h, v17.8h, #3
        shl             v18.8h, v18.8h, #3
        shl             v19.8h, v19.8h, #2
        shl             v20.8h, v20.8h, #2
        shl             v21.8h, v21.8h, #2
        shl             v22.8h, v22.8h, #2
        shl             v23.8h, v23.8h, #2

        butterfly       v4.8h, v0.8h, v20.8h, v21.8h

        movrel          x7, fdct_consts
        ld1             { v7.8h }, [x7], #16

        sqdmulh         v30.8h, v23.8h, v7.h[1]
        sqdmulh         v31.8h, v22.8h, v7.h[1]

        sub             v6.8h, v30.8h, v22.8h
        add             v2.8h, v31.8h, v23.8h

        # see comment in COLUMN_DCT8
        butterfly       v28.8h, v29.8h, v17.8h, v18.8h
        sqdmulh         v30.8h, v29.8h, v7.h[3]
        sqdmulh         v31.8h, v28.8h, v7.h[3]
        butterfly       v23.8h, v21.8h, v19.8h, v31.8h
        butterfly       v20.8h, v22.8h, v16.8h, v30.8h

        sqdmulh         v30.8h, v23.8h, v7.h[2]
        sqdmulh         v31.8h, v21.8h, v7.h[0]
        add             v16.8h, v30.8h, v23.8h
        add             v17.8h, v31.8h, v22.8h

        # CORRECT_LSB
        movi            v31.8h, #1
        add             v18.8h, v16.8h, v31.8h
        sqdmulh         v30.8h, v20.8h, v7.h[2]
        add             v19.8h, v30.8h, v20.8h

        add             v1.8h, v17.8h, v31.8h
        sub             v3.8h, v20.8h, v18.8h
        add             v5.8h, v23.8h, v19.8h
        sqdmulh         v30.8h, v22.8h, v7.h[0]
        sub             v7.8h, v30.8h, v21.8h

        transpose_8x8h  v0, v1, v2, v3, v4, v5, v6, v7, v16, v17

        # Horizontal pass

        butterfly       v16.8h, v27.8h, v0.8h, v7.8h
        butterfly       v17.8h, v26.8h, v1.8h, v6.8h
        butterfly       v18.8h, v25.8h, v2.8h, v5.8h
        butterfly       v19.8h, v24.8h, v3.8h, v4.8h
        butterfly       v23.8h, v20.8h, v27.8h, v24.8h
        butterfly       v22.8h, v21.8h, v26.8h, v25.8h

        // even part
        ld1             { v28.8h-v30.8h }, [x7], #48

        mult_s32        v24, v25, v20, v29
        madd_s32        v24, v25, v21, v29
        uzp2            v0.8h, v24.8h, v25.8h

        mult_s32        v24, v25, v20, v29
        msub_s32        v24, v25, v21, v29
        uzp2            v4.8h, v24.8h, v25.8h

        mult_s32        v24, v25, v23, v28
        madd_s32        v24, v25, v22, v30
        uzp2            v2.8h, v24.8h, v25.8h

        mult_s32        v24, v25, v23, v30
        msub_s32        v24, v25, v22, v28
        uzp2            v6.8h, v24.8h, v25.8h

        # odd part
        ld1             { v28.8h-v31.8h }, [x7]

        mult_s32        v24, v25, v16, v28
        mult_s32        v26, v27, v16, v29
        mult_s32        v20, v21, v16, v30
        mult_s32        v22, v23, v16, v31

        madd_s32        v24, v25, v17, v29
        msub_s32        v26, v27, v17, v31
        msub_s32        v20, v21, v17, v28
        msub_s32        v22, v23, v17, v30

        madd_s32        v24, v25, v18, v30
        msub_s32        v26, v27, v18, v28
        madd_s32        v20, v21, v18, v31
        madd_s32        v22, v23, v18, v29

        madd_s32        v24, v25, v19, v31
        msub_s32        v26, v27, v19, v30
        madd_s32        v20, v21, v19, v29
        msub_s32        v22, v23, v19, v28

        uzp2            v1.8h, v24.8h, v25.8h
        uzp2            v3.8h, v26.8h, v27.8h
        uzp2            v5.8h, v20.8h, v21.8h
        uzp2            v7.8h, v22.8h, v23.8h

        # final transpose
        transpose_8x8h  v0, v1, v2, v3, v4, v5, v6, v7, v16, v17

        # and storage.
        mov             x7, x0
        st1             { v0.8h-v3.8h }, [x7], #64
        st1             { v4.8h-v7.8h }, [x7]

        ret
endfunc
