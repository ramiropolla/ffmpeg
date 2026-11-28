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

%include "ops_include.asm"

SECTION .text

;---------------------------------------------------------
; Global entry point. See `ops_include.asm` for info.

%macro process_fn 1 ; number of planes
cglobal sws_process%1_x86, 6, 7 + 2 * %1, 16, 20
            ; Args:
            ;   execq, implq, bxd, yd as defined in ops_include.asm
            ;   bx_end and y_end are initially in tmp0d / tmp1d
            ;   (see SwsOpFunc signature)
            mov [rsp +  0], implq ; [qword] impl     (restore implq after chain)
            mov [rsp +  8], bxd   ; [dword] bx_start (restore after line finish)
            mov [rsp + 12], tmp0d ; [dword] bx_end   (loop counter limit)
            mov [rsp + 16], tmp1d ; [dword] y_end    (loop counter limit)
            movsxdifnidn bxq, bxd
            movsxdifnidn yq, yd

            ; load plane pointers
            mov in0q,  [execq + SwsOpExec.in0]
IF %1 > 1,  mov in1q,  [execq + SwsOpExec.in1]
IF %1 > 2,  mov in2q,  [execq + SwsOpExec.in2]
IF %1 > 3,  mov in3q,  [execq + SwsOpExec.in3]
            mov out0q, [execq + SwsOpExec.out0]
IF %1 > 1,  mov out1q, [execq + SwsOpExec.out1]
IF %1 > 2,  mov out2q, [execq + SwsOpExec.out2]
IF %1 > 3,  mov out3q, [execq + SwsOpExec.out3]
.loop:
            call [implq] ; call into op chain
            mov implq, [rsp]
            inc bxd
            cmp bxd, [rsp + 12]
            jne .loop
            ; end of line
            inc yd
            cmp yd, [rsp + 16]
            je .end
            ; bump addresses to point to start of next line
            add in0q,  [execq + SwsOpExec.in_bump0]
IF %1 > 1,  add in1q,  [execq + SwsOpExec.in_bump1]
IF %1 > 2,  add in2q,  [execq + SwsOpExec.in_bump2]
IF %1 > 3,  add in3q,  [execq + SwsOpExec.in_bump3]
            add out0q, [execq + SwsOpExec.out_bump0]
IF %1 > 1,  add out1q, [execq + SwsOpExec.out_bump1]
IF %1 > 2,  add out2q, [execq + SwsOpExec.out_bump2]
IF %1 > 3,  add out3q, [execq + SwsOpExec.out_bump3]
            mov bxd, [rsp + 8]
            ; conditionally apply y bump (if non-NULL)
            mov tmp0q, [execq + SwsOpExec.in_bump_y]
            test tmp0q, tmp0q
            jz .loop
            movsxd tmp0q, [tmp0q + yq * 4 - 4] ; load (signed) y bump
%if %1 > 3
            mov tmp1q, tmp0q
            imul tmp1q, [execq + SwsOpExec.in_stride3]
            add in3q, tmp1q
%endif
%if %1 > 2
            mov tmp1q, tmp0q
            imul tmp1q, [execq + SwsOpExec.in_stride2]
            add in2q, tmp1q
%endif
%if %1 > 1
            mov tmp1q, tmp0q
            imul tmp1q, [execq + SwsOpExec.in_stride1]
            add in1q, tmp1q
%endif
            imul tmp0q, [execq + SwsOpExec.in_stride0]
            add in0q, tmp0q
            jmp .loop
.end:
            RET
%endmacro

process_fn 1
process_fn 2
process_fn 3
process_fn 4

;---------------------------------------------------------
; Packed shuffle fast-path

; This is a special entry point for handling a subset of operation chains
; that can be reduced down to a single `pshufb` shuffle mask. For more details
; about when this works, refer to `solve_shuffle_ext()` in ops_optimizer.c.
;
; This macro gets instantiated for every parameter combination of
; SWS_UOP_RW_SHUFFLE, which embeds the clear value, the read and write sizes
; and the number of planes.
;
; The private data holds one shuffle mask of mmsize bytes for each pair of
; output plane q and input plane p, at index q * IN_PLANES + p, followed by
; the clear values of each output plane. Each mask only selects bytes from
; its input plane and zeroes all others, and the masks of an output plane
; are combined with por. The clear values are only applied if CLEAR_VALUE
; is not zero.
;
; Since pshufb can't shuffle across lanes, we only call SSE4 versions for
; all shuffles that are not a clean multiple of 128 bits (e.g. rgb24 -> rgb0),
; unless we have access to AVX-512 vpermb (with a single input plane), or if
; the lanes are all independent, in which case we can also use `pshufb` on
; mmsize == 32/64. This is detected by the `LANE_ALIGNED` condition.

