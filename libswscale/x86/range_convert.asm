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

chr_to_mult:        times 8 dd 4663
chr_to_offset:      times 8 dd -9289992
%define chr_to_shift 12

chr_from_mult:      times 8 dd 1799
chr_from_offset:    times 8 dd 4081085
%define chr_from_shift 11

lum_to_mult:        times 8 dd 19077
lum_to_offset:      times 8 dd -39057361
%define lum_to_shift 14

lum_from_mult:      times 8 dd 14071
lum_from_offset:    times 8 dd 33561947
%define lum_from_shift 14

SECTION .text

; NOTE: there is no need to clamp the input when converting to jpeg range
;       (like we do in the C code) because packssdw will saturate the output.

;-----------------------------------------------------------------------------
; lumConvertRange
;
; void ff_lumRangeToJpeg_<opt>(int16_t *dst, int width);
; void ff_lumRangeFromJpeg_<opt>(int16_t *dst, int width);
;
;-----------------------------------------------------------------------------

%macro LUMCONVERTRANGE 4
cglobal %1, 2, 3, 3, dst, width, x
    movsxdifnidn widthq, widthd
    xor              xq, xq
    mova             m4, [%2]
    mova             m5, [%3]
.loop:
    pmovsxwd         m0, [dstq+xq*2]
    pmovsxwd         m1, [dstq+xq*2+mmsize/2]
    pmulld           m0, m4
    pmulld           m1, m4
    paddd            m0, m5
    paddd            m1, m5
    psrad            m0, %4
    psrad            m1, %4
%if mmsize == 16
    packssdw         m0, m0
    packssdw         m1, m1
    movq    [dstq+xq*2], m0
    movq    [dstq+xq*2+mmsize/2], m1
%else
    vextracti128    xm7, ym0, 1
    packssdw        xm0, xm7
    vextracti128    xm7, ym1, 1
    packssdw        xm1, xm7
    movdqu  [dstq+xq*2], xm0
    movdqu  [dstq+xq*2+mmsize/2], xm1
%endif
    add              xq, mmsize / 2
    cmp              xd, widthd
    jl .loop
    RET
%endmacro

;-----------------------------------------------------------------------------
; chrConvertRange
;
; void ff_chrRangeToJpeg_<opt>(int16_t *dstU, int16_t *dstV, int width);
; void ff_chrRangeFromJpeg_<opt>(int16_t *dstU, int16_t *dstV, int width);
;
;-----------------------------------------------------------------------------

%macro CHRCONVERTRANGE 4
cglobal %1, 3, 4, 4, dstU, dstV, width, x
    movsxdifnidn widthq, widthd
    xor              xq, xq
    mova             m4, [%2]
    mova             m5, [%3]
.loop:
    pmovsxwd         m0, [dstUq+xq*2]
    pmovsxwd         m1, [dstUq+xq*2+mmsize/2]
    pmovsxwd         m2, [dstVq+xq*2]
    pmovsxwd         m3, [dstVq+xq*2+mmsize/2]
    pmulld           m0, m4
    pmulld           m1, m4
    pmulld           m2, m4
    pmulld           m3, m4
    paddd            m0, m5
    paddd            m1, m5
    paddd            m2, m5
    paddd            m3, m5
    psrad            m0, %4
    psrad            m1, %4
    psrad            m2, %4
    psrad            m3, %4
%if mmsize == 16
    packssdw         m0, m0
    packssdw         m1, m1
    packssdw         m2, m2
    packssdw         m3, m3
    movq   [dstUq+xq*2], m0
    movq   [dstUq+xq*2+mmsize/2], m1
    movq   [dstVq+xq*2], m2
    movq   [dstVq+xq*2+mmsize/2], m3
%else
    vextracti128    xm7, ym0, 1
    packssdw        xm0, xm7
    vextracti128    xm7, ym1, 1
    packssdw        xm1, xm7
    vextracti128    xm7, ym2, 1
    packssdw        xm2, xm7
    vextracti128    xm7, ym3, 1
    packssdw        xm3, xm7
    movdqu [dstUq+xq*2], xm0
    movdqu [dstUq+xq*2+mmsize/2], xm1
    movdqu [dstVq+xq*2], xm2
    movdqu [dstVq+xq*2+mmsize/2], xm3
%endif
    add              xq, mmsize / 2
    cmp              xd, widthd
    jl .loop
    RET
%endmacro

%if ARCH_X86_64
INIT_XMM sse4
LUMCONVERTRANGE lumRangeToJpeg,   lum_to_mult,   lum_to_offset,   lum_to_shift
CHRCONVERTRANGE chrRangeToJpeg,   chr_to_mult,   chr_to_offset,   chr_to_shift
LUMCONVERTRANGE lumRangeFromJpeg, lum_from_mult, lum_from_offset, lum_from_shift
CHRCONVERTRANGE chrRangeFromJpeg, chr_from_mult, chr_from_offset, chr_from_shift

INIT_YMM avx2
LUMCONVERTRANGE lumRangeToJpeg,   lum_to_mult,   lum_to_offset,   lum_to_shift
CHRCONVERTRANGE chrRangeToJpeg,   chr_to_mult,   chr_to_offset,   chr_to_shift
LUMCONVERTRANGE lumRangeFromJpeg, lum_from_mult, lum_from_offset, lum_from_shift
CHRCONVERTRANGE chrRangeFromJpeg, chr_from_mult, chr_from_offset, chr_from_shift
%endif
