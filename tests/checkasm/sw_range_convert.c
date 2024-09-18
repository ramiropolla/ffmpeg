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

static struct {
    enum AVPixelFormat pix_fmt;
    int bit_depth;
} pixel_formats[] = {
    { AV_PIX_FMT_YUV444P, 8 },
    { AV_PIX_FMT_YUV444P9, 9 },
    { AV_PIX_FMT_YUV444P10, 10 },
    { AV_PIX_FMT_YUV444P12, 12 },
    { AV_PIX_FMT_YUV444P14, 14 },
    { AV_PIX_FMT_YUV444P16, 16 },
};

static void randomize_buffers(int16_t *buf0, int16_t *buf1, int bit_depth, int width)
{
    int32_t *buf0_32 = (int32_t *) buf0;
    int32_t *buf1_32 = (int32_t *) buf1;
    int mask = (1 << bit_depth) - 1;
    int src_shift = bit_depth <= 14 ? 15 - bit_depth : 19 - bit_depth;
    for (int i = 0; i < width; i++) {
        int32_t r = rnd() & mask;
        if (bit_depth == 16) {
            buf0_32[i] = r << src_shift;
            buf1_32[i] = r << src_shift;
        } else {
            buf0[i] = r << src_shift;
            buf1[i] = r << src_shift;
        }
    }
}

static void check_lumConvertRange(int from)
{
    const char *func_str = from ? "lumRangeFromJpeg" : "lumRangeToJpeg";
#define LARGEST_INPUT_SIZE 1920
    static const int input_sizes[] = {8, LARGEST_INPUT_SIZE};
    struct SwsContext *ctx;

    LOCAL_ALIGNED_32(int16_t, dst0, [LARGEST_INPUT_SIZE * 2]);
    LOCAL_ALIGNED_32(int16_t, dst1, [LARGEST_INPUT_SIZE * 2]);

    declare_func(void, int16_t *dst, int width);

    ctx = sws_alloc_context();
    if (sws_init_context(ctx, NULL, NULL) < 0)
        fail();

    ctx->srcRange = from;
    ctx->dstRange = !from;

    for (int pfi = 0; pfi < FF_ARRAY_ELEMS(pixel_formats); pfi++) {
        enum AVPixelFormat pix_fmt = pixel_formats[pfi].pix_fmt;
        int bit_depth = pixel_formats[pfi].bit_depth;
        int sample_size = bit_depth == 16 ? sizeof(int32_t) : sizeof(int16_t);
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(pix_fmt);
        ctx->srcFormat = pix_fmt;
        ctx->dstFormat = pix_fmt;
        ctx->dstBpc = desc->comp[0].depth;
        ff_sws_init_scale(ctx);
    for (int dstWi = 0; dstWi < FF_ARRAY_ELEMS(input_sizes); dstWi++) {
        int width = input_sizes[dstWi];
        randomize_buffers(dst0, dst1, bit_depth, width);
        if (check_func(ctx->lumConvertRange, "%s%d_%d", func_str, bit_depth, width)) {
            call_ref(dst0, width);
            call_new(dst1, width);
            if (memcmp(dst0, dst1, width * sample_size))
                fail();
            if (bit_depth == 8 || bit_depth == 16)
            bench_new(dst1, width);
        }
    }
    }

    sws_freeContext(ctx);
}
#undef LARGEST_INPUT_SIZE

static void check_chrConvertRange(int from)
{
    const char *func_str = from ? "chrRangeFromJpeg" : "chrRangeToJpeg";
#define LARGEST_INPUT_SIZE 1920
    static const int input_sizes[] = {8, LARGEST_INPUT_SIZE};
    struct SwsContext *ctx;

    LOCAL_ALIGNED_32(int16_t, dstU0, [LARGEST_INPUT_SIZE * 2]);
    LOCAL_ALIGNED_32(int16_t, dstV0, [LARGEST_INPUT_SIZE * 2]);
    LOCAL_ALIGNED_32(int16_t, dstU1, [LARGEST_INPUT_SIZE * 2]);
    LOCAL_ALIGNED_32(int16_t, dstV1, [LARGEST_INPUT_SIZE * 2]);

    declare_func(void, int16_t *dstU, int16_t *dstV, int width);

    ctx = sws_alloc_context();
    if (sws_init_context(ctx, NULL, NULL) < 0)
        fail();

    ctx->srcRange = from;
    ctx->dstRange = !from;

    for (int pfi = 0; pfi < FF_ARRAY_ELEMS(pixel_formats); pfi++) {
        enum AVPixelFormat pix_fmt = pixel_formats[pfi].pix_fmt;
        int bit_depth = pixel_formats[pfi].bit_depth;
        int sample_size = bit_depth == 16 ? sizeof(int32_t) : sizeof(int16_t);
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(pix_fmt);
        ctx->srcFormat = pix_fmt;
        ctx->dstFormat = pix_fmt;
        ctx->dstBpc = desc->comp[0].depth;
        ff_sws_init_scale(ctx);
    for (int dstWi = 0; dstWi < FF_ARRAY_ELEMS(input_sizes); dstWi++) {
        int width = input_sizes[dstWi];
        randomize_buffers(dstU0, dstU1, bit_depth, width);
        randomize_buffers(dstV0, dstV1, bit_depth, width);
        if (check_func(ctx->chrConvertRange, "%s%d_%d", func_str, bit_depth, width)) {
            call_ref(dstU0, dstV0, width);
            call_new(dstU1, dstV1, width);
            if (memcmp(dstU0, dstU1, width * sample_size) ||
                memcmp(dstV0, dstV1, width * sample_size))
                fail();
            if (bit_depth == 8 || bit_depth == 16)
            bench_new(dstU1, dstV1, width);
        }
    }
    }

    sws_freeContext(ctx);
}
#undef LARGEST_INPUT_SIZE

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
