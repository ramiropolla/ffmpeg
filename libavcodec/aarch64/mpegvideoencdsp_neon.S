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

.macro sum_line add_insn, postinc
.ifb \postinc
        ld1             { v1.16b }, [x0]
.else
        ld1             { v1.16b }, [x0], \postinc
.endif
        \add_insn       v0.8h, v1.16b
.endm

function ff_pix_sum_neon, export=1
// x0  const uint8_t *pix
// x1  int line_size

        uxtw            x1, w1

        sum_line        uaddlp, x1
        sum_line        uadalp, x1
        sum_line        uadalp, x1
        sum_line        uadalp, x1
        sum_line        uadalp, x1
        sum_line        uadalp, x1
        sum_line        uadalp, x1
        sum_line        uadalp, x1
        sum_line        uadalp, x1
        sum_line        uadalp, x1
        sum_line        uadalp, x1
        sum_line        uadalp, x1
        sum_line        uadalp, x1
        sum_line        uadalp, x1
        sum_line        uadalp, x1
        sum_line        uadalp

        uaddlp          v0.4s, v0.8h
        uaddlv          d0, v0.4s
        fmov            w0, s0

        ret
endfunc

.macro norm1_line add_insn, postinc
.ifb \postinc
        ld1             { v2.16b }, [x0]
.else
        ld1             { v2.16b }, [x0], \postinc
.endif
        umull           v0.8h, v2.8b,  v2.8b
        umull2          v1.8h, v2.16b, v2.16b
        \add_insn       v3.4s, v0.8h
        \add_insn       v4.4s, v1.8h
.endm

function ff_pix_norm1_neon, export=1
// x0  const uint8_t *pix
// x1  int line_size

        uxtw            x1, w1

        norm1_line      uaddlp, x1
        norm1_line      uadalp, x1
        norm1_line      uadalp, x1
        norm1_line      uadalp, x1
        norm1_line      uadalp, x1
        norm1_line      uadalp, x1
        norm1_line      uadalp, x1
        norm1_line      uadalp, x1
        norm1_line      uadalp, x1
        norm1_line      uadalp, x1
        norm1_line      uadalp, x1
        norm1_line      uadalp, x1
        norm1_line      uadalp, x1
        norm1_line      uadalp, x1
        norm1_line      uadalp, x1
        norm1_line      uadalp

        add             v0.4s, v3.4s, v4.4s
        uaddlv          d0, v0.4s
        fmov            w0, s0

        ret
endfunc
