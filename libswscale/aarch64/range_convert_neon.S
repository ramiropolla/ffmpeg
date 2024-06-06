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

.macro lumConvertRange name mult offset shift
const offset_\name, align=4
        .word \offset, \offset, \offset, \offset
endconst
function ff_\name, export=1
        mov             w3, #\mult
        dup             v2.4s, w3
        movrel          x3, offset_\name
        ld1             {v3.8h}, [x3]
1:
        ld1             {v0.8h}, [x0]
        sxtl            v4.4s, v0.4h
        sxtl2           v5.4s, v0.8h
        mul             v4.4s, v4.4s, v2.4s
        mul             v5.4s, v5.4s, v2.4s
        add             v4.4s, v4.4s, v3.4s
        add             v5.4s, v5.4s, v3.4s
        sshr            v4.4s, v4.4s, #\shift
        sshr            v5.4s, v5.4s, #\shift
        sqxtn           v0.4h, v4.4s
        sqxtn2          v0.8h, v5.4s
        subs            w1, w1, #8
        st1             {v0.8h}, [x0], #16
        b.gt            1b
        ret
endfunc
.endm

.macro chrConvertRange name mult offset shift
const offset_\name, align=4
        .word \offset, \offset, \offset, \offset
endconst
function ff_\name, export=1
        mov             w3, #\mult
        dup             v2.4s, w3
        movrel          x3, offset_\name
        ld1             {v3.8h}, [x3]
1:
        ld1             {v0.8h}, [x0]
        sxtl            v4.4s, v0.4h
        sxtl2           v5.4s, v0.8h
        mul             v4.4s, v4.4s, v2.4s
        mul             v5.4s, v5.4s, v2.4s
        add             v4.4s, v4.4s, v3.4s
        add             v5.4s, v5.4s, v3.4s
        sshr            v4.4s, v4.4s, #\shift
        sshr            v5.4s, v5.4s, #\shift
        sqxtn           v0.4h, v4.4s
        sqxtn2          v0.8h, v5.4s
        ld1             {v1.8h}, [x1]
        sxtl            v4.4s, v1.4h
        sxtl2           v5.4s, v1.8h
        mul             v4.4s, v4.4s, v2.4s
        mul             v5.4s, v5.4s, v2.4s
        add             v4.4s, v4.4s, v3.4s
        add             v5.4s, v5.4s, v3.4s
        sshr            v4.4s, v4.4s, #\shift
        sshr            v5.4s, v5.4s, #\shift
        sqxtn           v1.4h, v4.4s
        sqxtn2          v1.8h, v5.4s
        subs            w2, w2, #8
        st1             {v0.8h}, [x0], #16
        st1             {v1.8h}, [x1], #16
        b.gt            1b
        ret
endfunc
.endm

lumConvertRange lumRangeToJpeg_neon,   19077, -39057361, 14
chrConvertRange chrRangeToJpeg_neon,    4663,  -9289992, 12
lumConvertRange lumRangeFromJpeg_neon, 14071,  33561947, 14
chrConvertRange chrRangeFromJpeg_neon,  1799,   4081085, 11