%macro READ 2 ; dst, src
    %if READ_SIZE <= 4
        movd xm %+ %1, %2
    %elif READ_SIZE <= 8
        movq xm %+ %1, %2
    %elif READ_SIZE <= 16
        movu xm %+ %1, %2
    %elif READ_SIZE <= 32
        movu ym %+ %1, %2
    %else
        movu zm %+ %1, %2
    %endif
%endmacro

%macro WRITE 2 ; dst, src
    %if WRITE_SIZE <= 4
        movd %1, xm %+ %2
    %elif WRITE_SIZE <= 8
        movq %1, xm %+ %2
    %elif WRITE_SIZE <= 16
        movu %1, xm %+ %2
    %elif WRITE_SIZE <= 32
        movu %1, ym %+ %2
    %else
        movu %1, zm %+ %2
    %endif
%endmacro

; Defines SH_M as the operand of mask %1: a register or a memory reference
%macro SHUFFLE_MASK 1
    %if MASK_REGS
        %assign SH_MR MASK_REG + %1
        %xdefine SH_M m %+ SH_MR
    %else
        %xdefine SH_M [shuffleq + (%1) * mmsize]
    %endif
%endmacro

%macro RW_SHUFFLE 5
%assign CLEAR_VALUE %1
%assign READ_SIZE   %2
%assign WRITE_SIZE  %3
%assign IN_PLANES   (%4 ? %4 : 1)
%assign OUT_PLANES  (%5 ? %5 : 1)

%assign LANE_ALIGNED (READ_SIZE == WRITE_SIZE && 16 % READ_SIZE == 0)
%assign MAX_SIZE     (READ_SIZE > WRITE_SIZE ? READ_SIZE : WRITE_SIZE)
%assign VPERMB       !(LANE_ALIGNED || mmsize == 16)

; Expand read/write sizes to the true number of groups, this matches
; logic on the C side in `translate_shuffle`
%assign GROUPS       (mmsize / MAX_SIZE)
%assign READ_SIZE    (READ_SIZE * GROUPS)
%assign WRITE_SIZE   (WRITE_SIZE * GROUPS)

; Vector registers: inputs in m0-m3, the output in m4, a temporary in m5,
; and the masks in m6-m15. SH_I and SH_O iterate over the input and output
; planes, and SH_R and SH_K hold register numbers. The pshufb masks and the
; clear values are kept in registers if they all fit.
%assign IN_REG   0
%assign OUT_REG  4
%assign TMP_REG  5
%assign MASK_REG 6
%assign NUM_MASKS (IN_PLANES * OUT_PLANES + (CLEAR_VALUE ? OUT_PLANES : 0))
%assign MASK_REGS (!VPERMB && NUM_MASKS <= 10)

cglobal NAME, 6, 14, 16, exec, shuffle, bx, row, bxend, rowend, src0, src1, src2, src3, dst0, dst1, dst2, dst3
%if VPERMB && (!cpuflag(avx512icl) || IN_PLANES > 1)
            ud2 ; runtime checks should prevent this variant from being called
