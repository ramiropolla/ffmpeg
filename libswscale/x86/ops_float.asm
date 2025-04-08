;******************************************************************************
;* Copyright (c) 2025 Niklas Haas
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

%include "ops_common.asm"

SECTION .text

;---------------------------------------------------------
; Pixel type conversions

%macro conv8to32f 0
op convert_U8_F32
        LOAD_CONT r2
IF X,   vpsrldq xmx2, xmx, 8
IF Y,   vpsrldq xmy2, xmy, 8
IF Z,   vpsrldq xmz2, xmz, 8
IF W,   vpsrldq xmw2, xmw, 8
IF X,   pmovzxbd mx, xmx
IF Y,   pmovzxbd my, xmy
IF Z,   pmovzxbd mz, xmz
IF W,   pmovzxbd mw, xmw
IF X,   pmovzxbd mx2, xmx2
IF Y,   pmovzxbd my2, xmy2
IF Z,   pmovzxbd mz2, xmz2
IF W,   pmovzxbd mw2, xmw2
IF X,   vcvtdq2ps mx, mx
IF Y,   vcvtdq2ps my, my
IF Z,   vcvtdq2ps mz, mz
IF W,   vcvtdq2ps mw, mw
IF X,   vcvtdq2ps mx2, mx2
IF Y,   vcvtdq2ps my2, my2
IF Z,   vcvtdq2ps mz2, mz2
IF W,   vcvtdq2ps mw2, mw2
        CONTINUE r2
%endmacro

%macro conv16to32f 0
op convert_U16_F32
        LOAD_CONT r2
IF X,   vextracti128 xmx2, mx, 1
IF Y,   vextracti128 xmy2, my, 1
IF Z,   vextracti128 xmz2, mz, 1
IF W,   vextracti128 xmw2, mw, 1
IF X,   pmovzxwd mx, xmx
IF Y,   pmovzxwd my, xmy
IF Z,   pmovzxwd mz, xmz
IF W,   pmovzxwd mw, xmw
IF X,   pmovzxwd mx2, xmx2
IF Y,   pmovzxwd my2, xmy2
IF Z,   pmovzxwd mz2, xmz2
IF W,   pmovzxwd mw2, xmw2
IF X,   vcvtdq2ps mx, mx
IF Y,   vcvtdq2ps my, my
IF Z,   vcvtdq2ps mz, mz
IF W,   vcvtdq2ps mw, mw
IF X,   vcvtdq2ps mx2, mx2
IF Y,   vcvtdq2ps my2, my2
IF Z,   vcvtdq2ps mz2, mz2
IF W,   vcvtdq2ps mw2, mw2
        CONTINUE r2
%endmacro

%macro conv32fto8 0
op convert_F32_U8
        LOAD_CONT r2
IF X,   cvttps2dq mx, mx
IF Y,   cvttps2dq my, my
IF Z,   cvttps2dq mz, mz
IF W,   cvttps2dq mw, mw
IF X,   cvttps2dq mx2, mx2
IF Y,   cvttps2dq my2, my2
IF Z,   cvttps2dq mz2, mz2
IF W,   cvttps2dq mw2, mw2
IF X,   packusdw mx, mx2
IF Y,   packusdw my, my2
IF Z,   packusdw mz, mz2
IF W,   packusdw mw, mw2
IF X,   vextracti128 xmx2, mx, 1
IF Y,   vextracti128 xmy2, my, 1
IF Z,   vextracti128 xmz2, mz, 1
IF W,   vextracti128 xmw2, mw, 1
IF X,   packuswb xmx, xmx2
IF Y,   packuswb xmy, xmy2
IF Z,   packuswb xmz, xmz2
IF W,   packuswb xmw, xmw2
IF X,   vpshufd xmx, xmx, q3120
IF Y,   vpshufd xmy, xmy, q3120
IF Z,   vpshufd xmz, xmz, q3120
IF W,   vpshufd xmw, xmw, q3120
        CONTINUE r2
%endmacro

%macro conv32fto16 0
op convert_F32_U16
        LOAD_CONT r2
IF X,   cvttps2dq mx, mx
IF Y,   cvttps2dq my, my
IF Z,   cvttps2dq mz, mz
IF W,   cvttps2dq mw, mw
IF X,   cvttps2dq mx2, mx2
IF Y,   cvttps2dq my2, my2
IF Z,   cvttps2dq mz2, mz2
IF W,   cvttps2dq mw2, mw2
IF X,   packusdw mx, mx2
IF Y,   packusdw my, my2
IF Z,   packusdw mz, mz2
IF W,   packusdw mw, mw2
IF X,   vpermq mx, mx, q3120
IF Y,   vpermq my, my, q3120
IF Z,   vpermq mz, mz, q3120
IF W,   vpermq mw, mw, q3120
        CONTINUE r2
%endmacro

%macro min_max 0
op min
IF X,   vbroadcastss m8,  [implq + SwsOpImpl.priv + 0]
IF Y,   vbroadcastss m9,  [implq + SwsOpImpl.priv + 4]
IF Z,   vbroadcastss m10, [implq + SwsOpImpl.priv + 8]
IF W,   vbroadcastss m11, [implq + SwsOpImpl.priv + 12]
        LOAD_CONT r2
IF X,   minps mx, mx, m8
IF Y,   minps my, my, m9
IF Z,   minps mz, mz, m10
IF W,   minps mw, mw, m11
IF X,   minps mx2, m8
IF Y,   minps my2, m9
IF Z,   minps mz2, m10
IF W,   minps mw2, m11
        CONTINUE r2

op max
IF X,   vbroadcastss m8,  [implq + SwsOpImpl.priv + 0]
IF Y,   vbroadcastss m9,  [implq + SwsOpImpl.priv + 4]
IF Z,   vbroadcastss m10, [implq + SwsOpImpl.priv + 8]
IF W,   vbroadcastss m11, [implq + SwsOpImpl.priv + 12]
        LOAD_CONT r2
IF X,   maxps mx, m8
IF Y,   maxps my, m9
IF Z,   maxps mz, m10
IF W,   maxps mw, m11
IF X,   maxps mx2, m8
IF Y,   maxps my2, m9
IF Z,   maxps mz2, m10
IF W,   maxps mw2, m11
        CONTINUE r2
%endmacro

INIT_YMM avx2
decl_common_patterns conv8to32f
decl_common_patterns conv16to32f
decl_common_patterns conv32fto8
decl_common_patterns conv32fto16
decl_common_patterns min_max
