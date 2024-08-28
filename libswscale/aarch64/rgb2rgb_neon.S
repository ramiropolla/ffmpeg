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
#define BU v1.h[0]
#define GU v1.h[1]
#define RU v1.h[2]
#define BV v2.h[0]
#define GV v2.h[1]
#define RV v2.h[2]
#define Y_OFFSET  v22
#define UV_OFFSET v23

// loads 8 pixels, split into separate vectors, and widen to 16-bit
.macro rgbload b, g, r, src
        ld3             {\b\().8b, \g\().8b, \r\().8b}, [\src], #24
        uxtl            \b\().8h, \b\().8b
        uxtl            \g\().8h, \g\().8b
        uxtl            \r\().8h, \r\().8b
.endm

// subsamples a line horizontally
.macro subsample dst, src1, src2
        xtn             \dst\().4h, \src1\().4s
        xtn2            \dst\().8h, \src2\().4s
.endm

// convert rgb to y, u, or v
// uses v3, v4, and v5
.macro rgbconv dst, b, g, r, bc, gc, rc, offset
        smull           v3.4s, \b\().4h, \bc
        smlal           v3.4s, \g\().4h, \gc
        smlal           v3.4s, \r\().4h, \rc
        smull2          v4.4s, \b\().8h, \bc
        smlal2          v4.4s, \g\().8h, \gc
        smlal2          v4.4s, \r\().8h, \rc        // v3:v4 = b * bc + g * gc + r * rc (32-bit)
        shrn            v5.4h, v3.4s, #7
        shrn2           v5.8h, v4.4s, #7            // v5 = b * bc + g * gc + r * rc (16-bit)
        addhn           \dst\().8b, v5.8h, \offset\().8h // dst = (b * bc + g * gc + r * rc) + offset (8-bit)
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
        ldrsw           x9,  [sp]
        ldr             x15, [sp, #8]
// x9  int srcStride
// x15 int32_t *rgb2yuv

        // src1 = x0
        // src2 = x10
        add             x10, x0,  x9                // x10 = src + srcStride
        lsl             x9,  x9,  #1                // srcStride *= 2
        add             w11, w4,  w4, lsl #1        // w11 = 3 * width
        sub             x9,  x9,  w11, sxtw         // srcPadding = (2 * srcStride) - (3 * width)

        // ydst1 = x1
        // ydst2 = x11
        add             x11, x1,  w6, sxtw          // x11 = ydst + lumStride
        lsl             w6,  w6,  #1                // lumStride *= 2
        sub             w6,  w6,  w4                // lumPadding = (2 * lumStride) - width

        sub             w7,  w7,  w4, lsr #1        // chromPadding = chromStride - (width / 2)

        // load rgb2yuv coefficients into v0, v1, and v2
        add             x15, x15, #RGB2YUV_COEFFS
        ld1             {v0.8h-v2.8h}, [x15]        // load 24 values

        // load offset constants
        movi            Y_OFFSET.8h,  #0x10, lsl #8
        movi            UV_OFFSET.8h, #0x80, lsl #8

1:
        mov             w15, w4                     // w15 = width

2:
        // first line

        rgbload         v16, v17, v18, x0           // v16 = B00, v17 = G00, v18 = R00
        rgbconv         v6, v16, v17, v18, BY, GY, RY, Y_OFFSET

        rgbload         v19, v20, v21, x0           // v19 = B01, v20 = G01, v21 = R01
        rgbconv         v7, v19, v20, v21, BY, GY, RY, Y_OFFSET

        st1             {v6.8b, v7.8b}, [x1], #16

        subsample       v24, v16, v19
        subsample       v25, v17, v20
        subsample       v26, v18, v21

        rgbconv         v6, v24, v25, v26, BU, GU, RU, UV_OFFSET
        rgbconv         v7, v24, v25, v26, BV, GV, RV, UV_OFFSET

        st1             {v6.8b}, [x2], #8           // store udst[i]
        st1             {v7.8b}, [x3], #8           // store vdst[i]

        // second line

        rgbload         v16, v17, v18, x10          // v16 = B10, v17 = G10, v18 = R10
        rgbconv         v6, v16, v17, v18, BY, GY, RY, Y_OFFSET

        rgbload         v19, v20, v21, x10          // v19 = B11, v20 = G11, v21 = R11
        rgbconv         v7, v19, v20, v21, BY, GY, RY, Y_OFFSET

        st1             {v6.8b, v7.8b}, [x11], #16

        subs            w15, w15, #16
        b.gt            2b

        // row += 2
        add             x0,  x0,  x9                // src1  += srcPadding
        add             x10, x10, x9                // src2  += srcPadding
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
