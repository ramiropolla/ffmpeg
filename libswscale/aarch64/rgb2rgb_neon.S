/*
 * Copyright (c) 2020 Martin Storsjo
 * Copyright (c) 2024 Ramiro Polla
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

#define RGB2YUV_COEFFS 16*4+16*32
#define BY v0.h[0]
#define GY v0.h[1]
#define RY v0.h[2]
#define BU v0.h[4]
#define GU v0.h[5]
#define RU v0.h[6]
#define BV v1.h[0]
#define GV v1.h[1]
#define RV v1.h[2]
#define YOFF  v22
#define UVOFF v23

// loads 8 pixels, split into separate vectors, and widen to 16-bit
.macro rgbload b, g, r, src
        ld3             {\b\().8b, \g\().8b, \r\().8b}, [\src], #24
        uxtl            \b\().8h, \b\().8b
        uxtl            \g\().8h, \g\().8b
        uxtl            \r\().8h, \r\().8b
.endm

// subsamples a line horizontally
// uses v2
.macro subsample src1, src2, dst
        xtn             \dst\().4h, \src1\().4s
        xtn             v2.4h, \src2\().4s
        ins             \dst\().d[1], v2.d[0]
.endm

// convert rgb to y, u, or v
// uses v2, v3, and v4
.macro rgbconv b, g, r, bc, gc, rc, offset, dst
        smull           v2.4s, \b\().4h, \bc
        smull2          v3.4s, \b\().8h, \bc
        smlal           v2.4s, \g\().4h, \gc
        smlal2          v3.4s, \g\().8h, \gc
        smlal           v2.4s, \r\().4h, \rc
        smlal2          v3.4s, \r\().8h, \rc        // v2:v3 = b * bc + g * gc + r * rc (32-bit)
        shrn            v4.4h, v2.4s, #7
        shrn2           v4.8h, v3.4s, #7            // v4 = b * bc + g * gc + r * rc (16-bit)
        addhn           \dst\().8b, v4.8h, \offset\().8h // v5 = (b * bc + g * gc + r * rc) + offset (8-bit)
.endm

// void ff_rgb24toyv12_neon(const uint8_t *src, uint8_t *ydst, uint8_t *udst,
//                          uint8_t *vdst, int width, int height, int lumStride,
//                          int chromStride, int srcStride, int32_t *rgb2yuv);
function ff_rgb24toyv12_neon, export=1
// x0  const uint8_t *src
// x1  uint8_t *ydst
// x2  uint8_t *udst
// x3  uint8_t *vdst
// w4  int width
// w5  int height
// w6  int lumStride
// w7  int chromStride
        ldr             w9,  [sp]
        ldr             x15, [sp, #8]
// w9  int srcStride
// x15 int32_t *rgb2yuv

        // src1 = x0
        // src2 = x10
        add             x10, x0,  w9, sxtw          // x10 = src + srcStride
        lsl             w9,  w9,  #1                // srcStride *= 2
        add             w11, w4,  w4, lsl #1        // w11 = 3 * width
        sub             w9,  w9,  w11               // srcPadding = (2 * srcStride) - (3 * width)

        // ydst1 = x1
        // ydst2 = x11
        add             x11, x1,  w6, sxtw          // x11 = ydst + lumStride
        lsl             w6,  w6,  #1                // lumStride *= 2
        sub             w6,  w6,  w4                // lumPadding = (2 * lumStride) - width

        sub             w7,  w7,  w4, lsr #1        // chromPadding = chromStride - (width / 2)

        // load rgb2yuv coefficients into v0 and v1
        add             x15, x15, #RGB2YUV_COEFFS
        ld1             {v0.8h-v2.8h}, [x15]        // load 24 values
        ins             v0.d[1], v1.d[0]            // merge v0 and v1 coefficients into v0
        mov             v1.16b, v2.16b              // move v2 coefficients down to v1

        movi            v22.8h, #0x10
        movi            v23.8h, #0x80
        shl             v22.8h, v22.8h, #8
        shl             v23.8h, v23.8h, #8

1:
        mov             w15, w4                     // w15 = width

2:
        // first line

        rgbload         v16, v17, v18, x0           // v16 = B00, v17 = G00, v18 = R00
        rgbconv         v16, v17, v18, BY, GY, RY, YOFF, v5

        rgbload         v19, v20, v21, x0           // v19 = B01, v20 = G01, v21 = R01
        rgbconv         v19, v20, v21, BY, GY, RY, YOFF, v6

        // merge v6 into v5 and store in x1
        ins             v5.d[1], v6.d[0]
        st1             {v5.16b}, [x1], #16

        subsample       v16, v19, v24
        subsample       v17, v20, v25
        subsample       v18, v21, v26

        rgbconv         v24, v25, v26, BU, GU, RU, UVOFF, v5
        rgbconv         v24, v25, v26, BV, GV, RV, UVOFF, v6

        st1             {v5.8b}, [x2], #8           // store udst[i]
        st1             {v6.8b}, [x3], #8           // store vdst[i]

        // second line

        rgbload         v16, v17, v18, x10          // v16 = B10, v17 = G10, v18 = R10
        rgbconv         v16, v17, v18, BY, GY, RY, YOFF, v5

        rgbload         v19, v20, v21, x10          // v19 = B11, v20 = G11, v21 = R11
        rgbconv         v19, v20, v21, BY, GY, RY, YOFF, v6

        // merge v6 into v5 and store in x11
        ins             v5.d[1], v6.d[0]
        st1             {v5.16b}, [x11], #16

        subs            w15, w15, #16
        b.gt            2b

        // row += 2
        add             x0,  x0,  w9, sxtw          // src1  += srcPadding
        add             x10, x10, w9, sxtw          // src2  += srcPadding
        add             x1,  x1,  w6, sxtw          // ydst1 += lumPadding
        add             x11, x11, w6, sxtw          // ydst2 += lumPadding
        add             x2,  x2,  w7, sxtw          // udst  += chromPadding
        add             x3,  x3,  w7, sxtw          // vdst  += chromPadding
        subs            w5, w5, #2
        b.gt            1b

        ret
endfunc

// void ff_interleave_bytes_neon(const uint8_t *src1, const uint8_t *src2,
//                               uint8_t *dest, int width, int height,
//                               int src1Stride, int src2Stride, int dstStride);
function ff_interleave_bytes_neon, export=1
        sub             w5,  w5,  w3
        sub             w6,  w6,  w3
        sub             w7,  w7,  w3, lsl #1
1:
        ands            w8,  w3,  #0xfffffff0 // & ~15
        b.eq            3f
2:
        ld1             {v0.16b}, [x0], #16
        ld1             {v1.16b}, [x1], #16
        subs            w8,  w8,  #16
        st2             {v0.16b, v1.16b}, [x2], #32
        b.gt            2b

        tst             w3,  #15
        b.eq            9f

3:
        tst             w3,  #8
        b.eq            4f
        ld1             {v0.8b}, [x0], #8
        ld1             {v1.8b}, [x1], #8
        st2             {v0.8b, v1.8b}, [x2], #16
4:
        tst             w3,  #4
        b.eq            5f

        ld1             {v0.s}[0], [x0], #4
        ld1             {v1.s}[0], [x1], #4
        zip1            v0.8b,   v0.8b,   v1.8b
        st1             {v0.8b}, [x2], #8

5:
        ands            w8,  w3,  #3
        b.eq            9f
6:
        ldrb            w9,  [x0], #1
        ldrb            w10, [x1], #1
        subs            w8,  w8,  #1
        bfi             w9,  w10, #8,  #8
        strh            w9,  [x2], #2
        b.gt            6b

9:
        subs            w4,  w4,  #1
        b.eq            0f
        add             x0,  x0,  w5, sxtw
        add             x1,  x1,  w6, sxtw
        add             x2,  x2,  w7, sxtw
        b               1b

0:
        ret
endfunc
