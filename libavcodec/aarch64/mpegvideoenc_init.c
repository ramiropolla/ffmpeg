/*
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

#include "libavutil/attributes.h"
#include "libavutil/cpu.h"
#include "libavutil/mem_internal.h"
#include "libavutil/aarch64/cpu.h"
#include "libavcodec/avcodec.h"
#include "libavcodec/dct.h"
#include "libavcodec/mpegvideoenc.h"

#define QUANT_BIAS_SHIFT 8
#define QMAT_SHIFT 21

/* not permutated inverse zigzag_direct + 1 for NEON quantizer */
DECLARE_ALIGNED(16, const uint16_t, ff_inv_zigzag_direct16)[64] = {
    1,  2,  6,  7,  15, 16, 28, 29,
    3,  5,  8,  14, 17, 27, 30, 43,
    4,  9,  13, 18, 26, 31, 42, 44,
    10, 12, 19, 25, 32, 41, 45, 54,
    11, 20, 24, 33, 40, 46, 53, 55,
    21, 23, 34, 39, 47, 52, 56, 61,
    22, 35, 38, 48, 51, 57, 60, 62,
    36, 37, 49, 50, 58, 59, 63, 64,
};

void ff_fdct_neon(int16_t *block);
int ff_quantize_neon(int16_t *temp_block, int16_t *block, const int16_t *qmat, const int16_t *bias, int *max_qcoeff, int last_non_zero_p1);

