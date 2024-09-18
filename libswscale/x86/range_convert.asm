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

;-----------------------------------------------------------------------------
; lumConvertRange
;
; void ff_lumRangeToJpeg{8,16}_<opt>(int16_t *dst, int width,
;                                    int amax, int coeff, int64_t offset);
; void ff_lumRangeFromJpeg{8,16}_<opt>(int16_t *dst, int width,
;                                      int amax, int coeff, int64_t offset);
;
;-----------------------------------------------------------------------------

%macro LUMCONVERTRANGE 3
%ifidni %2,To
%if %3 == 16
cglobal %1Range%2Jpeg%3, 5, 5, 5, dst, width, amax, coeff, offset
%elif %3 == 8
cglobal %1Range%2Jpeg%3, 5, 5, 6, dst, width, amax, coeff, offset
%endif ; %3 == 8/16
%else
cglobal %1Range%2Jpeg%3, 5, 5, 5, dst, width, amax, coeff, offset
%endif
%if %3 == 16
    shl          widthd, 2
%elif %3 == 8
    shl          widthd, 1
%endif ; %3 == 8/16
    movd            xm2, coeffd
    VBROADCASTSS     m2, xm2
%if ARCH_X86_64
    movq            xm3, offsetq
%else
    movq            xm3, offsetm
%endif
%if %3 == 16
    VBROADCASTSD     m3, xm3
%elif %3 == 8
    VBROADCASTSS     m3, xm3
    pxor             m4, m4
%endif ; %3 == 8/16
%ifidni %2,To
%if %3 == 16
    movd            xm4, amaxd
    VBROADCASTSS     m4, xm4
%elif %3 == 8
    movd            xm5, amaxd
    SPLATW           m5, xm5
%endif ; %3 == 8/16
%endif
    add            dstq, widthq
    neg          widthq
.loop:
    movu             m0, [dstq+widthq]
%if %3 == 16
%ifidni %2,To
    PMINSD           m0, m4, m1
%endif
    pshufd           m1, m0, 0xb1
    pmuludq          m0, m2
    pmuludq          m1, m2
    paddq            m0, m3
    paddq            m1, m3
    psrlq            m0, 18
    psrlq            m1, 18
    pshufd           m0, m0, 0xd8
    pshufd           m1, m1, 0xd8
    punpckldq        m0, m1
%elif %3 == 8
%ifidni %2,To
    pminsw           m0, m5
%endif
    punpckhwd        m1, m0, m4
    punpcklwd        m0, m4
    pmaddwd          m0, m2
    pmaddwd          m1, m2
    paddd            m0, m3
    paddd            m1, m3
    psrad            m0, 14
    psrad            m1, 14
    packssdw         m0, m1
%endif ; %3 == 8/16
    movu  [dstq+widthq], m0
    add          widthq, mmsize
    jl .loop
    RET
%endmacro

;-----------------------------------------------------------------------------
; chrConvertRange
;
; void ff_chrRangeToJpeg{8,16}_<opt>(int16_t *dstU, int16_t *dstV, int width,
;                                    int amax, int coeff, int64_t offset);
; void ff_chrRangeFromJpeg{8,16}_<opt>(int16_t *dstU, int16_t *dstV, int width,
;                                      int amax, int coeff, int64_t offset);
;
;-----------------------------------------------------------------------------

%macro CHRCONVERTRANGE 3
%ifidni %2,To
%if %3 == 16
cglobal %1Range%2Jpeg%3, 6, 6, 7, dstU, dstV, width, amax, coeff, offset
%elif %3 == 8
cglobal %1Range%2Jpeg%3, 6, 6, 8, dstU, dstV, width, amax, coeff, offset
%endif ; %3 == 8/16
%else
cglobal %1Range%2Jpeg%3, 6, 6, 7, dstU, dstV, width, amax, coeff, offset
%endif
%if %3 == 16
    shl          widthd, 2
%elif %3 == 8
    shl          widthd, 1
%endif ; %3 == 8/16
    movd            xm4, coeffd
    VBROADCASTSS     m4, xm4
%if ARCH_X86_64
    movq            xm5, offsetq
%else
    movq            xm5, offsetm
%endif
%if %3 == 16
    VBROADCASTSD     m5, xm5
%elif %3 == 8
    VBROADCASTSS     m5, xm5
    pxor             m6, m6
%endif ; %3 == 8/16
%ifidni %2,To
%if %3 == 16
    movd            xm6, amaxd
    VBROADCASTSS     m6, xm6
%elif %3 == 8
    movd            xm7, amaxd
    SPLATW           m7, xm7
%endif ; %3 == 8/16
%endif
    add           dstUq, widthq
    add           dstVq, widthq
    neg          widthq
.loop:
    movu             m0, [dstUq+widthq]
    movu             m2, [dstVq+widthq]
%if %3 == 16
%ifidni %2,To
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
    psrlq            m0, 18
    psrlq            m1, 18
    psrlq            m2, 18
    psrlq            m3, 18
    pshufd           m0, m0, 0xd8
    pshufd           m1, m1, 0xd8
    pshufd           m2, m2, 0xd8
    pshufd           m3, m3, 0xd8
    punpckldq        m0, m1
    punpckldq        m2, m3
%elif %3 == 8
%ifidni %2,To
    pminsw           m0, m7
    pminsw           m2, m7
%endif
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
    psrad            m0, 14
    psrad            m1, 14
    psrad            m2, 14
    psrad            m3, 14
    packssdw         m0, m1
    packssdw         m2, m3
%endif ; %3 == 8/16
    movu [dstUq+widthq], m0
    movu [dstVq+widthq], m2
    add          widthq, mmsize
    jl .loop
    RET
%endmacro

INIT_XMM sse2
LUMCONVERTRANGE lum, To,    8
LUMCONVERTRANGE lum, To,   16
CHRCONVERTRANGE chr, To,    8
CHRCONVERTRANGE chr, To,   16
LUMCONVERTRANGE lum, From,  8
LUMCONVERTRANGE lum, From, 16
CHRCONVERTRANGE chr, From,  8
CHRCONVERTRANGE chr, From, 16

INIT_XMM sse4
LUMCONVERTRANGE lum, To,    8
LUMCONVERTRANGE lum, To,   16
CHRCONVERTRANGE chr, To,    8
CHRCONVERTRANGE chr, To,   16

%if HAVE_AVX2_EXTERNAL
INIT_YMM avx2
LUMCONVERTRANGE lum, To,    8
LUMCONVERTRANGE lum, To,   16
CHRCONVERTRANGE chr, To,    8
CHRCONVERTRANGE chr, To,   16
LUMCONVERTRANGE lum, From,  8
LUMCONVERTRANGE lum, From, 16
CHRCONVERTRANGE chr, From,  8
CHRCONVERTRANGE chr, From, 16
%endif
