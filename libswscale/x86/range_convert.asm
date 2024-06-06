;******************************************************************************
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

chr_to_mult:        times 4 dd 4663
chr_to_offset:      times 4 dd -9289992
%define chr_to_shift 12

chr_from_mult:      times 4 dd 1799
chr_from_offset:    times 4 dd 4081085
%define chr_from_shift 11

lum_to_mult:        times 4 dd 19077
lum_to_offset:      times 4 dd -39057361
%define lum_to_shift 14

lum_from_mult:      times 4 dd 14071
lum_from_offset:    times 4 dd 33561947
%define lum_from_shift 14

SECTION .text

; NOTE: there is no need to clamp the input when converting to Jpeg range
;       because packssdw will saturate the output.

;-----------------------------------------------------------------------------
; lumConvertRange
;
; void ff_lumConvertRange_<opt>(int16_t *dst, int width);
;
;-----------------------------------------------------------------------------

%macro LUMCONVERTRANGE 4
cglobal %1, 2, 3, 3, dst, width, x
    movsxdifnidn widthq, widthd
    xor              xq, xq
    mova             m1, [%2]
    mova             m2, [%3]
.loop:
    pmovsxwd         m0, [dstq+xq*2]
    pmulld           m0, m1
    paddd            m0, m2
    psrad            m0, %4
    packssdw         m0, m0
    movh    [dstq+xq*2], m0
    add              xq, mmsize / 4
    cmp              xd, widthd
    jl .loop
    RET
%endmacro

;-----------------------------------------------------------------------------
; chrConvertRange
;
; void ff_chrConvertRange_<opt>(int16_t *dstU, int16_t *dstV, int width);
;
;-----------------------------------------------------------------------------

%macro CHRCONVERTRANGE 4
cglobal %1, 3, 4, 4, dstU, dstV, width, x
    movsxdifnidn widthq, widthd
    xor              xq, xq
    mova             m1, [%2]
    mova             m2, [%3]
.loop:
    pmovsxwd         m0, [dstUq+xq*2]
    pmulld           m0, m1
    paddd            m0, m2
    psrad            m0, %4
    packssdw         m0, m0
    movh   [dstUq+xq*2], m0
    pmovsxwd         m0, [dstVq+xq*2]
    pmulld           m0, m1
    paddd            m0, m2
    psrad            m0, %4
    packssdw         m0, m0
    movh   [dstVq+xq*2], m0
    add              xq, mmsize / 4
    cmp              xd, widthd
    jl .loop
    RET
%endmacro

%if ARCH_X86_64
INIT_XMM sse4
LUMCONVERTRANGE  lumRangeToJpeg,   lum_to_mult,   lum_to_offset,   lum_to_shift
CHRCONVERTRANGE  chrRangeToJpeg,   chr_to_mult,   chr_to_offset,   chr_to_shift
LUMCONVERTRANGE  lumRangeFromJpeg, lum_from_mult, lum_from_offset, lum_from_shift
CHRCONVERTRANGE  chrRangeFromJpeg, chr_from_mult, chr_from_offset, chr_from_shift
%endif
