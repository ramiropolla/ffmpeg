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
; and the number of planes. The layout described here must match the logic
; on the C side in `translate_shuffle()`.
;
; Each 128-bit lane processes GROUPS iterations of the shuffle, which write
; OUT_REGS full registers of 16 bytes per output plane. Output register j
; covers bytes [16 * j, 16 * j + 16) of the lane, and is shuffled from the
; registers that are loaded from each input plane at 16-byte steps from the
; offset given by IN_RANGE, which cover all input bytes of the iterations it
; writes to. The stores never overlap, which avoids stalls on the store
; buffer, but the loads may. With a single output register per lane, there
; are as many lanes as fit in a vector register, and lane l processes the
; l-th chunk of LANE_IN input and LANE_OUT output bytes of every plane (so
; the stores are contiguous). Interleaving the stores of several output
; registers of two lanes stalls on the store buffer of Golden Cove.
;
; Shuffles with too many output registers per lane instead process a single
; lane of GROUPS = 16 / max(read size, write size) iterations, with one load
; and one store per plane, like the vpermb variant does on a full register.
;
; The private data holds one shuffle mask of mmsize bytes for each output
; plane q, output register j, input plane p and input register k, at index
; ((q * OUT_REGS + j) * IN_PLANES + p) * MAX_IN_REGS + k, followed by the
; clear values of each output register, at index NUM_SHUF + q * OUT_REGS + j.
; Each mask only selects bytes from its input register and zeroes all others,
; and the shuffled registers of an output register are combined with por.
; The clear values are only applied if CLEAR_VALUE is not zero.
;
; Since pshufb can't shuffle across lanes, shuffles that are not a clean
; multiple of 128 bits (e.g. rgb24 -> rgb0) use at most two lanes, unless we
; have access to AVX-512 vpermb (with a single input plane). This is detected
; by the `LANE_ALIGNED` condition.

%macro READ 3 ; dst, src, size
    %if %3 <= 4
        movd xm %+ %1, %2
    %elif %3 <= 8
        movq xm %+ %1, %2
    %elif %3 <= 16
        movu xm %+ %1, %2
    %elif %3 <= 32
        movu ym %+ %1, %2
    %else
        movu zm %+ %1, %2
    %endif
%endmacro

%macro WRITE 3 ; dst, src, size
    %if %3 <= 4
        movd %1, xm %+ %2
    %elif %3 <= 8
        movq %1, xm %+ %2
    %elif %3 <= 16
        movu %1, xm %+ %2
    %elif %3 <= 32
        movu %1, ym %+ %2
    %else
        movu %1, zm %+ %2
    %endif
%endmacro

%macro LOAD 2 ; dst, src address
    %if LANES == 1
        READ %1, [%2], LOAD_SIZE
    %elif LANE_IN == 16
        movu m %+ %1, [%2]
    %else
        movu xm %+ %1, [%2]
        vinserti128 m %+ %1, m %+ %1, [%2 + LANE_IN], 1
    %endif
%endmacro

%macro STORE 2 ; dst address, src
    %if LANES == 1
        WRITE [%1], %2, STORE_SIZE
    %else
        movu [%1], m %+ %2
    %endif
%endmacro

; Defines SH_M as the operand of mask %1: a register or a memory reference
%macro SHUFFLE_MASK 1
    %if MASK_REGS
        %assign SH_MR MASK_REG + %1
        %xdefine SH_M VR %+ SH_MR
    %else
        %xdefine SH_M [shuffleq + (%1) * mmsize]
    %endif
%endmacro

; Shuffles input register %1 with mask %2 into the accumulator SH_A, in
; place if %3 (the last use of the input). SH_F is 1 for the first input
; of an output register.
%macro SHUFFLE_IN 3 ; input register, mask index, last use
    %assign SH_R IN_REG + %1
    SHUFFLE_MASK %2
    %if %3
            pshufb VR %+ SH_R, SH_M
        %if SH_F
            %assign SH_A SH_R
        %else
            por VR %+ SH_A, VR %+ SH_R
        %endif
    %elif SH_F
            pshufb VR %+ OUT_REG, VR %+ SH_R, SH_M
        %assign SH_A OUT_REG
    %else
            pshufb VR %+ TMP_REG, VR %+ SH_R, SH_M
            por VR %+ SH_A, VR %+ TMP_REG
    %endif
    %assign SH_F 0
%endmacro

; Sets SH_OFF and SH_NK to the input offset and number of input registers of
; output register %1
%macro IN_RANGE 1
    %if SINGLE
        %assign SH_OFF 0
        %assign SH_NK  1
    %else
        %assign SH_N0  ((16 * (%1)) / W_SIZE)
        %assign SH_N1  ((16 * (%1) + 15) / W_SIZE)
        %if SH_N1 > GROUPS - 1
            %assign SH_N1 GROUPS - 1
        %endif
        %assign SH_OFF (SH_N0 * R_SIZE)
        %assign SH_NK  (((SH_N1 - SH_N0 + 1) * R_SIZE + 15) / 16)
    %endif
%endmacro

%macro RW_SHUFFLE 5
%assign CLEAR_VALUE %1
%assign R_SIZE      %2
%assign W_SIZE      %3
%assign IN_PLANES   (%4 ? %4 : 1)
%assign OUT_PLANES  (%5 ? %5 : 1)

%assign LANE_ALIGNED (R_SIZE == W_SIZE && 16 % R_SIZE == 0)
%assign VPERMB       (mmsize == 64 && !LANE_ALIGNED)
%assign MAX_SIZE     (R_SIZE > W_SIZE ? R_SIZE : W_SIZE)

