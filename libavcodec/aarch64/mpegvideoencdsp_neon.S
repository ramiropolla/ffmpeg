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

function ff_pix_sum16_neon, export=1
// x0  const uint8_t *pix
// x1  int line_size

        add             x2, x0, w1, sxtw
        lsl             w1, w1, #1

        ld1             { v1.16b }, [x0], x1
        ld1             { v2.16b }, [x2], x1
        ld1             { v3.16b }, [x0], x1
        uaddlp          v0.8h, v1.16b
        ld1             { v1.16b }, [x2], x1
        uadalp          v0.8h, v2.16b
        ld1             { v2.16b }, [x0], x1
        uadalp          v0.8h, v3.16b
        ld1             { v3.16b }, [x2], x1
        uadalp          v0.8h, v1.16b
        ld1             { v1.16b }, [x0], x1
        uadalp          v0.8h, v2.16b
        ld1             { v2.16b }, [x2], x1
        uadalp          v0.8h, v3.16b
        ld1             { v3.16b }, [x0], x1
        uadalp          v0.8h, v1.16b
        ld1             { v1.16b }, [x2], x1
        uadalp          v0.8h, v2.16b
        ld1             { v2.16b }, [x0], x1
        uadalp          v0.8h, v3.16b
        ld1             { v3.16b }, [x2], x1
        uadalp          v0.8h, v1.16b
        ld1             { v1.16b }, [x0], x1
        uadalp          v0.8h, v2.16b
        ld1             { v2.16b }, [x2], x1
        uadalp          v0.8h, v3.16b
        ld1             { v3.16b }, [x0], x1
        uadalp          v0.8h, v1.16b
        ld1             { v1.16b }, [x2]
        uadalp          v0.8h, v2.16b
        uadalp          v0.8h, v3.16b
        uadalp          v0.8h, v1.16b

        uaddlp          v0.4s, v0.8h
        uaddlv          d0, v0.4s
        fmov            w0, s0

        ret
endfunc

function ff_pix_norm1_neon, export=1
// x0  const uint8_t *pix
// x1  int line_size

        add             x2, x0, w1, sxtw
        lsl             w1, w1, #1

        ld1             { v1.16b }, [x0], x1
        ld1             { v2.16b }, [x2], x1
        ld1             { v3.16b }, [x0], x1
        umull           v4.8h, v1.8b,  v1.8b
        umull2          v5.8h, v1.16b, v1.16b
        ld1             { v1.16b }, [x2], x1
        uaddlp          v6.4s, v4.8h
        uaddlp          v7.4s, v5.8h
        umull           v4.8h, v2.8b,  v2.8b
        umull2          v5.8h, v2.16b, v2.16b
        ld1             { v2.16b }, [x0], x1
        uadalp          v6.4s, v4.8h
        uadalp          v7.4s, v5.8h
        umull           v4.8h, v3.8b,  v3.8b
        umull2          v5.8h, v3.16b, v3.16b
        ld1             { v3.16b }, [x2], x1
        uadalp          v6.4s, v4.8h
        uadalp          v7.4s, v5.8h
        umull           v4.8h, v1.8b,  v1.8b
        umull2          v5.8h, v1.16b, v1.16b
        ld1             { v1.16b }, [x0], x1
        uadalp          v6.4s, v4.8h
        uadalp          v7.4s, v5.8h
        umull           v4.8h, v2.8b,  v2.8b
        umull2          v5.8h, v2.16b, v2.16b
        ld1             { v2.16b }, [x2], x1
        uadalp          v6.4s, v4.8h
        uadalp          v7.4s, v5.8h
        umull           v4.8h, v3.8b,  v3.8b
        umull2          v5.8h, v3.16b, v3.16b
        ld1             { v3.16b }, [x0], x1
        uadalp          v6.4s, v4.8h
        uadalp          v7.4s, v5.8h
        umull           v4.8h, v1.8b,  v1.8b
        umull2          v5.8h, v1.16b, v1.16b
        ld1             { v1.16b }, [x2], x1
        uadalp          v6.4s, v4.8h
        uadalp          v7.4s, v5.8h
        umull           v4.8h, v2.8b,  v2.8b
        umull2          v5.8h, v2.16b, v2.16b
        ld1             { v2.16b }, [x0], x1
        uadalp          v6.4s, v4.8h
        uadalp          v7.4s, v5.8h
        umull           v4.8h, v3.8b,  v3.8b
        umull2          v5.8h, v3.16b, v3.16b
        ld1             { v3.16b }, [x2], x1
        uadalp          v6.4s, v4.8h
        uadalp          v7.4s, v5.8h
        umull           v4.8h, v1.8b,  v1.8b
        umull2          v5.8h, v1.16b, v1.16b
        ld1             { v1.16b }, [x0], x1
        uadalp          v6.4s, v4.8h
        uadalp          v7.4s, v5.8h
        umull           v4.8h, v2.8b,  v2.8b
        umull2          v5.8h, v2.16b, v2.16b
        ld1             { v2.16b }, [x2], x1
        uadalp          v6.4s, v4.8h
        uadalp          v7.4s, v5.8h
        umull           v4.8h, v3.8b,  v3.8b
        umull2          v5.8h, v3.16b, v3.16b
        ld1             { v3.16b }, [x0], x1
        uadalp          v6.4s, v4.8h
        uadalp          v7.4s, v5.8h
        umull           v4.8h, v1.8b,  v1.8b
        umull2          v5.8h, v1.16b, v1.16b
        ld1             { v1.16b }, [x2]
        uadalp          v6.4s, v4.8h
        uadalp          v7.4s, v5.8h
        umull           v4.8h, v2.8b,  v2.8b
        umull2          v5.8h, v2.16b, v2.16b
        uadalp          v6.4s, v4.8h
        uadalp          v7.4s, v5.8h
        umull           v4.8h, v3.8b,  v3.8b
        umull2          v5.8h, v3.16b, v3.16b
        uadalp          v6.4s, v4.8h
        uadalp          v7.4s, v5.8h
        umull           v4.8h, v1.8b,  v1.8b
        umull2          v5.8h, v1.16b, v1.16b
        uadalp          v6.4s, v4.8h
        uadalp          v7.4s, v5.8h

        add             v0.4s, v6.4s, v7.4s
        uaddlv          d0, v0.4s
        fmov            w0, s0

        ret
