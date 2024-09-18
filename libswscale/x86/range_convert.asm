;******************************************************************************
;* Copyright (c) 2024 Ramiro Polla
;*
;* This file is part of FFmpeg.
;*
;* FFmpeg is free software; you can redistribute it and/or
;* modify it under the terms of the GNU Lesser General Public
;* License as published by the Free Software Foundation; either
;* version 2.1 of the License, or (at your option) any later version.
;*
;* FFmpeg is distributed in the hope that it will be useful,
;* but WITHOUT ANY WARRANTY; without even the implied warranty of
;* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
;* Lesser General Public License for more details.
;*
;* You should have received a copy of the GNU Lesser General Public
;* License along with FFmpeg; if not, write to the Free Software
;* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
;******************************************************************************

%include "libavutil/x86/x86util.asm"

SECTION_RODATA

chr_to_mult8:       times 4 dw 4663, 0
chr_to_offset8:     times 4 dd -9289992
%define chr_to_shift8 12

chr_from_mult8:     times 4 dw 1799, 0
chr_from_offset8:   times 4 dd 4081085
%define chr_from_shift8 11

lum_to_mult8:       times 4 dw 19077, 0
lum_to_offset8:     times 4 dd -39057361
%define lum_to_shift8 14

lum_from_mult8:     times 4 dw 14071, 0
lum_from_offset8:   times 4 dd 33561947
%define lum_from_shift8 14

chr_to_max16:       times 4 dd 492400
chr_to_mult16:      times 4 dw 4663, 0
chr_to_offset16:    times 2 dq -148639872
%define chr_to_shift16 12

chr_from_mult16:    times 4 dw 1799, 0
chr_from_offset16:  times 2 dq 65297360
%define chr_from_shift16 11

lum_to_max16:       times 4 dd 483024
lum_to_mult16:      times 4 dw 4769, 0
lum_to_offset16:    times 2 dq -156229444
%define lum_to_shift16 12

lum_from_mult16:    times 4 dw 3517, 0
lum_from_offset16:  times 2 dq 134247788
%define lum_from_shift16 12

SECTION .text

; NOTE: there is no need to clamp the input when converting to jpeg range
;       in the non-16-bpc versions (like we do in the C code) because packssdw
;       will saturate the output.

;-----------------------------------------------------------------------------
; lumConvertRange
;
; void ff_lumRangeToJpeg{8,16}_<opt>(int16_t *dst, int width);
; void ff_lumRangeFromJpeg{8,16}_<opt>(int16_t *dst, int width);
;
;-----------------------------------------------------------------------------

%macro LUMCONVERTRANGE 6
cglobal %1%5, 2, 2, 5, dst, width
%if %5 == 16
    shl          widthd, 2
%elif %5 == 8
    shl          widthd, 1
%endif ; %5 == 8/16
    VBROADCASTI128   m2, [%2%5]
    VBROADCASTI128   m3, [%3%5]
%if %5 == 16
%ifid %6
    VBROADCASTI128   m4, [%6%5]
%endif
%elif %5 == 8
    pxor             m4, m4
%endif ; %5 == 8/16
    add            dstq, widthq
    neg          widthq
.loop:
    movu             m0, [dstq+widthq]
%if %5 == 16
%ifid %6
    PMINSD           m0, m4, m1
%endif
    pshufd           m1, m0, 0xb1
    pmuludq          m0, m2
    pmuludq          m1, m2
    paddq            m0, m3
    paddq            m1, m3
    psrlq            m0, %4%5
    psrlq            m1, %4%5
    pshufd           m0, m0, 0xd8
    pshufd           m1, m1, 0xd8
    punpckldq        m0, m1
%elif %5 == 8
    punpckhwd        m1, m0, m4
    punpcklwd        m0, m4
    pmaddwd          m0, m2
    pmaddwd          m1, m2
    paddd            m0, m3
    paddd            m1, m3
    psrad            m0, %4%5
    psrad            m1, %4%5
    packssdw         m0, m1
%endif ; %5 == 8/16
    movu  [dstq+widthq], m0
    add          widthq, mmsize
    jl .loop
    RET
%endmacro

;-----------------------------------------------------------------------------
; chrConvertRange
;
; void ff_chrRangeToJpeg{8,16}_<opt>(int16_t *dstU, int16_t *dstV, int width);
; void ff_chrRangeFromJpeg{8,16}_<opt>(int16_t *dstU, int16_t *dstV, int width);
;
;-----------------------------------------------------------------------------

%macro CHRCONVERTRANGE 6
cglobal %1%5, 3, 3, 7, dstU, dstV, width
%if %5 == 16
    shl          widthd, 2
%elif %5 == 8
    shl          widthd, 1
%endif ; %5 == 8/16
    VBROADCASTI128   m4, [%2%5]
    VBROADCASTI128   m5, [%3%5]
