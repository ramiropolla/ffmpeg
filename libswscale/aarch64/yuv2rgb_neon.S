/*
 * Copyright (c) 2016 Matthieu Bouron <matthieu.bouron stupeflix.com>
 * Copyright (c) 2016 Clément Bœsch <clement stupeflix.com>
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

#define width        w0
#define widthx       x0
#define height       w1
#define cur_width    w8
#define orig_height  w9

#define srcY         x2
#define srcC         x3
#define srcU         x3
#define srcV         x4
#define srcPaddingY  x10
#define srcPaddingC  x11
#define srcPaddingU  x11
#define srcPaddingV  x12
#define srcPaddingYw w10
#define srcPaddingCw w11
#define srcPaddingUw w11
#define srcPaddingVw w12

#define dst          x7
#define dst0         x7
#define dst1         x6
#define dst2         x5
#define dstPadding   x13
#define dstPadding0  x13
#define dstPadding1  x14
#define dstPadding2  x15
#define dstPaddingw  w13
#define dstPadding0w w13
#define dstPadding1w w14
#define dstPadding2w w15

#define l2_dst1      x16
#define l2_dst2      x17

// (callee-saved)
#define l2_srcY      x19
#define l2_dst       x20
#define l2_dst0      x20

/*********************************************************************/
/* src nv12 */

.macro src_load_args_nv12
// x5  const uint8_t *src[]
// x6  int *srcStride[]
        ldp             srcPaddingYw, srcPaddingCw, [x6]                // srcStride[0], srcStride[1]
        ldp             srcY,         srcC,         [x5]                // src[0], src[1]
        sxtw            srcPaddingY,  srcPaddingYw
        sxtw            srcPaddingC,  srcPaddingCw
        add             l2_srcY,      srcY,         srcPaddingY         // l2_srcY = srcY + srcStride[0]
        lsl             srcPaddingY,  srcPaddingY,  #1                  // srcPaddingY *= 2
        sub             srcPaddingY,  srcPaddingY,  widthx              // srcPaddingY = srcStride[0] * 2 - width
        sub             srcPaddingC,  srcPaddingC,  widthx              // srcPaddingC = srcStride[1] - width
.endm

.macro src_load_chroma_nv12
        ld2             {v28.8b, v29.8b}, [srcC], #16
        ushll           v30.8h, v28.8b, #3
        ushll           v31.8h, v29.8b, #3
.endm

.macro src_increment_nv12
        add             srcY,    srcY,    srcPaddingY                   // srcY    += srcPaddingY
        add             l2_srcY, l2_srcY, srcPaddingY                   // l2_srcY += srcPaddingY
        add             srcC,    srcC,    srcPaddingC                   // srcC    += srcPaddingC
.endm

/*********************************************************************/
/* src nv21 */

.macro src_load_args_nv21
        src_load_args_nv12
.endm

.macro src_load_chroma_nv21
        ld2             {v28.8b, v29.8b}, [srcC], #16
        ushll           v31.8h, v28.8b, #3
        ushll           v30.8h, v29.8b, #3
.endm

.macro src_increment_nv21
        src_increment_nv12
.endm

/*********************************************************************/
/* src yuv422p */

