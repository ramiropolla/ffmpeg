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

function ff_quantize_neon, export=1
// x0  int16_t *temp_block
// x1  int16_t *block
// x2  const int16_t *qmat
// x3  const int16_t *bias
// x4  int *max_qcoeff
// w5  int last_non_zero_p1

        // x6 = ff_inv_zigzag_direct16
        movrel          x6, ff_inv_zigzag_direct16
        // w7 = i
        mov             w7, #64
        // v6 = max
        movi            v6.16b, #0
        // v7 = last_non_zero_p1
        dup             v7.8h, w5

1:
        ld1             {v0.8h}, [x1], #16          // v0 = block[i]
        ld1             {v1.8h}, [x3], #16          // v1 = bias[i]
        ld1             {v2.8h}, [x2], #16          // v2 = qmat[i]
        ld1             {v3.8h}, [x6], #16          // v3 = ff_inv_zigzag_direct16[i]
        stp             xzr, xzr, [x1, #-16]        // block[i] = 0
        sshr            v2.8h, v2.8h, #1            // v2 >>= 1; (since we use sqdmulh, which doubles the result)
        cmgt            v4.8h, v0.8h, #0            // v4 = block[i] > 0
        abs             v0.8h, v0.8h                // v0 = abs(block[i])
        usqadd          v0.8h, v1.8h                // v0 = abs(block[i]) + bias[i]
        sqdmulh         v0.8h, v0.8h, v2.8h         // v0 = ((abs(block[i]) + bias[i]) * qmat[i]) >> 16
        orr             v6.16b, v6.16b, v0.16b      // v6 |= v0
        eor             v0.16b, v0.16b, v4.16b      // v0 = v0 ^ v4
        sub             v0.8h, v4.8h, v0.8h         // v0 = v4 - v0 (merges positive and negative values)
        st1             {v0.8h}, [x0], #16          // temp_block[i] = v0
        cmeq            v4.8h, v0.8h, #0            // v4 = level != 0
        bic             v5.16b, v3.16b, v4.16b      // v5 = v4 ? v3 : 0
        umax            v7.8h, v7.8h, v5.8h         // v7 = max(v7, v5)

        subs            w7, w7, #8
        b.gt            1b

        // *max_qcoeff = max
        umaxv           h0, v6.8h
        fmov            w0, s0
        str             w0, [x4]

        // return last_non_zero_p1
        umaxv           h0, v7.8h
        fmov            w0, s0

        ret
endfunc