%if %5 == 16
%ifid %6
    VBROADCASTI128   m6, [%6%5]
%endif
%elif %5 == 8
    pxor             m6, m6
%endif ; %5 == 8/16
    add           dstUq, widthq
    add           dstVq, widthq
    neg          widthq
.loop:
    movu             m0, [dstUq+widthq]
    movu             m2, [dstVq+widthq]
%if %5 == 16
%ifid %6
    PMINSD           m0, m6, m1
    PMINSD           m2, m6, m3
%endif
    pshufd           m1, m0, 0xb1
    pshufd           m3, m2, 0xb1
    pmuludq          m0, m4
    pmuludq          m1, m4
    pmuludq          m2, m4
    pmuludq          m3, m4
    paddq            m0, m5
    paddq            m1, m5
    paddq            m2, m5
    paddq            m3, m5
    psrlq            m0, %4%5
    psrlq            m1, %4%5
    psrlq            m2, %4%5
    psrlq            m3, %4%5
    pshufd           m0, m0, 0xd8
    pshufd           m1, m1, 0xd8
    pshufd           m2, m2, 0xd8
    pshufd           m3, m3, 0xd8
    punpckldq        m0, m1
    punpckldq        m2, m3
%elif %5 == 8
    punpckhwd        m1, m0, m6
    punpckhwd        m3, m2, m6
    punpcklwd        m0, m6
    punpcklwd        m2, m6
    pmaddwd          m0, m4
    pmaddwd          m1, m4
    pmaddwd          m2, m4
    pmaddwd          m3, m4
    paddd            m0, m5
    paddd            m1, m5
    paddd            m2, m5
    paddd            m3, m5
    psrad            m0, %4%5
    psrad            m1, %4%5
    psrad            m2, %4%5
    psrad            m3, %4%5
    packssdw         m0, m1
    packssdw         m2, m3
%endif ; %5 == 8/16
    movu [dstUq+widthq], m0
    movu [dstVq+widthq], m2
    add          widthq, mmsize
    jl .loop
    RET
%endmacro

INIT_XMM sse2
LUMCONVERTRANGE lumRangeToJpeg,   lum_to_mult,   lum_to_offset,   lum_to_shift,    8, 0
LUMCONVERTRANGE lumRangeToJpeg,   lum_to_mult,   lum_to_offset,   lum_to_shift,   16, lum_to_max
CHRCONVERTRANGE chrRangeToJpeg,   chr_to_mult,   chr_to_offset,   chr_to_shift,    8, 0
CHRCONVERTRANGE chrRangeToJpeg,   chr_to_mult,   chr_to_offset,   chr_to_shift,   16, chr_to_max
LUMCONVERTRANGE lumRangeFromJpeg, lum_from_mult, lum_from_offset, lum_from_shift,  8, 0
LUMCONVERTRANGE lumRangeFromJpeg, lum_from_mult, lum_from_offset, lum_from_shift, 16, 0
CHRCONVERTRANGE chrRangeFromJpeg, chr_from_mult, chr_from_offset, chr_from_shift,  8, 0
CHRCONVERTRANGE chrRangeFromJpeg, chr_from_mult, chr_from_offset, chr_from_shift, 16, 0

INIT_XMM sse4
LUMCONVERTRANGE lumRangeToJpeg,   lum_to_mult,   lum_to_offset,   lum_to_shift,   16, lum_to_max
CHRCONVERTRANGE chrRangeToJpeg,   chr_to_mult,   chr_to_offset,   chr_to_shift,   16, chr_to_max

%if HAVE_AVX2_EXTERNAL
INIT_YMM avx2
LUMCONVERTRANGE lumRangeToJpeg,   lum_to_mult,   lum_to_offset,   lum_to_shift,    8, 0
LUMCONVERTRANGE lumRangeToJpeg,   lum_to_mult,   lum_to_offset,   lum_to_shift,   16, lum_to_max
CHRCONVERTRANGE chrRangeToJpeg,   chr_to_mult,   chr_to_offset,   chr_to_shift,    8, 0
CHRCONVERTRANGE chrRangeToJpeg,   chr_to_mult,   chr_to_offset,   chr_to_shift,   16, chr_to_max
LUMCONVERTRANGE lumRangeFromJpeg, lum_from_mult, lum_from_offset, lum_from_shift,  8, 0
LUMCONVERTRANGE lumRangeFromJpeg, lum_from_mult, lum_from_offset, lum_from_shift, 16, 0
CHRCONVERTRANGE chrRangeFromJpeg, chr_from_mult, chr_from_offset, chr_from_shift,  8, 0
CHRCONVERTRANGE chrRangeFromJpeg, chr_from_mult, chr_from_offset, chr_from_shift, 16, 0
%endif