%else
    %assign SH_I 0
    %rep IN_PLANES
            mov src %+ SH_I %+ q, [execq + SwsOpExec.in0 + 8 * SH_I]
        %assign SH_I SH_I + 1
    %endrep
    %assign SH_O 0
    %rep OUT_PLANES
            mov dst %+ SH_O %+ q, [execq + SwsOpExec.out0 + 8 * SH_O]
        %assign SH_O SH_O + 1
    %endrep

    %if VPERMB
            ; setup the masks, and zero the cleared bytes in vpermb
        %assign SH_O 0
        %rep OUT_PLANES
            %assign SH_R MASK_REG + SH_O
            %assign SH_K SH_O + 1
            mova m %+ SH_R, [shuffleq + SH_O * mmsize]
            vpmovb2m k %+ SH_K, m %+ SH_R
            knotq k %+ SH_K, k %+ SH_K
            %assign SH_O SH_O + 1
        %endrep
    %elif MASK_REGS
        %assign SH_K 0
        %rep NUM_MASKS
            %assign SH_R MASK_REG + SH_K
            mova m %+ SH_R, [shuffleq + SH_K * mmsize]
            %assign SH_K SH_K + 1
        %endrep
    %endif

            ; setup loop bounds and variables
            sub bxendd, bxd
            sub rowendd, rowd
            ; reuse now-unneeded regs
            %define srcidxq bxq
            imul srcidxq, bxendq, -READ_SIZE
    %if READ_SIZE == WRITE_SIZE
            %define dstidxq srcidxq
    %else
            %define dstidxq rowq
            imul dstidxq, bxendq, -WRITE_SIZE
    %endif
    %assign SH_I 0
    %rep IN_PLANES
            sub src %+ SH_I %+ q, srcidxq
        %assign SH_I SH_I + 1
    %endrep
    %assign SH_O 0
    %rep OUT_PLANES
            sub dst %+ SH_O %+ q, dstidxq
        %assign SH_O SH_O + 1
    %endrep

.loop:
    %assign SH_I 0
    %rep IN_PLANES
        %assign SH_R IN_REG + SH_I
            READ SH_R, [src %+ SH_I %+ q + srcidxq]
        %assign SH_I SH_I + 1
    %endrep
    %assign SH_O 0
    %rep OUT_PLANES
        %assign SH_A OUT_REG ; accumulator register of this output plane
        %if VPERMB
            %assign SH_R MASK_REG + SH_O
            %assign SH_K SH_O + 1
            vpermb m %+ OUT_REG {k %+ SH_K}{z}, m %+ SH_R, m %+ IN_REG
        %else
            %assign SH_I 0
            %rep IN_PLANES
                %assign SH_R IN_REG + SH_I
                SHUFFLE_MASK (SH_O * IN_PLANES + SH_I)
                %if SH_O == OUT_PLANES - 1 ; last use of the input, shuffle in place
            pshufb m %+ SH_R, SH_M
                    %if SH_I == 0
                        %assign SH_A SH_R
                    %else
            por m %+ SH_A, m %+ SH_R
                    %endif
                %elif SH_I == 0
            pshufb m %+ OUT_REG, m %+ SH_R, SH_M
                %else
            pshufb m %+ TMP_REG, m %+ SH_R, SH_M
            por m %+ OUT_REG, m %+ TMP_REG
                %endif
                %assign SH_I SH_I + 1
            %endrep
        %endif
        %if CLEAR_VALUE != 0 ; the cleared bytes are zero after the shuffle
            SHUFFLE_MASK (OUT_PLANES * IN_PLANES + SH_O)
            por m %+ SH_A, SH_M
        %endif
            WRITE [dst %+ SH_O %+ q + dstidxq], SH_A
        %assign SH_O SH_O + 1
    %endrep

            add srcidxq, READ_SIZE
    %if READ_SIZE != WRITE_SIZE
            add dstidxq, WRITE_SIZE
    %endif
            jnz .loop
    %assign SH_I 0
    %rep IN_PLANES
            add src %+ SH_I %+ q, [execq + SwsOpExec.in_stride0 + 8 * SH_I]
        %assign SH_I SH_I + 1
    %endrep
    %assign SH_O 0
    %rep OUT_PLANES
            add dst %+ SH_O %+ q, [execq + SwsOpExec.out_stride0 + 8 * SH_O]
        %assign SH_O SH_O + 1
    %endrep
            imul srcidxq, bxendq, -READ_SIZE
    %if READ_SIZE != WRITE_SIZE
            imul dstidxq, bxendq, -WRITE_SIZE
    %endif
            dec rowendd
            jnz .loop
            RET
%endif
%endmacro

%ifdef SWS_UOPS_MACROS

INIT_XMM sse4
DECL_U8_RW_SHUFFLE RW_SHUFFLE

INIT_YMM avx2
DECL_U8_RW_SHUFFLE RW_SHUFFLE

INIT_ZMM avx512
DECL_U8_RW_SHUFFLE RW_SHUFFLE

INIT_ZMM avx512icl
DECL_U8_RW_SHUFFLE RW_SHUFFLE

%endif
