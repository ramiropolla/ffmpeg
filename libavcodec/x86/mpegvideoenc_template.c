/*
 * MPEG video MMX templates
 *
 * Copyright (c) 2002 Michael Niedermayer <michaelni@gmx.at>
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

#include <stdint.h>

#include "libavutil/internal.h"
#include "libavutil/mem_internal.h"
#include "libavutil/x86/asm.h"
#include "libavcodec/mpegutils.h"
#include "libavcodec/mpegvideo.h"
#include "fdct.h"

#undef MMREG_WIDTH
#undef MM
#undef MOVQ
#undef SPREADW
#undef PMAXW
#undef PMAX
#undef SAVE_SIGN
#undef RESTORE_SIGN

#if COMPILE_TEMPLATE_SSE2
#define MMREG_WIDTH "16"
#define MM "%%xmm"
#define MOVQ "movdqa"
#define SPREADW(a) \
            "pshuflw $0, "a", "a"       \n\t"\
            "punpcklwd "a", "a"         \n\t"
#define PMAXW(a,b) "pmaxsw "a", "b"     \n\t"
#define PMAX(a,b) \
            "movhlps "a", "b"           \n\t"\
            PMAXW(b, a)\
            "pshuflw $0x0E, "a", "b"    \n\t"\
            PMAXW(b, a)\
            "pshuflw $0x01, "a", "b"    \n\t"\
            PMAXW(b, a)
#else
#define MMREG_WIDTH "8"
#define MM "%%mm"
#define MOVQ "movq"
#define SPREADW(a) \
            "punpcklwd "a", "a"         \n\t"\
            "punpcklwd "a", "a"         \n\t"
#define PMAXW(a,b) \
            "psubusw "a", "b"           \n\t"\
            "paddw "a", "b"             \n\t"
#define PMAX(a,b)  \
            "movq "a", "b"              \n\t"\
            "psrlq $32, "a"             \n\t"\
            PMAXW(b, a)\
            "movq "a", "b"              \n\t"\
            "psrlq $16, "a"             \n\t"\
            PMAXW(b, a)

#endif

#if COMPILE_TEMPLATE_SSSE3
#define SAVE_SIGN(a,b) \
            "movdqa "b", "a"            \n\t"\
            "pabsw  "b", "b"            \n\t"
#define RESTORE_SIGN(a,b) \
            "psignw "a", "b"            \n\t"
#else
#define SAVE_SIGN(a,b) \
            "pxor "a", "a"              \n\t"\
            "pcmpgtw "b", "a"           \n\t" /* block[i] <= 0 ? 0xFF : 0x00 */\
            "pxor "a", "b"              \n\t"\
            "psubw "a", "b"             \n\t" /* ABS(block[i]) */
#define RESTORE_SIGN(a,b) \
            "pxor "a", "b"              \n\t"\
            "psubw "a", "b"             \n\t" // out=((ABS(block[i])*qmat[0] - bias[0]*qmat[0])>>16)*sign(block[i])
#endif