endfunc

#if HAVE_DOTPROD
ENABLE_DOTPROD

function ff_pix_norm1_dotprod, export=1
// x0  const uint8_t *pix
// x1  int line_size

        add             x2, x0, w1, sxtw
        lsl             w1, w1, #1

        movi            v0.16b, #0

        ld1             { v1.16b }, [x0], x1
        ld1             { v2.16b }, [x2], x1
        ld1             { v3.16b }, [x0], x1
        udot            v0.4s, v1.16b, v1.16b
        ld1             { v1.16b }, [x2], x1
        udot            v0.4s, v2.16b, v2.16b
        ld1             { v2.16b }, [x0], x1
        udot            v0.4s, v3.16b, v3.16b
        ld1             { v3.16b }, [x2], x1
        udot            v0.4s, v1.16b, v1.16b
        ld1             { v1.16b }, [x0], x1
        udot            v0.4s, v2.16b, v2.16b
        ld1             { v2.16b }, [x2], x1
        udot            v0.4s, v3.16b, v3.16b
        ld1             { v3.16b }, [x0], x1
        udot            v0.4s, v1.16b, v1.16b
        ld1             { v1.16b }, [x2], x1
        udot            v0.4s, v2.16b, v2.16b
        ld1             { v2.16b }, [x0], x1
        udot            v0.4s, v3.16b, v3.16b
        ld1             { v3.16b }, [x2], x1
        udot            v0.4s, v1.16b, v1.16b
        ld1             { v1.16b }, [x0], x1
        udot            v0.4s, v2.16b, v2.16b
        ld1             { v2.16b }, [x2], x1
        udot            v0.4s, v3.16b, v3.16b
        ld1             { v3.16b }, [x0], x1
        udot            v0.4s, v1.16b, v1.16b
        ld1             { v1.16b }, [x2]
        udot            v0.4s, v2.16b, v2.16b
        udot            v0.4s, v3.16b, v3.16b
        udot            v0.4s, v1.16b, v1.16b

        uaddlv          d0, v0.4s
        fmov            w0, s0

        ret
endfunc

DISABLE_DOTPROD
#endif
