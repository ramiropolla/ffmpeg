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

#include "libavutil/mem.h"
#include "libavutil/mem_internal.h"

#include "libavcodec/mpegvideoencdsp.h"

#include "checkasm.h"

static void check_pix_sum(MpegvideoEncDSPContext *c)
{
    LOCAL_ALIGNED_16(uint8_t, src, [16 * 16]);

    declare_func(int, const uint8_t *pix, int line_size);

    for (int i = 0; i < 16 * 16; i++)
        src[i] = rnd();

    if (check_func(c->pix_sum, "pix_sum")) {
        int sum0, sum1;
        sum0 = call_ref(src, 16);
        sum1 = call_new(src, 16);
        if (sum0 != sum1)
            fail();
        bench_new(src, 16);
    }
}

static void check_pix_norm1(MpegvideoEncDSPContext *c)
{
    LOCAL_ALIGNED_16(uint8_t, src, [16 * 16]);

    declare_func(int, const uint8_t *pix, int line_size);

    for (int i = 0; i < 16 * 16; i++)
        src[i] = rnd();

    if (check_func(c->pix_norm1, "pix_norm1")) {
        int sum0, sum1;
        sum0 = call_ref(src, 16);
        sum1 = call_new(src, 16);
        if (sum0 != sum1)
            fail();
        bench_new(src, 16);
    }
}

void checkasm_check_mpegvideoencdsp(void)
{
    AVCodecContext avctx = {
        .bits_per_raw_sample = 8,
    };
    MpegvideoEncDSPContext c = { 0 };

    ff_mpegvideoencdsp_init(&c, &avctx);

    check_pix_sum(&c);
    report("pix_sum");
    check_pix_norm1(&c);
    report("pix_norm1");
}
