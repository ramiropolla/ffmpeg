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

#define randomize_buffers(buf, size)      \
    do {                                  \
        for (int j = 0; j < size; j += 4) \
            AV_WN32(buf + j, rnd());      \
    } while (0)

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

#define NUM_LINES 4
#define MAX_LINE_SIZE 1920
#define EDGE_WIDTH 16
#define LINESIZE (EDGE_WIDTH + MAX_LINE_SIZE + EDGE_WIDTH)
#define BUFSIZE ((EDGE_WIDTH + NUM_LINES + EDGE_WIDTH) * LINESIZE)

static void check_draw_edges(MpegvideoEncDSPContext *c)
{
    static const int input_sizes[] = {8, 128, 1080, MAX_LINE_SIZE};
    LOCAL_ALIGNED_16(uint8_t, buf0, [BUFSIZE]);
    LOCAL_ALIGNED_16(uint8_t, buf1, [BUFSIZE]);

    declare_func_emms(AV_CPU_FLAG_MMX, void, uint8_t *buf, int wrap, int width, int height,
                                             int w, int h, int sides);

    for (int isi = 0; isi < FF_ARRAY_ELEMS(input_sizes); isi++) {
        int width = input_sizes[isi];
        int linesize = EDGE_WIDTH + width + EDGE_WIDTH;
        int height = (BUFSIZE / linesize) - (2 * EDGE_WIDTH);
        uint8_t *dst0 = buf0 + EDGE_WIDTH * linesize + EDGE_WIDTH;
        uint8_t *dst1 = buf1 + EDGE_WIDTH * linesize + EDGE_WIDTH;

        for (int shift = 0; shift < 3; shift++) {
            int edge = EDGE_WIDTH >> shift;
            if (check_func(c->draw_edges, "draw_edges_%d_%d", width, shift)) {
                for (int i = 0; i < BUFSIZE; i++) {
                    uint8_t r = rnd();
                    buf0[i] = r;
                    buf1[i] = r;
                }
                call_ref(dst0, linesize, width, height, edge, edge, EDGE_BOTTOM | EDGE_TOP);
                call_new(dst1, linesize, width, height, edge, edge, EDGE_BOTTOM | EDGE_TOP);
                if (memcmp(buf0, buf1, BUFSIZE))
                    fail();
                bench_new(dst1, linesize, width, height, edge, edge, EDGE_BOTTOM | EDGE_TOP);
            }
        }
    }
}

#undef NUM_LINES
#undef MAX_LINE_SIZE
#undef EDGE_WIDTH

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
    check_draw_edges(&c);
    report("draw_edges");
}
