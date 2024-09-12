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

SECTION .text

; NOTE: there is no need to clamp the input when converting to jpeg range
;       (like we do in the C code) because packssdw will saturate the output.
;       therefore the same function can be used to and from jpeg range.

;-----------------------------------------------------------------------------
; lumConvertRange
;
; void ff_lumRangeConvert_<opt>(int16_t *dst, int width,
;                               int64_t coeff, int offset, int amax);
;
;-----------------------------------------------------------------------------

%macro LUMCONVERTRANGE 2
cglobal %1, 4, 4, 5, dst, width, coeff, offset
    shl          widthd, 1
    movsxdifnidn coeffq, coeffd
    movq            xm2, coeffq
    VBROADCASTSS     m2, xm2
    movq            xm3, offsetq
    VBROADCASTSS     m3, xm3
    pxor             m4, m4
    add            dstq, widthq
    neg          widthq
.loop:
    movu             m0, [dstq+widthq]
    punpckhwd        m1, m0, m4
    punpcklwd        m0, m4
    pmaddwd          m0, m2
    pmaddwd          m1, m2
    paddd            m0, m3
    paddd            m1, m3
    psrad            m0, %2
    psrad            m1, %2
    packssdw         m0, m1
    movu  [dstq+widthq], m0
    add          widthq, mmsize
    jl .loop
    RET
%endmacro

;-----------------------------------------------------------------------------
; chrConvertRange
;
; void ff_chrRangeConvert_<opt>(int16_t *dstU, int16_t *dstV, int width
;                               int64_t coeff, int offset, int amax);
;
;-----------------------------------------------------------------------------

%macro CHRCONVERTRANGE 2
cglobal %1, 5, 5, 7, dstU, dstV, width, coeff, offset
    shl          widthd, 1
    movsxdifnidn coeffq, coeffd
    movq            xm4, coeffq
    VBROADCASTSS     m4, xm4
    movq            xm5, offsetq
    VBROADCASTSS     m5, xm5
    pxor             m6, m6
    add           dstUq, widthq
    add           dstVq, widthq
    neg          widthq
.loop:
    movu             m0, [dstUq+widthq]
    movu             m2, [dstVq+widthq]
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
    psrad            m0, %2
    psrad            m1, %2
    psrad            m2, %2
    psrad            m3, %2
    packssdw         m0, m1
    packssdw         m2, m3
    movu [dstUq+widthq], m0
    movu [dstVq+widthq], m2
    add          widthq, mmsize
    jl .loop
    RET
%endmacro

INIT_XMM sse2
LUMCONVERTRANGE lumRangeConvert,   14
CHRCONVERTRANGE chrRangeConvert,   14
LUMCONVERTRANGE lumRangeConvert16, 16
CHRCONVERTRANGE chrRangeConvert16, 16

%if HAVE_AVX2_EXTERNAL
INIT_YMM avx2
LUMCONVERTRANGE lumRangeConvert,   14
CHRCONVERTRANGE chrRangeConvert,   14
LUMCONVERTRANGE lumRangeConvert16, 16
CHRCONVERTRANGE chrRangeConvert16, 16
%endif