static int dct_quantize_neon(MpegEncContext *s,
                             int16_t *block, int n,
                             int qscale, int *overflow)
{
    LOCAL_ALIGNED_16(int16_t, temp_block, [64]);
    int level, last_non_zero_p1, q;
    int max_qcoeff;
    const uint16_t *qmat, *bias;

//    s->fdsp.fdct(block);
    ff_fdct_neon(block);

    if (s->dct_error_sum)
        s->denoise_dct(s, block);

    if (s->mb_intra) {
        if (n < 4) {
            q = s->y_dc_scale;
            bias = s->q_intra_matrix16[qscale][1];
            qmat = s->q_intra_matrix16[qscale][0];
        } else {
            q = s->c_dc_scale;
            bias = s->q_chroma_intra_matrix16[qscale][1];
            qmat = s->q_chroma_intra_matrix16[qscale][0];
        }
        if (!s->h263_aic) {
            q = q << 3;
            level = (block[0] + (q >> 1)) / q;
        } else {
            /* For AIC we skip quant/dequant of INTRADC */
            level = (block[0] + 4) >> 3;
        }

        /* note: block[0] is assumed to be positive */
        last_non_zero_p1 = 1;
    } else {
        last_non_zero_p1 = 0;
        bias = s->q_inter_matrix16[qscale][1];
        qmat = s->q_inter_matrix16[qscale][0];
    }

    last_non_zero_p1 = ff_quantize_neon(temp_block, block, qmat, bias, &max_qcoeff, last_non_zero_p1);
    // TODO restore overflow, it's not needed for now.
    // *overflow = s->max_qcoeff < max_qcoeff; // overflow might have happened
    *overflow = 0;

    if (s->mb_intra)
        block[0] = level;
    else
        block[0] = temp_block[0];

    /* we need this permutation so that we correct the IDCT, we only permute the !=0 elements */
    if (s->idsp.perm_type == FF_IDCT_PERM_PARTTRANS) {
        if (last_non_zero_p1 <= 1) goto end;
        block[0x08] = temp_block[0x01];
        block[0x01] = temp_block[0x08]; block[0x02] = temp_block[0x10];
        if (last_non_zero_p1 <= 4) goto end;
        block[0x09] = temp_block[0x09]; block[0x10] = temp_block[0x02];
        block[0x18] = temp_block[0x03];
        if (last_non_zero_p1 <= 7) goto end;
        block[0x11] = temp_block[0x0A]; block[0x0A] = temp_block[0x11];
        block[0x03] = temp_block[0x18]; block[0x20] = temp_block[0x20];
        if (last_non_zero_p1 <= 11) goto end;
        block[0x0B] = temp_block[0x19]; block[0x12] = temp_block[0x12];
        block[0x19] = temp_block[0x0B]; block[0x04] = temp_block[0x04];
        block[0x0C] = temp_block[0x05];
        if (last_non_zero_p1 <= 16) goto end;
        block[0x05] = temp_block[0x0C]; block[0x1A] = temp_block[0x13];
        block[0x13] = temp_block[0x1A]; block[0x28] = temp_block[0x21];
        block[0x21] = temp_block[0x28]; block[0x22] = temp_block[0x30];
        block[0x29] = temp_block[0x29]; block[0x30] = temp_block[0x22];
        if (last_non_zero_p1 <= 24) goto end;
        block[0x1B] = temp_block[0x1B]; block[0x06] = temp_block[0x14];
        block[0x0D] = temp_block[0x0D]; block[0x14] = temp_block[0x06];
        block[0x1C] = temp_block[0x07]; block[0x15] = temp_block[0x0E];
        block[0x0E] = temp_block[0x15]; block[0x07] = temp_block[0x1C];
        if (last_non_zero_p1 <= 32) goto end;
        block[0x38] = temp_block[0x23]; block[0x31] = temp_block[0x2A];
        block[0x2A] = temp_block[0x31]; block[0x23] = temp_block[0x38];
        block[0x2B] = temp_block[0x39]; block[0x32] = temp_block[0x32];
        block[0x39] = temp_block[0x2B]; block[0x24] = temp_block[0x24];
        if (last_non_zero_p1 <= 40) goto end;
        block[0x0F] = temp_block[0x1D]; block[0x16] = temp_block[0x16];
        block[0x1D] = temp_block[0x0F]; block[0x1E] = temp_block[0x17];
        block[0x17] = temp_block[0x1E]; block[0x2C] = temp_block[0x25];
        block[0x25] = temp_block[0x2C]; block[0x3A] = temp_block[0x33];
        if (last_non_zero_p1 <= 48) goto end;
        block[0x33] = temp_block[0x3A]; block[0x3B] = temp_block[0x3B];
        block[0x26] = temp_block[0x34]; block[0x2D] = temp_block[0x2D];
        block[0x34] = temp_block[0x26]; block[0x1F] = temp_block[0x1F];
        block[0x3C] = temp_block[0x27]; block[0x35] = temp_block[0x2E];
        if (last_non_zero_p1 <= 56) goto end;
        block[0x2E] = temp_block[0x35]; block[0x27] = temp_block[0x3C];
        block[0x2F] = temp_block[0x3D]; block[0x36] = temp_block[0x36];
        block[0x3D] = temp_block[0x2F]; block[0x3E] = temp_block[0x37];
        block[0x37] = temp_block[0x3E]; block[0x3F] = temp_block[0x3F];
    } else if (s->idsp.perm_type == FF_IDCT_PERM_NONE) {
        if (last_non_zero_p1 <= 1) goto end;
        block[0x01] = temp_block[0x01];
        block[0x08] = temp_block[0x08]; block[0x10] = temp_block[0x10];
        if (last_non_zero_p1 <= 4) goto end;
        block[0x09] = temp_block[0x09]; block[0x02] = temp_block[0x02];
        block[0x03] = temp_block[0x03];
        if (last_non_zero_p1 <= 7) goto end;
        block[0x0A] = temp_block[0x0A]; block[0x11] = temp_block[0x11];
        block[0x18] = temp_block[0x18]; block[0x20] = temp_block[0x20];
        if (last_non_zero_p1 <= 11) goto end;
        block[0x19] = temp_block[0x19];
        block[0x12] = temp_block[0x12]; block[0x0B] = temp_block[0x0B];
        block[0x04] = temp_block[0x04]; block[0x05] = temp_block[0x05];
        if (last_non_zero_p1 <= 16) goto end;
        block[0x0C] = temp_block[0x0C]; block[0x13] = temp_block[0x13];
        block[0x1A] = temp_block[0x1A]; block[0x21] = temp_block[0x21];
        block[0x28] = temp_block[0x28]; block[0x30] = temp_block[0x30];
        block[0x29] = temp_block[0x29]; block[0x22] = temp_block[0x22];
        if (last_non_zero_p1 <= 24) goto end;
        block[0x1B] = temp_block[0x1B]; block[0x14] = temp_block[0x14];
        block[0x0D] = temp_block[0x0D]; block[0x06] = temp_block[0x06];
        block[0x07] = temp_block[0x07]; block[0x0E] = temp_block[0x0E];
        block[0x15] = temp_block[0x15]; block[0x1C] = temp_block[0x1C];
        if (last_non_zero_p1 <= 32) goto end;
        block[0x23] = temp_block[0x23]; block[0x2A] = temp_block[0x2A];
        block[0x31] = temp_block[0x31]; block[0x38] = temp_block[0x38];
        block[0x39] = temp_block[0x39]; block[0x32] = temp_block[0x32];
        block[0x2B] = temp_block[0x2B]; block[0x24] = temp_block[0x24];
        if (last_non_zero_p1 <= 40) goto end;
        block[0x1D] = temp_block[0x1D]; block[0x16] = temp_block[0x16];
        block[0x0F] = temp_block[0x0F]; block[0x17] = temp_block[0x17];
        block[0x1E] = temp_block[0x1E]; block[0x25] = temp_block[0x25];
        block[0x2C] = temp_block[0x2C]; block[0x33] = temp_block[0x33];
        if (last_non_zero_p1 <= 48) goto end;
        block[0x3A] = temp_block[0x3A]; block[0x3B] = temp_block[0x3B];
        block[0x34] = temp_block[0x34]; block[0x2D] = temp_block[0x2D];
        block[0x26] = temp_block[0x26]; block[0x1F] = temp_block[0x1F];
        block[0x27] = temp_block[0x27]; block[0x2E] = temp_block[0x2E];
        if (last_non_zero_p1 <= 56) goto end;
        block[0x35] = temp_block[0x35]; block[0x3C] = temp_block[0x3C];
        block[0x3D] = temp_block[0x3D]; block[0x36] = temp_block[0x36];
        block[0x2F] = temp_block[0x2F]; block[0x37] = temp_block[0x37];
        block[0x3E] = temp_block[0x3E]; block[0x3F] = temp_block[0x3F];
    } else if (s->idsp.perm_type == FF_IDCT_PERM_TRANSPOSE) {
        if (last_non_zero_p1 <= 1) goto end;
        block[0x08] = temp_block[0x01];
        block[0x01] = temp_block[0x08]; block[0x02] = temp_block[0x10];
        if (last_non_zero_p1 <= 4) goto end;
        block[0x09] = temp_block[0x09]; block[0x10] = temp_block[0x02];
        block[0x18] = temp_block[0x03];
        if (last_non_zero_p1 <= 7) goto end;
        block[0x11] = temp_block[0x0A]; block[0x0A] = temp_block[0x11];
        block[0x03] = temp_block[0x18]; block[0x04] = temp_block[0x20];
        if (last_non_zero_p1 <= 11) goto end;
        block[0x0B] = temp_block[0x19];
        block[0x12] = temp_block[0x12]; block[0x19] = temp_block[0x0B];
        block[0x20] = temp_block[0x04]; block[0x28] = temp_block[0x05];
        if (last_non_zero_p1 <= 16) goto end;
        block[0x21] = temp_block[0x0C]; block[0x1A] = temp_block[0x13];
        block[0x13] = temp_block[0x1A]; block[0x0C] = temp_block[0x21];
        block[0x05] = temp_block[0x28]; block[0x06] = temp_block[0x30];
        block[0x0D] = temp_block[0x29]; block[0x14] = temp_block[0x22];
        if (last_non_zero_p1 <= 24) goto end;
        block[0x1B] = temp_block[0x1B]; block[0x22] = temp_block[0x14];
        block[0x29] = temp_block[0x0D]; block[0x30] = temp_block[0x06];
        block[0x38] = temp_block[0x07]; block[0x31] = temp_block[0x0E];
        block[0x2A] = temp_block[0x15]; block[0x23] = temp_block[0x1C];
        if (last_non_zero_p1 <= 32) goto end;
        block[0x1C] = temp_block[0x23]; block[0x15] = temp_block[0x2A];
        block[0x0E] = temp_block[0x31]; block[0x07] = temp_block[0x38];
        block[0x0F] = temp_block[0x39]; block[0x16] = temp_block[0x32];
        block[0x1D] = temp_block[0x2B]; block[0x24] = temp_block[0x24];
        if (last_non_zero_p1 <= 40) goto end;
        block[0x2B] = temp_block[0x1D]; block[0x32] = temp_block[0x16];
        block[0x39] = temp_block[0x0F]; block[0x3A] = temp_block[0x17];
        block[0x33] = temp_block[0x1E]; block[0x2C] = temp_block[0x25];
        block[0x25] = temp_block[0x2C]; block[0x1E] = temp_block[0x33];
        if (last_non_zero_p1 <= 48) goto end;
        block[0x17] = temp_block[0x3A]; block[0x1F] = temp_block[0x3B];
        block[0x26] = temp_block[0x34]; block[0x2D] = temp_block[0x2D];
        block[0x34] = temp_block[0x26]; block[0x3B] = temp_block[0x1F];
        block[0x3C] = temp_block[0x27]; block[0x35] = temp_block[0x2E];
        if (last_non_zero_p1 <= 56) goto end;
        block[0x2E] = temp_block[0x35]; block[0x27] = temp_block[0x3C];
        block[0x2F] = temp_block[0x3D]; block[0x36] = temp_block[0x36];
        block[0x3D] = temp_block[0x2F]; block[0x3E] = temp_block[0x37];
        block[0x37] = temp_block[0x3E]; block[0x3F] = temp_block[0x3F];
    } else if (s->idsp.perm_type == FF_IDCT_PERM_LIBMPEG2) {
        if (last_non_zero_p1 <= 1) goto end;
        block[0x04] = temp_block[0x01];
        block[0x08] = temp_block[0x08]; block[0x10] = temp_block[0x10];
        if (last_non_zero_p1 <= 4) goto end;
        block[0x0C] = temp_block[0x09]; block[0x01] = temp_block[0x02];
        block[0x05] = temp_block[0x03];
        if (last_non_zero_p1 <= 7) goto end;
        block[0x09] = temp_block[0x0A]; block[0x14] = temp_block[0x11];
        block[0x18] = temp_block[0x18]; block[0x20] = temp_block[0x20];
        if (last_non_zero_p1 <= 11) goto end;
        block[0x1C] = temp_block[0x19];
        block[0x11] = temp_block[0x12]; block[0x0D] = temp_block[0x0B];
        block[0x02] = temp_block[0x04]; block[0x06] = temp_block[0x05];
        if (last_non_zero_p1 <= 16) goto end;
        block[0x0A] = temp_block[0x0C]; block[0x15] = temp_block[0x13];
        block[0x19] = temp_block[0x1A]; block[0x24] = temp_block[0x21];
        block[0x28] = temp_block[0x28]; block[0x30] = temp_block[0x30];
        block[0x2C] = temp_block[0x29]; block[0x21] = temp_block[0x22];
        if (last_non_zero_p1 <= 24) goto end;
        block[0x1D] = temp_block[0x1B]; block[0x12] = temp_block[0x14];
        block[0x0E] = temp_block[0x0D]; block[0x03] = temp_block[0x06];
        block[0x07] = temp_block[0x07]; block[0x0B] = temp_block[0x0E];
        block[0x16] = temp_block[0x15]; block[0x1A] = temp_block[0x1C];
        if (last_non_zero_p1 <= 32) goto end;
        block[0x25] = temp_block[0x23]; block[0x29] = temp_block[0x2A];
        block[0x34] = temp_block[0x31]; block[0x38] = temp_block[0x38];
        block[0x3C] = temp_block[0x39]; block[0x31] = temp_block[0x32];
        block[0x2D] = temp_block[0x2B]; block[0x22] = temp_block[0x24];
        if (last_non_zero_p1 <= 40) goto end;
        block[0x1E] = temp_block[0x1D]; block[0x13] = temp_block[0x16];
        block[0x0F] = temp_block[0x0F]; block[0x17] = temp_block[0x17];
        block[0x1B] = temp_block[0x1E]; block[0x26] = temp_block[0x25];
        block[0x2A] = temp_block[0x2C]; block[0x35] = temp_block[0x33];
        if (last_non_zero_p1 <= 48) goto end;
        block[0x39] = temp_block[0x3A]; block[0x3D] = temp_block[0x3B];
        block[0x32] = temp_block[0x34]; block[0x2E] = temp_block[0x2D];
        block[0x23] = temp_block[0x26]; block[0x1F] = temp_block[0x1F];
        block[0x27] = temp_block[0x27]; block[0x2B] = temp_block[0x2E];
        if (last_non_zero_p1 <= 56) goto end;
        block[0x36] = temp_block[0x35]; block[0x3A] = temp_block[0x3C];
        block[0x3E] = temp_block[0x3D]; block[0x33] = temp_block[0x36];
        block[0x2F] = temp_block[0x2F]; block[0x37] = temp_block[0x37];
        block[0x3B] = temp_block[0x3E]; block[0x3F] = temp_block[0x3F];
    } else {
        av_log(s, AV_LOG_DEBUG, "s->idsp.perm_type: %d\n",
               (int) s->idsp.perm_type);
        av_assert0(s->idsp.perm_type == FF_IDCT_PERM_PARTTRANS ||
                   s->idsp.perm_type == FF_IDCT_PERM_NONE ||
                   s->idsp.perm_type == FF_IDCT_PERM_TRANSPOSE ||
                   s->idsp.perm_type == FF_IDCT_PERM_LIBMPEG2);
    }
    end:
    return last_non_zero_p1 - 1;
}

av_cold void ff_dct_encode_init_aarch64(MpegEncContext *s)
{
    int cpu_flags = av_get_cpu_flags();

    if (have_neon(cpu_flags)) {
        s->dct_quantize = dct_quantize_neon;
    }
}
