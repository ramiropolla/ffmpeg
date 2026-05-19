%include "ops_common.asm"

SECTION .text

extern fwrite

%macro debug_kernel 0
cglobal sws_debug, 0, 0, 0

    ; save caller-saved registers to stack
    sub  rsp, 8 * mmsize + 48
    movu [rsp + 0 * mmsize +  0], m0
    movu [rsp + 1 * mmsize +  0], m4
    movu [rsp + 2 * mmsize +  0], m1
    movu [rsp + 3 * mmsize +  0], m5
    movu [rsp + 4 * mmsize +  0], m2
    movu [rsp + 5 * mmsize +  0], m6
    movu [rsp + 6 * mmsize +  0], m3
    movu [rsp + 7 * mmsize +  0], m7
    mov  [rsp + 8 * mmsize +  0], execq
    mov  [rsp + 8 * mmsize +  8], implq
    mov  [rsp + 8 * mmsize + 16], bxq
    mov  [rsp + 8 * mmsize + 24], yq
    mov  [rsp + 8 * mmsize + 32], out0q
    mov  [rsp + 8 * mmsize + 40], in0q

    ; fwrite(rsp, 8 * mmsize, 1, impl->priv.ptr);
    mov rcx, [implq + SwsOpImpl.priv + 0]
    mov rdx, 1
    mov rsi, 8 * mmsize
    mov rdi, rsp
    call fwrite wrt ..plt

    ; restore caller-saved registers from stack
    movu m0,    [rsp + 0 * mmsize +  0]
    movu m4,    [rsp + 1 * mmsize +  0]
    movu m1,    [rsp + 2 * mmsize +  0]
    movu m5,    [rsp + 3 * mmsize +  0]
    movu m2,    [rsp + 4 * mmsize +  0]
    movu m6,    [rsp + 5 * mmsize +  0]
    movu m3,    [rsp + 6 * mmsize +  0]
    movu m7,    [rsp + 7 * mmsize +  0]
    mov  execq, [rsp + 8 * mmsize +  0]
    mov  implq, [rsp + 8 * mmsize +  8]
    mov  bxq,   [rsp + 8 * mmsize + 16]
    mov  yq,    [rsp + 8 * mmsize + 24]
    mov  out0q, [rsp + 8 * mmsize + 32]
    mov  in0q,  [rsp + 8 * mmsize + 40]
    add  rsp, 8 * mmsize + 48

    CONTINUE
%endmacro

INIT_YMM avx2
debug_kernel
