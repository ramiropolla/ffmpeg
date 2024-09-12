/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with FFmpeg; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <string.h>

#include "libavutil/common.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/mem_internal.h"

#include "libswscale/swscale.h"
#include "libswscale/swscale_internal.h"

#include "checkasm.h"

static int int16_cmp_off_by_n(const int16_t *ref, const int16_t *test, size_t n, int accuracy)
{
    for (size_t i = 0; i < n; i++) {
        if (abs(ref[i] - test[i]) > accuracy)
            return 1;
    }
    return 0;
}

static void check_lumConvertRange(int from)
{
    const char *func_str = from ? "lumRangeFromJpeg" : "lumRangeToJpeg";
#define LARGEST_INPUT_SIZE 512
#define INPUT_SIZES 6
    static const int input_sizes[] = {8, 24, 128, 144, 256, 512};
    struct SwsContext *ctx;

    LOCAL_ALIGNED_32(int16_t, dst0, [LARGEST_INPUT_SIZE]);
    LOCAL_ALIGNED_32(int16_t, dst1, [LARGEST_INPUT_SIZE]);

    declare_func(void, int16_t *dst, int width,
                       int coeff, int offset, int amax);

    ctx = sws_alloc_context();
    if (sws_init_context(ctx, NULL, NULL) < 0)
        fail();

    ctx->srcFormat = from ? AV_PIX_FMT_YUVJ444P : AV_PIX_FMT_YUV444P;
    ctx->dstFormat = from ? AV_PIX_FMT_YUV444P : AV_PIX_FMT_YUVJ444P;
    ctx->srcRange = from;
    ctx->dstRange = !from;

    for (int dstWi = 0; dstWi < INPUT_SIZES; dstWi++) {
        int width = input_sizes[dstWi];
        for (int i = 0; i < width; i++) {
            uint8_t r = rnd();
            dst0[i] = (int16_t) r << 7;
            dst1[i] = (int16_t) r << 7;
        }
        ff_sws_init_scale(ctx);
        if (check_func(ctx->lumConvertRange, "%s_%d", func_str, width)) {
            call_ref(dst0, width,
                     ctx->lumConvertRange_coeff, ctx->lumConvertRange_offset,
                     ctx->lumConvertRange_max);
            call_new(dst1, width,
                     ctx->lumConvertRange_coeff, ctx->lumConvertRange_offset,
                     ctx->lumConvertRange_max);
            if (int16_cmp_off_by_n(dst0, dst1, width, 1))
                fail();
            bench_new(dst1, width,
                     ctx->lumConvertRange_coeff, ctx->lumConvertRange_offset,
                     ctx->lumConvertRange_max);
        }
    }

    sws_freeContext(ctx);
}
#undef LARGEST_INPUT_SIZE
#undef INPUT_SIZES

static void check_chrConvertRange(int from)
{
    const char *func_str = from ? "chrRangeFromJpeg" : "chrRangeToJpeg";
#define LARGEST_INPUT_SIZE 512
#define INPUT_SIZES 6
    static const int input_sizes[] = {8, 24, 128, 144, 256, 512};
    struct SwsContext *ctx;

    LOCAL_ALIGNED_32(int16_t, dstU0, [LARGEST_INPUT_SIZE]);
    LOCAL_ALIGNED_32(int16_t, dstV0, [LARGEST_INPUT_SIZE]);
    LOCAL_ALIGNED_32(int16_t, dstU1, [LARGEST_INPUT_SIZE]);
    LOCAL_ALIGNED_32(int16_t, dstV1, [LARGEST_INPUT_SIZE]);

    declare_func(void, int16_t *dstU, int16_t *dstV, int width,
                       int coeff, int offset, int amax);

    ctx = sws_alloc_context();
    if (sws_init_context(ctx, NULL, NULL) < 0)
        fail();

    ctx->srcFormat = from ? AV_PIX_FMT_YUVJ444P : AV_PIX_FMT_YUV444P;
    ctx->dstFormat = from ? AV_PIX_FMT_YUV444P : AV_PIX_FMT_YUVJ444P;
    ctx->srcRange = from;
    ctx->dstRange = !from;

    for (int dstWi = 0; dstWi < INPUT_SIZES; dstWi++) {
        int width = input_sizes[dstWi];
        for (int i = 0; i < width; i++) {
            uint8_t r = rnd();
            dstU0[i] = (int16_t) r << 7;
            dstV0[i] = (int16_t) r << 7;
            dstU1[i] = (int16_t) r << 7;
            dstV1[i] = (int16_t) r << 7;
        }
        ff_sws_init_scale(ctx);
        if (check_func(ctx->chrConvertRange, "%s_%d", func_str, width)) {
            call_ref(dstU0, dstV0, width,
                     ctx->chrConvertRange_coeff, ctx->chrConvertRange_offset,
                     ctx->chrConvertRange_max);
            call_new(dstU1, dstV1, width,
                     ctx->chrConvertRange_coeff, ctx->chrConvertRange_offset,
                     ctx->chrConvertRange_max);
            if (int16_cmp_off_by_n(dstU0, dstU1, width, 1) ||
                int16_cmp_off_by_n(dstV0, dstV1, width, 1))
                fail();
            bench_new(dstU1, dstV1, width,
                      ctx->chrConvertRange_coeff, ctx->chrConvertRange_offset,
                      ctx->chrConvertRange_max);
        }
    }

    sws_freeContext(ctx);
}
#undef LARGEST_INPUT_SIZE
#undef INPUT_SIZES

void checkasm_check_sw_range_convert(void)
{
    check_lumConvertRange(1);
    report("lumRangeFromJpeg");
    check_chrConvertRange(1);
    report("chrRangeFromJpeg");
    check_lumConvertRange(0);
    report("lumRangeToJpeg");
    check_chrConvertRange(0);
    report("chrRangeToJpeg");
}