.macro src_load_args_yuv422p
// x5  const uint8_t *src[]
// x6  int *srcStride[]
        ldp             srcPaddingYw, srcPaddingUw, [x6]                // srcStride[0], srcStride[1]
        ldr             srcPaddingVw,               [x6, #8]            // srcStride[2]
        ldp             srcY,         srcU,         [x5]                // src[0], src[1]
        ldr             srcV,                       [x5, #16]           // src[2]
        sxtw            srcPaddingY,  srcPaddingYw
        sxtw            srcPaddingU,  srcPaddingUw
        sxtw            srcPaddingV,  srcPaddingVw
        sub             srcPaddingY,  srcPaddingY,  widthx              // srcPaddingY = srcStride[0] - width
        sub             srcPaddingU,  srcPaddingU,  widthx, lsr #1      // srcPaddingU = srcStride[1] - width / 2
        sub             srcPaddingV,  srcPaddingV,  widthx, lsr #1      // srcPaddingV = srcStride[2] - width / 2
.endm

.macro src_load_chroma_yuv422p
        ldr             d28,    [srcU], #8
        ldr             d29,    [srcV], #8
        ushll           v30.8h, v28.8b, #3
        ushll           v31.8h, v29.8b, #3
.endm

.macro src_increment_yuv422p
        add             srcY, srcY, srcPaddingY                         // srcY += srcPaddingY
        add             srcU, srcU, srcPaddingU                         // srcU += srcPaddingU
        add             srcV, srcV, srcPaddingV                         // srcV += srcPaddingV
.endm

/*********************************************************************/
/* src yuv420p */

.macro src_load_args_yuv420p
// x5  const uint8_t *src[]
// x6  int *srcStride[]
        ldp             srcPaddingYw, srcPaddingUw, [x6]                // srcStride[0], srcStride[1]
        ldr             srcPaddingVw,               [x6, #8]            // srcStride[2]
        ldp             srcY,         srcU,         [x5]                // src[0], src[1]
        ldr             srcV,                       [x5, #16]           // src[2]
        sxtw            srcPaddingY,  srcPaddingYw
        sxtw            srcPaddingU,  srcPaddingUw
        sxtw            srcPaddingV,  srcPaddingVw
        add             l2_srcY,      srcY,         srcPaddingY         // l2_srcY = srcY + srcStride[0]
        lsl             srcPaddingY,  srcPaddingY,  #1                  // srcPaddingY *= 2
        sub             srcPaddingY,  srcPaddingY,  widthx              // srcPaddingY = srcStride[0] * 2 - width
        sub             srcPaddingU,  srcPaddingU,  widthx, lsr #1      // srcPaddingU = srcStride[1] - width / 2
        sub             srcPaddingV,  srcPaddingV,  widthx, lsr #1      // srcPaddingV = srcStride[2] - width / 2
.endm

.macro src_load_chroma_yuv420p
        src_load_chroma_yuv422p
.endm

.macro src_increment_yuv420p
        add             srcY,    srcY,    srcPaddingY                   // srcY    += srcPaddingY
        add             l2_srcY, l2_srcY, srcPaddingY                   // l2_srcY += srcPaddingY
        add             srcU,    srcU,    srcPaddingU                   // srcU    += srcPaddingU
        add             srcV,    srcV,    srcPaddingV                   // srcV    += srcPaddingV
.endm

/*********************************************************************/
/* dst packed */

.macro dst_load_args_packed nb_lines, bpp
// x7  uint8_t *dst
// sp1 int dstStride
        ldr             dstPaddingw, [sp]
        sxtw            dstPadding,  dstPaddingw
.ifc \nb_lines,2
        add             l2_dst, dst, dstPadding                         // l2_dst = dst + dstStride[0]
        lsl             dstPadding, dstPadding, #1                      // dstPadding *= 2
.endif
.ifc \bpp,3
        sub             dstPadding, dstPadding, widthx, lsl #1
        sub             dstPadding, dstPadding, widthx                  // dstPadding = dstStride - width * 3
.else
        sub             dstPadding, dstPadding, widthx, lsl #2          // dstPadding = dstStride - width * 4
.endif
.endm

.macro dst_increment_packed
        add             dst, dst, dstPadding                            // dst += dstPadding
.endm

/*********************************************************************/
/* dst planar */

.macro dst_load_args_planar nb_lines
// x7  uint8_t *dst0
// sp1 int dstStride0
// sp2 uint8_t *dst1
// sp3 int dstStride1
// sp4 uint8_t *dst2
// sp5 int dstStride2
        ldr             dstPadding0w, [sp]
        ldr             dst1,         [sp, #8]
        ldr             dstPadding1w, [sp, #16]
        ldr             dst2,         [sp, #24]
        ldr             dstPadding2w, [sp, #32]
        sxtw            dstPadding0,  dstPadding0w
        sxtw            dstPadding1,  dstPadding1w
        sxtw            dstPadding2,  dstPadding2w
.ifc \nb_lines,2
        add             l2_dst0, dst0, dstPadding0                      // l2_dst0 = dst0 + dstStride[0]
        add             l2_dst1, dst1, dstPadding1                      // l2_dst1 = dst1 + dstStride[1]
        add             l2_dst2, dst2, dstPadding2                      // l2_dst2 = dst2 + dstStride[2]
        lsl             dstPadding0, dstPadding0, #1                    // dstPadding0 *= 2
        lsl             dstPadding1, dstPadding1, #1                    // dstPadding1 *= 2
        lsl             dstPadding2, dstPadding2, #1                    // dstPadding2 *= 2
.endif
        sub             dstPadding0, dstPadding0, widthx                // dstPadding0 = dstStride0 - width
        sub             dstPadding1, dstPadding1, widthx                // dstPadding1 = dstStride1 - width
        sub             dstPadding2, dstPadding2, widthx                // dstPadding2 = dstStride2 - width
.endm

.macro dst_increment_planar
        add             dst0, dst0, dstPadding0                         // dst0 += dstPadding0
        add             dst1, dst1, dstPadding1                         // dst1 += dstPadding1
        add             dst2, dst2, dstPadding2                         // dst2 += dstPadding2
.endm

/*********************************************************************/
.macro dst_load_args_argb nb_lines
        dst_load_args_packed \nb_lines, 4
.endm

.macro dst_load_args_rgba nb_lines
        dst_load_args_packed \nb_lines, 4
.endm

.macro dst_load_args_rgb24 nb_lines
        dst_load_args_packed \nb_lines, 3
.endm

.macro dst_load_args_abgr nb_lines
        dst_load_args_packed \nb_lines, 4
.endm

.macro dst_load_args_bgra nb_lines
        dst_load_args_packed \nb_lines, 4
.endm

.macro dst_load_args_bgr24 nb_lines
        dst_load_args_packed \nb_lines, 3
.endm

.macro dst_load_args_gbrp nb_lines
        dst_load_args_planar \nb_lines
.endm

/*********************************************************************/
.macro dst_init_alpha_argb
        movi            v4.8b, #255
        movi            v16.8b, #255
.endm

.macro dst_init_alpha_rgba
        movi            v7.8b, #255
        movi            v19.8b, #255
.endm

.macro dst_init_alpha_rgb24
.endm

.macro dst_init_alpha_abgr
        dst_init_alpha_argb
.endm

.macro dst_init_alpha_bgra
        dst_init_alpha_rgba
.endm

.macro dst_init_alpha_bgr24
.endm

.macro dst_init_alpha_gbrp
.endm

/*********************************************************************/
.macro compute_rgb r1, g1, b1, r2, g2, b2
        ushll           v26.8h, v2.8b,  #3                              // Y1*(1<<3)
        ushll2          v27.8h, v2.16b, #3                              // Y2*(1<<3)
        sub             v26.8h, v26.8h, v3.8h                           // Y1*(1<<3) - y_offset
        sub             v27.8h, v27.8h, v3.8h                           // Y2*(1<<3) - y_offset
        sqdmulh         v26.8h, v26.8h, v0.8h                           // ((Y1*(1<<3) - y_offset) * y_coeff) >> 15
        sqdmulh         v27.8h, v27.8h, v0.8h                           // ((Y2*(1<<3) - y_offset) * y_coeff) >> 15
        add             \r1\().8h, v26.8h, v20.8h                       // Y1 + R1
        add             \r2\().8h, v27.8h, v21.8h                       // Y2 + R2
        add             \g1\().8h, v26.8h, v22.8h                       // Y1 + G1
        add             \g2\().8h, v27.8h, v23.8h                       // Y2 + G2
        add             \b1\().8h, v26.8h, v24.8h                       // Y1 + B1
        add             \b2\().8h, v27.8h, v25.8h                       // Y2 + B2
        sqrshrun        \r1\().8b, \r1\().8h, #1                        // clip_u8((Y1 + R1) >> 1)
        sqrshrun        \r2\().8b, \r2\().8h, #1                        // clip_u8((Y2 + R1) >> 1)
        sqrshrun        \g1\().8b, \g1\().8h, #1                        // clip_u8((Y1 + G1) >> 1)
        sqrshrun        \g2\().8b, \g2\().8h, #1                        // clip_u8((Y2 + G1) >> 1)
        sqrshrun        \b1\().8b, \b1\().8h, #1                        // clip_u8((Y1 + B1) >> 1)
        sqrshrun        \b2\().8b, \b2\().8h, #1                        // clip_u8((Y2 + B1) >> 1)
.endm

/*********************************************************************/
.macro store_rgba ldst
        st4             { v4.8b, v5.8b, v6.8b, v7.8b}, [\ldst], #32
        st4             {v16.8b,v17.8b,v18.8b,v19.8b}, [\ldst], #32
.endm

.macro store_rgb24 ldst
        st3             { v4.8b, v5.8b, v6.8b}, [\ldst], #24
        st3             {v16.8b,v17.8b,v18.8b}, [\ldst], #24
.endm

.macro store_gbrp ldst0, ldst1, ldst2
        stp              d4,  d5, [\ldst0], #16
        stp              d6,  d7, [\ldst1], #16
        stp             d18, d19, [\ldst2], #16
.endm

/*********************************************************************/
.macro convert_argb ldst0, ldst1, ldst2 // 1 2 3
        compute_rgb     v5,v6,v7, v17,v18,v19
        store_rgba \ldst0
.endm

.macro convert_rgba ldst0, ldst1, ldst2 // 0 1 2
        compute_rgb     v4,v5,v6, v16,v17,v18
        store_rgba \ldst0
.endm

.macro convert_rgb24 ldst0, ldst1, ldst2 // 0 1 2
        compute_rgb     v4,v5,v6, v16,v17,v18
        store_rgb24 \ldst0
.endm

.macro convert_abgr ldst0, ldst1, ldst2 // 3 2 1
        compute_rgb     v7,v6,v5, v19,v18,v17
        store_rgba \ldst0
.endm

.macro convert_bgra ldst0, ldst1, ldst2 // 2 1 0
        compute_rgb     v6,v5,v4, v18,v17,v16
        store_rgba \ldst0
.endm

.macro convert_bgr24 ldst0, ldst1, ldst2 // 2 1 0
        compute_rgb     v6,v5,v4, v18,v17,v16
        store_rgb24 \ldst0
.endm

.macro convert_gbrp ldst0, ldst1, ldst2
        compute_rgb     v18,v4,v6, v19,v5,v7
        store_gbrp \ldst0, \ldst1, \ldst2
.endm

/*********************************************************************/
.macro declare_func ifmt, ofmt, dsttype, nb_lines
function ff_\ifmt\()_to_\ofmt\()_neon, export=1
// w0  int w
// w1  int h
// w2  int y_offset
// w3  int y_coeff
// x4  const int16_t *yuv2rgb_table

        uxtw            widthx, width
        dup             v3.8h, w2                                       // y_offset
        dup             v0.8h, w3                                       // y_coeff
        ldr             q1, [x4]                                        // yuv2rgb_table

.ifc \nb_lines,2
        stp             l2_srcY, l2_dst0, [sp, #-16]
.endif

        src_load_args_\ifmt
        dst_load_args_\ofmt \nb_lines
        dst_init_alpha_\ofmt

        mov             orig_height, height
        mov             cur_width, width                                // cur_width = width

.align 4
1:
        movi            v5.8h, #4, lsl #8                               // 128 * (1<<3)
        src_load_chroma_\ifmt
        sub             v30.8h, v30.8h, v5.8h                           // U*(1<<3) - 128*(1<<3)
        sub             v31.8h, v31.8h, v5.8h                           // V*(1<<3) - 128*(1<<3)
        sqdmulh         v20.8h, v31.8h, v1.h[0]                         // V * v2r            (R)
        sqdmulh         v22.8h, v30.8h, v1.h[1]                         // U * u2g
        sqdmulh         v31.8h, v31.8h, v1.h[2]                         //           V * v2g
        add             v22.8h, v22.8h, v31.8h                          // U * u2g + V * v2g  (G)
        sqdmulh         v24.8h, v30.8h, v1.h[3]                         // U * u2b            (B)
        zip2            v21.8h, v20.8h, v20.8h                          // R2
        zip1            v20.8h, v20.8h, v20.8h                          // R1
        zip2            v23.8h, v22.8h, v22.8h                          // G2
        zip1            v22.8h, v22.8h, v22.8h                          // G1
        zip2            v25.8h, v24.8h, v24.8h                          // B2
        zip1            v24.8h, v24.8h, v24.8h                          // B1
        ldr             q2, [srcY], #16                                 // load luma
        convert_\ofmt dst0, dst1, dst2
.ifc \nb_lines,2
        ldr             q2, [l2_srcY], #16                              // load luma (second line)
        convert_\ofmt l2_dst0, l2_dst1, l2_dst2
.endif
        subs            cur_width, cur_width, #16                       // cur_width -= 16
        b.gt            1b
        mov             cur_width, width                                // cur_width = width
        dst_increment_\dsttype
        src_increment_\ifmt
        subs            height, height, #\nb_lines                      // height -= nb_lines
        b.gt            1b
        mov             w0, orig_height
.ifc \nb_lines,2
        ldp             l2_srcY, l2_dst0, [sp, #-16]
.endif
        ret
endfunc
.endm

.macro declare_rgb_funcs ifmt, nb_lines
        declare_func    \ifmt, argb,  packed, \nb_lines
        declare_func    \ifmt, rgba,  packed, \nb_lines
        declare_func    \ifmt, rgb24, packed, \nb_lines
        declare_func    \ifmt, abgr,  packed, \nb_lines
        declare_func    \ifmt, bgra,  packed, \nb_lines
        declare_func    \ifmt, bgr24, packed, \nb_lines
        declare_func    \ifmt, gbrp,  planar, \nb_lines
.endm

declare_rgb_funcs nv12,    2
declare_rgb_funcs nv21,    2
declare_rgb_funcs yuv420p, 2
declare_rgb_funcs yuv422p, 1
