/*
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

function ff_nv24_to_yuv420p_chroma_neon, export=1
// x0  uint8_t *dst1
// x1  int dstStride1
// x2  uint8_t *dst2
// x3  int dstStride2
// x4  const uint8_t *src
// x5  int srcStride
// w6  int w
// w7  int h

        uxtw            x1, w1
        uxtw            x3, w3
        uxtw            x5, w5

        add             x9, x4, x5                  // x9 = src + srcStride
        lsl             w5, w5, #1                  // srcStride *= 2

1:
        mov             w10, w6                     // w10 = w
        mov             x11, x4                     // x11 = src1 (line 1)
        mov             x12, x9                     // x12 = src2 (line 2)
        mov             x13, x0                     // x13 = dst1 (dstU)
        mov             x14, x2                     // x14 = dst2 (dstV)

2:
        ld2             { v0.16b, v1.16b }, [x11], #32 // v0 = U1, v1 = V1
        ld2             { v2.16b, v3.16b }, [x12], #32 // v2 = U2, v3 = V2

        uaddlp          v0.8h, v0.16b               // pairwise add U1 into v0
        uaddlp          v1.8h, v1.16b               // pairwise add V1 into v1
        uadalp          v0.8h, v2.16b               // pairwise add U2, accumulate into v0
        uadalp          v1.8h, v3.16b               // pairwise add V2, accumulate into v1

        shrn            v0.8b, v0.8h, #2            // divide by 4
        shrn            v1.8b, v1.8h, #2            // divide by 4

        st1             { v0.8b }, [x13], #8        // store U into dst1
        st1             { v1.8b }, [x14], #8        // store V into dst2

        subs            w10, w10, #8
        b.gt            2b

        // next row
        add             x4, x4, x5                  // src1 += srcStride * 2
        add             x9, x9, x5                  // src2 += srcStride * 2
        add             x0, x0, x1                  // dst1 += dstStride1
        add             x2, x2, x3                  // dst2 += dstStride2

        subs            w7, w7, #2
        b.gt            1b

        ret
endfunc