static int RENAME(dct_quantize)(MpegEncContext *s,
                            int16_t *block, int n,
                            int qscale, int *overflow)
{
    x86_reg last_non_zero_p1;
    int level=0, q; //=0 is because gcc says uninitialized ...
    const uint16_t *qmat, *bias;
    LOCAL_ALIGNED_16(int16_t, temp_block, [64]);
    const uint8_t *scantable;
    const uint16_t *inv_scantable_p1;

    av_assert2((7&(uintptr_t)(&temp_block[0])) == 0); //did gcc align it correctly?

    //s->fdct (block);
    RENAME_FDCT(ff_fdct)(block); // cannot be anything else ...

    if(s->dct_error_sum)
        s->denoise_dct(s, block);

    if (s->mb_intra) {
        int dummy;
        scantable        = s->intra_scantable.scantable;
        inv_scantable_p1 = s->intra_scantable.inv_scantable_p1;
        if (n < 4){
            q = s->y_dc_scale;
            bias = s->q_intra_matrix16[qscale][1];
            qmat = s->q_intra_matrix16[qscale][0];
        }else{
            q = s->c_dc_scale;
            bias = s->q_chroma_intra_matrix16[qscale][1];
            qmat = s->q_chroma_intra_matrix16[qscale][0];
        }
        /* note: block[0] is assumed to be positive */
        if (!s->h263_aic) {
        __asm__ volatile (
                "mul %%ecx                \n\t"
                : "=d" (level), "=a"(dummy)
                : "a" ((block[0]>>2) + q), "c" (ff_inverse[q<<1])
        );
        } else
            /* For AIC we skip quant/dequant of INTRADC */
            level = (block[0] + 4)>>3;

        block[0]=0; //avoid fake overflow
//        temp_block[0] = (block[0] + (q >> 1)) / q;
        last_non_zero_p1 = 1;
    } else {
        scantable        = s->inter_scantable.scantable;
        inv_scantable_p1 = s->inter_scantable.inv_scantable_p1;
        last_non_zero_p1 = 0;
        bias = s->q_inter_matrix16[qscale][1];
        qmat = s->q_inter_matrix16[qscale][0];
    }

    if((s->out_format == FMT_H263 || s->out_format == FMT_H261) && s->mpeg_quant==0){

        __asm__ volatile(
            "movd %%"FF_REG_a", "MM"3           \n\t" // last_non_zero_p1
            SPREADW(MM"3")
            "pxor "MM"7, "MM"7                  \n\t" // 0
            "pxor "MM"4, "MM"4                  \n\t" // 0
            MOVQ" (%2), "MM"5                   \n\t" // qmat[0]
            "pxor "MM"6, "MM"6                  \n\t"
            "psubw (%3), "MM"6                  \n\t" // -bias[0]
            "mov $-128, %%"FF_REG_a"            \n\t"
            ".p2align 4                         \n\t"
            "1:                                 \n\t"
            MOVQ" (%1, %%"FF_REG_a"), "MM"0     \n\t" // block[i]
            SAVE_SIGN(MM"1", MM"0")                   // ABS(block[i])
            "psubusw "MM"6, "MM"0               \n\t" // ABS(block[i]) + bias[0]
            "pmulhw "MM"5, "MM"0                \n\t" // (ABS(block[i])*qmat[0] - bias[0]*qmat[0])>>16
            "por "MM"0, "MM"4                   \n\t"
            RESTORE_SIGN(MM"1", MM"0")                // out=((ABS(block[i])*qmat[0] - bias[0]*qmat[0])>>16)*sign(block[i])
            MOVQ" "MM"0, (%5, %%"FF_REG_a")     \n\t"
            "pcmpeqw "MM"7, "MM"0               \n\t" // out==0 ? 0xFF : 0x00
            MOVQ" (%4, %%"FF_REG_a"), "MM"1     \n\t"
            MOVQ" "MM"7, (%1, %%"FF_REG_a")     \n\t" // 0
            "pandn "MM"1, "MM"0                 \n\t"
            PMAXW(MM"0", MM"3")
            "add $"MMREG_WIDTH", %%"FF_REG_a"   \n\t"
            " js 1b                             \n\t"
            PMAX(MM"3", MM"0")
            "movd "MM"3, %%"FF_REG_a"           \n\t"
            "movzbl %%al, %%eax                 \n\t" // last_non_zero_p1
            : "+a" (last_non_zero_p1)
            : "r" (block+64), "r" (qmat), "r" (bias),
              "r" (inv_scantable_p1 + 64), "r" (temp_block + 64)
              XMM_CLOBBERS_ONLY("%xmm0", "%xmm1", "%xmm2", "%xmm3",
                                "%xmm4", "%xmm5", "%xmm6", "%xmm7")
        );
    }else{ // FMT_H263
        __asm__ volatile(
            "movd %%"FF_REG_a", "MM"3           \n\t" // last_non_zero_p1
            SPREADW(MM"3")
            "pxor "MM"7, "MM"7                  \n\t" // 0
            "pxor "MM"4, "MM"4                  \n\t" // 0
            "mov $-128, %%"FF_REG_a"            \n\t"
            ".p2align 4                         \n\t"
            "1:                                 \n\t"
            MOVQ" (%1, %%"FF_REG_a"), "MM"0     \n\t" // block[i]
            SAVE_SIGN(MM"1", MM"0")                   // ABS(block[i])
            MOVQ" (%3, %%"FF_REG_a"), "MM"6     \n\t" // bias[0]
            "paddusw "MM"6, "MM"0               \n\t" // ABS(block[i]) + bias[0]
            MOVQ" (%2, %%"FF_REG_a"), "MM"5     \n\t" // qmat[i]
            "pmulhw "MM"5, "MM"0                \n\t" // (ABS(block[i])*qmat[0] + bias[0]*qmat[0])>>16
            "por "MM"0, "MM"4                   \n\t"
            RESTORE_SIGN(MM"1", MM"0")                // out=((ABS(block[i])*qmat[0] - bias[0]*qmat[0])>>16)*sign(block[i])
            MOVQ" "MM"0, (%5, %%"FF_REG_a")     \n\t"
            "pcmpeqw "MM"7, "MM"0               \n\t" // out==0 ? 0xFF : 0x00
            MOVQ" (%4, %%"FF_REG_a"), "MM"1     \n\t"
            MOVQ" "MM"7, (%1, %%"FF_REG_a")     \n\t" // 0
            "pandn "MM"1, "MM"0                 \n\t"
            PMAXW(MM"0", MM"3")
            "add $"MMREG_WIDTH", %%"FF_REG_a"   \n\t"
            " js 1b                             \n\t"
            PMAX(MM"3", MM"0")
            "movd "MM"3, %%"FF_REG_a"           \n\t"
            "movzbl %%al, %%eax                 \n\t" // last_non_zero_p1
            : "+a" (last_non_zero_p1)
            : "r" (block+64), "r" (qmat+64), "r" (bias+64),
              "r" (inv_scantable_p1 + 64), "r" (temp_block + 64)
              XMM_CLOBBERS_ONLY("%xmm0", "%xmm1", "%xmm2", "%xmm3",
                                "%xmm4", "%xmm5", "%xmm6", "%xmm7")
        );
    }
    __asm__ volatile(
        "movd %1, "MM"1                     \n\t" // max_qcoeff
        SPREADW(MM"1")
        "psubusw "MM"1, "MM"4               \n\t"
        "packuswb "MM"4, "MM"4              \n\t"
#if COMPILE_TEMPLATE_SSE2
        "packsswb "MM"4, "MM"4              \n\t"
#endif
        "movd "MM"4, %0                     \n\t" // *overflow
        : "=g" (*overflow)
        : "g" (s->max_qcoeff)
    );

    s->bdsp.clear_block(block);
    if (last_non_zero_p1 > 0) {
        if (s->mb_intra)
            temp_block[0] = level;
        ff_block_permute(block, temp_block, s->idsp.idct_permutation,
                         scantable, last_non_zero_p1 - 1);
    }

    return last_non_zero_p1 - 1;
}