; GROUPS = 16 / gcd(16, W_SIZE) fill whole output registers
%assign SH_A 16
%assign SH_B W_SIZE
%rep 8
    %if SH_B
        %assign SH_T SH_A % SH_B
        %assign SH_A SH_B
        %assign SH_B SH_T
    %endif
%endrep
%assign GROUPS    (16 / SH_A)
%assign OUT_REGS  (GROUPS * W_SIZE / 16)
%assign SINGLE    (VPERMB || OUT_REGS > 8)
%if SINGLE
    %assign LANE_SIZE (VPERMB ? mmsize : 16)
    %assign GROUPS    (LANE_SIZE / MAX_SIZE)
    %assign OUT_REGS  1
    %assign LANES     1
    %assign LOAD_SIZE  (GROUPS * R_SIZE)
    %assign STORE_SIZE (GROUPS * W_SIZE)
%else
    %assign LANES     (OUT_REGS == 1 ? mmsize / 16 : 1)
    %assign LOAD_SIZE  16
    %assign STORE_SIZE 16
%endif
%assign LANE_IN   (GROUPS * R_SIZE)
%assign LANE_OUT  (GROUPS * W_SIZE)
; vector register prefix: with a single 128-bit lane, only use xmm registers
%if LANES == 1 && !VPERMB
    %xdefine VR xm
%else
    %xdefine VR m
%endif
%assign STEP_IN   (LANES * LANE_IN)
%assign STEP_OUT  (LANES * LANE_OUT)

%assign MAX_IN_REGS 1
%assign SH_J 0
%rep OUT_REGS
    IN_RANGE SH_J
    %if SH_NK > MAX_IN_REGS
        %assign MAX_IN_REGS SH_NK
    %endif
    %assign SH_J SH_J + 1
%endrep

; Vector registers: the inputs from m0, then the output, a temporary and the
; masks. SH_I, SH_O, SH_J and SH_U iterate over the input planes, output
; planes, output registers and input registers, and SH_R and SH_K hold
; register numbers. The pshufb masks and the clear values are kept in
; registers if they all fit.
%assign NUM_IN    (IN_PLANES * MAX_IN_REGS)
%assign IN_REG    0
%assign OUT_REG   NUM_IN
%assign TMP_REG   (NUM_IN + 1)
%assign MASK_REG  (NUM_IN + 2)
%assign NUM_SHUF  (OUT_PLANES * OUT_REGS * IN_PLANES * MAX_IN_REGS)
%assign NUM_MASKS (NUM_SHUF + (CLEAR_VALUE ? OUT_PLANES * OUT_REGS : 0))
%assign MASK_REGS (!VPERMB && MASK_REG + NUM_MASKS <= 16)
%if MASK_REG > 16
    %error too many input registers
%endif

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
            mova VR %+ SH_R, [shuffleq + SH_K * mmsize]
            %assign SH_K SH_K + 1
        %endrep
    %endif

            ; setup loop bounds and variables
            sub bxendd, bxd
            sub rowendd, rowd
            ; reuse now-unneeded regs
            %define srcidxq bxq
            imul srcidxq, bxendq, -STEP_IN
    %if STEP_IN == STEP_OUT
            %define dstidxq srcidxq
    %else
            %define dstidxq rowq
            imul dstidxq, bxendq, -STEP_OUT
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
    %assign SH_J 0
    %rep OUT_REGS
        IN_RANGE SH_J
        %assign SH_I 0
        %rep IN_PLANES
            %assign SH_U 0
            %rep SH_NK
                %assign SH_R IN_REG + SH_I * MAX_IN_REGS + SH_U
            LOAD SH_R, src %+ SH_I %+ q + srcidxq + SH_OFF + 16 * SH_U
                %assign SH_U SH_U + 1
            %endrep
            %assign SH_I SH_I + 1
        %endrep
        %assign SH_O 0
        %rep OUT_PLANES
            %assign SH_A OUT_REG ; accumulator register of this output register
            %assign SH_F 1
            %if VPERMB
                %assign SH_R MASK_REG + SH_O
                %assign SH_K SH_O + 1
            vpermb m %+ OUT_REG {k %+ SH_K}{z}, m %+ SH_R, m %+ IN_REG
            %else
                %assign SH_I 0
                %rep IN_PLANES
                    %assign SH_U 0
                    %rep SH_NK
                        SHUFFLE_IN (SH_I * MAX_IN_REGS + SH_U), ((((SH_O * OUT_REGS + SH_J) * IN_PLANES + SH_I) * MAX_IN_REGS) + SH_U), (SH_O == OUT_PLANES - 1)
                        %assign SH_U SH_U + 1
                    %endrep
                    %assign SH_I SH_I + 1
                %endrep
            %endif
            %if CLEAR_VALUE != 0 ; the cleared bytes are zero after the shuffle
                %if VPERMB
            por m %+ SH_A, [shuffleq + (OUT_PLANES + SH_O) * mmsize]
                %else
                SHUFFLE_MASK (NUM_SHUF + SH_O * OUT_REGS + SH_J)
            por VR %+ SH_A, SH_M
                %endif
            %endif
            STORE dst %+ SH_O %+ q + dstidxq + 16 * SH_J, SH_A
            %assign SH_O SH_O + 1
        %endrep
        %assign SH_J SH_J + 1
    %endrep

            add srcidxq, STEP_IN
    %if STEP_IN != STEP_OUT
            add dstidxq, STEP_OUT
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
            imul srcidxq, bxendq, -STEP_IN
    %if STEP_IN != STEP_OUT
            imul dstidxq, bxendq, -STEP_OUT
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
