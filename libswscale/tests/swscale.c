/*
 * Copyright (C) 2024      Niklas Haas
 * Copyright (C) 2003-2011 Michael Niedermayer <michaelni@gmx.at>
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stdarg.h>
#include <signal.h>

#undef HAVE_AV_CONFIG_H
#include "libavutil/cpu.h"
#include "libavutil/pixdesc.h"
#include "libavutil/lfg.h"
#include "libavutil/sfc64.h"
#include "libavutil/frame.h"
#include "libavutil/opt.h"
#include "libavutil/time.h"
#include "libavutil/pixfmt.h"
#include "libavutil/avassert.h"
#include "libavutil/macros.h"

#include "libswscale/swscale.h"

struct options {
    enum AVPixelFormat src_fmt;
    enum AVPixelFormat dst_fmt;
    double prob;
    int w, h;
    int threads;
    int iters;
    int bench;
    int flags;
    int dither;
    int unscaled;
    int legacy;
};

struct mode {
    SwsFlags flags;
    SwsDither dither;
};

const SwsFlags flags[] = {
    0, // test defaults
    SWS_FAST_BILINEAR,
    SWS_BILINEAR,
    SWS_BICUBIC,
    SWS_X | SWS_BITEXACT,
    SWS_POINT,
    SWS_AREA | SWS_ACCURATE_RND,
    SWS_BICUBIC | SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP,
};

static FFSFC64 prng_state;

/* reused between tests for efficiency */
static SwsContext *sws_ref_src;
static SwsContext *sws_src_dst;
static SwsContext *sws_dst_out;

static double speedup_logavg;
static double speedup_min = 1e10;
static double speedup_max = 0;
static int speedup_count;

static const char *speedup_color(double ratio)
{
    return ratio > 10.00 ? "\033[1;94m" : /* bold blue */
           ratio >  2.00 ? "\033[1;32m" : /* bold green */
           ratio >  1.02 ? "\033[32m"   : /* green */
           ratio >  0.98 ? ""           : /* default */
           ratio >  0.90 ? "\033[33m"   : /* yellow */
           ratio >  0.75 ? "\033[31m"   : /* red */
            "\033[1;31m";  /* bold red */
}

static void exit_handler(int sig)
{
    if (speedup_count) {
        double ratio = exp(speedup_logavg / speedup_count);
        printf("Overall speedup=%.3fx %s%s\033[0m, min=%.3fx max=%.3fx\n", ratio,
               speedup_color(ratio), ratio >= 1.0 ? "faster" : "slower",
               speedup_min, speedup_max);
    }

    exit(sig);
}

/* Estimate luma variance assuming uniform dither noise distribution */
static float estimate_quantization_noise(enum AVPixelFormat fmt)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(fmt);
    float variance = 1.0 / 12;
    if (desc->comp[0].depth < 8) {
        /* Extra headroom for very low bit depth output */
        variance *= (8 - desc->comp[0].depth);
    }

    if (desc->flags & AV_PIX_FMT_FLAG_FLOAT) {
        return 0.0;
    } else if (desc->flags & AV_PIX_FMT_FLAG_RGB) {
        const float r = 0.299 / (1 << desc->comp[0].depth);
        const float g = 0.587 / (1 << desc->comp[1].depth);
        const float b = 0.114 / (1 << desc->comp[2].depth);
        return (r * r + g * g + b * b) * variance;
    } else {
        const float y = 1.0 / (1 << desc->comp[0].depth);
        return y * y * variance;
    }
}

static int fmt_comps(enum AVPixelFormat fmt)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(fmt);
    int comps = desc->nb_components >= 3 ? 0x7 : 0x1;
    if (desc->flags & AV_PIX_FMT_FLAG_ALPHA)
        comps |= 0x8;
    return comps;
}

static void get_ssim(float ssim[4], const AVFrame *out, const AVFrame *ref, int comps)
{
    av_assert1(out->format == AV_PIX_FMT_YUVA444P);
    av_assert1(ref->format == out->format);
    av_assert1(ref->width == out->width && ref->height == out->height);

    for (int p = 0; p < 4; p++) {
        const int stride_a = out->linesize[p];
        const int stride_b = ref->linesize[p];
        const int w = out->width;
        const int h = out->height;

        const int is_chroma = p == 1 || p == 2;
        const uint8_t def = is_chroma ? 128 : 0xFF;
        const int has_ref = comps & (1 << p);
        double sum = 0;
        int count = 0;

        /* 4x4 SSIM */
        for (int y = 0; y < (h & ~3); y += 4) {
            for (int x = 0; x < (w & ~3); x += 4) {
                const float c1 = .01 * .01 * 255 * 255 * 64;
                const float c2 = .03 * .03 * 255 * 255 * 64 * 63;
                int s1 = 0, s2 = 0, ss = 0, s12 = 0, var, covar;

                for (int yy = 0; yy < 4; yy++) {
                    for (int xx = 0; xx < 4; xx++) {
                        int a = out->data[p][(y + yy) * stride_a + x + xx];
                        int b = has_ref ? ref->data[p][(y + yy) * stride_b + x + xx] : def;
                        s1  += a;
                        s2  += b;
                        ss  += a * a + b * b;
                        s12 += a * b;
                    }
                }

                var = ss * 64 - s1 * s1 - s2 * s2;
                covar = s12 * 64 - s1 * s2;
                sum += (2 * s1 * s2 + c1) * (2 * covar + c2) /
                       ((s1 * s1 + s2 * s2 + c1) * (var + c2));
                count++;
            }
        }

        ssim[p] = count ? sum / count : 0.0;
    }
}

static float get_loss(const float ssim[4])
{
    const float weights[3] = { 0.8, 0.1, 0.1 }; /* tuned for Y'CrCr */

    float sum = 0;
    for (int i = 0; i < 3; i++)
        sum += weights[i] * ssim[i];
    sum *= ssim[3]; /* ensure alpha errors get caught */

    return 1.0 - sum;
}

static int init_legacy_context(AVFrame *dst, const AVFrame *src,
                               const struct mode *mode, const struct options *opts)
{
    sws_free_context(&sws_src_dst);
    sws_src_dst = sws_alloc_context();
    if (!sws_src_dst)
        return AVERROR(ENOMEM);

    sws_src_dst->src_w      = src->width;
    sws_src_dst->src_h      = src->height;
    sws_src_dst->src_format = src->format;
    sws_src_dst->dst_w      = dst->width;
    sws_src_dst->dst_h      = dst->height;
    sws_src_dst->dst_format = dst->format;
    sws_src_dst->flags      = mode->flags;
    sws_src_dst->dither     = mode->dither;
    sws_src_dst->threads    = opts->threads;

    /* Clear dst frame to prevent overwriting data referenced from src. */
    av_frame_unref(dst);
    av_frame_copy_props(dst, src);
    dst->width  = sws_src_dst->dst_w;
    dst->height = sws_src_dst->dst_h;
    dst->format = sws_src_dst->dst_format;

    return sws_init_context(sws_src_dst, NULL, NULL);
}

static void print_loss(float loss_ref, float loss, const char *str)
{
    if (loss - loss_ref <= 1e-4)
        return;
    const int bad = loss - loss_ref > 1e-2;
    const int level = bad ? AV_LOG_ERROR : AV_LOG_WARNING;
    av_log(NULL, level, "  loss is %s by %g, %s loss %g\n",
           bad ? "WORSE" : "worse", loss - loss_ref, str, loss_ref);
}

static void print_test(enum AVPixelFormat src_fmt, enum AVPixelFormat dst_fmt,
                       const AVFrame *ref, AVFrame *src, AVFrame *dst, AVFrame *out,
                       int dst_w, int dst_h,
                       const struct mode *mode, const struct options *opts,
                       const float ssim[4],
                       float loss_ref, float expected_loss, float loss,
                       int64_t time_ref, int64_t time)
{
    printf("%-14s %4dx%4d -> %-14s %4dx%4d, flags=0x%08x dither=%u"
           " SSIM={Y=%f U=%f V=%f A=%f} loss=%e",
           av_get_pix_fmt_name(src_fmt), src->width, src->height,
           av_get_pix_fmt_name(dst_fmt), dst->width, dst->height,
           mode->flags, mode->dither,
           ssim[0], ssim[1], ssim[2], ssim[3], loss);

    if (opts->bench) {
        printf(" time=%6"PRId64" us", time / opts->iters);
        if (time_ref) {
            double ratio = (double) time_ref / time;
            if (FFMIN(time, time_ref) > 100 /* don't pollute stats with low precision */) {
                speedup_min = FFMIN(speedup_min, ratio);
                speedup_max = FFMAX(speedup_max, ratio);
                speedup_logavg += log(ratio);
                speedup_count++;
            }

            printf(" ref=%6"PRId64" us, speedup=%.3fx %s%s\033[0m",
                   time_ref / opts->iters, ratio,
                   speedup_color(ratio), ratio >= 1.0 ? "faster" : "slower");
        }
    }
    printf("\n");

    if (dst_w >= ref->width && dst_h >= ref->height)
        print_loss(expected_loss, loss, "expected");

    if (!isnan(loss_ref))
        print_loss(loss_ref, loss, "ref");

    fflush(stdout);
}

static int sws_scale_frame_wrapper(SwsContext *c, AVFrame *dst, const AVFrame *src)
{
    int ret = sws_scale_frame(c, dst, src);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed %s ---> %s\n",
               av_get_pix_fmt_name(src->format), av_get_pix_fmt_name(dst->format));
    }
    return ret;
}

static int initialize_frame(AVFrame **pframe, const AVFrame *ref, int width, int height, enum AVPixelFormat format)
{
    AVFrame *frame = av_frame_alloc();
    if (!frame)
        return AVERROR(ENOMEM);
    av_frame_copy_props(frame, ref);
    frame->width  = width;
    frame->height = height;
    frame->format = format;
    *pframe = frame;
    return 0;
}

/* Runs a series of ref -> src -> dst -> out, and compares out vs ref */
static int run_test(enum AVPixelFormat src_fmt, enum AVPixelFormat dst_fmt,
                    int dst_w, int dst_h,
                    const struct mode *mode, const struct options *opts,
                    const AVFrame *ref, AVFrame *src,
                    float loss_ref, int64_t time_ref)
{
    AVFrame *dst = NULL, *out = NULL;
    float ssim[4];
    const int comps = fmt_comps(src_fmt) & fmt_comps(dst_fmt);
    int64_t time;
    int ret = 0;

    /* Estimate the expected amount of loss from bit depth reduction */
    const float c1 = 0.01 * 0.01; /* stabilization constant */
    const float ref_var = 1.0 / 12.0; /* uniformly distributed signal */
    const float src_var = estimate_quantization_noise(src_fmt);
    const float dst_var = estimate_quantization_noise(dst_fmt);
    const float out_var = estimate_quantization_noise(ref->format);
    const float total_var = src_var + dst_var + out_var;
    const float ssim_luma = (2 * ref_var + c1) / (2 * ref_var + total_var + c1);
    const float ssim_expected[4] = { ssim_luma, 1, 1, 1 }; /* for simplicity */
    const float expected_loss = get_loss(ssim_expected);
    float loss;

    /* ref -> src (if needed) */
    if (src->format != src_fmt) {
        av_frame_unref(src);
        av_frame_copy_props(src, ref);
        src->width  = ref->width;
        src->height = ref->height;
        src->format = src_fmt;
        ret = sws_scale_frame_wrapper(sws_ref_src, src, ref);
        if (ret < 0)
            goto error;
    }

    /* src -> dst */
    ret = initialize_frame(&dst, ref, dst_w, dst_h, dst_fmt);
    if (ret < 0)
        goto error;

    if (opts->legacy) {
        ret = init_legacy_context(dst, src, mode, opts);
        if (ret < 0)
            goto error;
    } else {
        sws_src_dst->flags  = mode->flags;
        sws_src_dst->dither = mode->dither;
        sws_src_dst->threads = opts->threads;
    }

    time = av_gettime_relative();
    for (int i = 0; ret >= 0 && i < opts->iters; i++)
        ret = sws_scale_frame_wrapper(sws_src_dst, dst, src);
    time = av_gettime_relative() - time;
    if (ret < 0)
        goto error;

    /* dst -> out */
    ret = initialize_frame(&out, ref, ref->width, ref->height, ref->format);
    if (ret < 0)
        goto error;

    ret = sws_scale_frame_wrapper(sws_dst_out, out, dst);
    if (ret < 0)
        goto error;

    get_ssim(ssim, out, ref, comps);

    if (opts->legacy) {
        /* Legacy swscale does not perform bit accurate upconversions of low
         * bit depth RGB. This artificially improves the SSIM score because the
         * resulting error deletes some of the input dither noise. This gives
         * it an unfair advantage when compared against a bit exact reference.
         * Work around this by ensuring that the reference SSIM score is not
         * higher than it theoretically "should" be. */
        if (src_var > dst_var) {
            const float src_loss = (2 * ref_var + c1) / (2 * ref_var + src_var + c1);
            ssim[0] = FFMIN(ssim[0], src_loss);
        }
    }

    loss = get_loss(ssim);

    ret = 0; /* fall through */

    print_test(src_fmt, dst_fmt,
               ref, src, dst, out,
               dst_w, dst_h,
               mode, opts,
               ssim,
               loss_ref, expected_loss, loss,
               time_ref, time);

 error:
    av_frame_free(&dst);
    av_frame_free(&out);
    return ret;
}

static inline int fmt_is_subsampled(enum AVPixelFormat fmt)
{
    return av_pix_fmt_desc_get(fmt)->log2_chroma_w != 0 ||
           av_pix_fmt_desc_get(fmt)->log2_chroma_h != 0;
}

static int run_self_tests(const AVFrame *ref, const struct options *opts)
{
    const int dst_w[] = { opts->w, opts->w - opts->w / 3, opts->w + opts->w / 3 };
    const int dst_h[] = { opts->h, opts->h - opts->h / 3, opts->h + opts->h / 3 };

    enum AVPixelFormat src_fmt, dst_fmt,
                       src_fmt_min = 0,
                       dst_fmt_min = 0,
                       src_fmt_max = AV_PIX_FMT_NB - 1,
                       dst_fmt_max = AV_PIX_FMT_NB - 1;

    AVFrame *src = av_frame_alloc();
    if (!src)
        return AVERROR(ENOMEM);

    int ret = 0;

    if (opts->src_fmt != AV_PIX_FMT_NONE)
        src_fmt_min = src_fmt_max = opts->src_fmt;
    if (opts->dst_fmt != AV_PIX_FMT_NONE)
        dst_fmt_min = dst_fmt_max = opts->dst_fmt;

    for (src_fmt = src_fmt_min; src_fmt <= src_fmt_max; src_fmt++) {
        if (opts->unscaled && fmt_is_subsampled(src_fmt))
            continue;
        if (!sws_test_format(src_fmt, 0) || !sws_test_format(src_fmt, 1))
            continue;
        for (dst_fmt = dst_fmt_min; dst_fmt <= dst_fmt_max; dst_fmt++) {
            if (opts->unscaled && fmt_is_subsampled(dst_fmt))
                continue;
            if (!sws_test_format(dst_fmt, 0) || !sws_test_format(dst_fmt, 1))
                continue;
            for (int h = 0; h < FF_ARRAY_ELEMS(dst_h); h++) {
                for (int w = 0; w < FF_ARRAY_ELEMS(dst_w); w++) {
                    for (int f = 0; f < FF_ARRAY_ELEMS(flags); f++) {
                        struct mode mode = {
                            .flags  = opts->flags  >= 0 ? opts->flags  : flags[f],
                            .dither = opts->dither >= 0 ? opts->dither : SWS_DITHER_AUTO,
                        };

                        if (ff_sfc64_get(&prng_state) > UINT64_MAX * opts->prob)
                            continue;

                        ret = run_test(src_fmt, dst_fmt, dst_w[w], dst_h[h],
                                       &mode, opts, ref, src, NAN, 0);
                        if (ret < 0)
                            goto error;

                        if (opts->flags >= 0 || opts->unscaled)
                            break;
                    }
                    if (opts->unscaled)
                        break;
                }
                if (opts->unscaled)
                    break;
            }
        }
    }

    ret = 0;

error:
    av_frame_free(&src);
    return ret;
}

static int run_file_tests(const AVFrame *ref, FILE *fp, const struct options *opts)
{
    char buf[256];
    int ret = 0;

    AVFrame *src = av_frame_alloc();
    if (!src)
        return AVERROR(ENOMEM);

    while (fgets(buf, sizeof(buf), fp)) {
        char src_fmt_str[21], dst_fmt_str[21];
        enum AVPixelFormat src_fmt;
        enum AVPixelFormat dst_fmt;
        int sw, sh, dw, dh;
        float fdummy;
        float loss;
        int64_t time;
        struct mode mode;

        ret = sscanf(buf,
                     "%20s %dx%d -> %20s %dx%d, flags=0x%x dither=%u "
                     "SSIM={Y=%f U=%f V=%f A=%f} loss=%e time=%6"PRId64"\n",
                     src_fmt_str, &sw, &sh, dst_fmt_str, &dw, &dh,
                     &mode.flags, &mode.dither,
                     &fdummy, &fdummy, &fdummy, &fdummy, &loss, &time);
        if (ret != 13 && ret != 14) {
            printf("[%d] %s", ret, buf);
            continue;
        }

        src_fmt = av_get_pix_fmt(src_fmt_str);
        dst_fmt = av_get_pix_fmt(dst_fmt_str);
        if (src_fmt == AV_PIX_FMT_NONE || dst_fmt == AV_PIX_FMT_NONE ||
            sw != ref->width || sh != ref->height || dw > 8192 || dh > 8192 ||
            mode.dither >= SWS_DITHER_NB) {
            av_log(NULL, AV_LOG_FATAL, "malformed input file\n");
            goto error;
        }

        if (opts->src_fmt != AV_PIX_FMT_NONE && src_fmt != opts->src_fmt ||
            opts->dst_fmt != AV_PIX_FMT_NONE && dst_fmt != opts->dst_fmt)
            continue;

        ret = run_test(src_fmt, dst_fmt, dw, dh, &mode, opts, ref, src, loss, time);
        if (ret < 0)
            goto error;
    }

    ret = 0;

error:
    av_frame_free(&src);
    return ret;
}

static int initialize_reference_frame(AVFrame *ref, const struct options *opts)
{
    SwsContext *ctx = sws_alloc_context();
    AVFrame *rgb = av_frame_alloc();
    AVLFG rand;
    int ret = -1;

    if (!ctx || !rgb)
        goto error;

    rgb->width  = opts->w / 12;
    rgb->height = opts->h / 12;
    rgb->format = AV_PIX_FMT_RGBA;
    if (av_frame_get_buffer(rgb, 32) < 0)
        goto error;

    av_lfg_init(&rand, 1);
    for (int y = 0; y < rgb->height; y++) {
        for (int x = 0; x < rgb->width; x++) {
            for (int c = 0; c < 4; c++)
                rgb->data[0][y * rgb->linesize[0] + x * 4 + c] = av_lfg_get(&rand);
        }
    }

    ctx->flags = SWS_BILINEAR | SWS_BITEXACT;
    ret = sws_scale_frame(ctx, ref, rgb);

error:
    sws_free_context(&ctx);
    av_frame_free(&rgb);
    return ret;
}

static int parse_options(int argc, char **argv, struct options *opts, FILE **fp)
{
    int ret = -1;

    for (int i = 1; i < argc; i += 2) {
        if (!strcmp(argv[i], "-help") || !strcmp(argv[i], "--help")) {
            fprintf(stderr,
                    "swscale [options...]\n"
                    "   -help\n"
                    "       This text\n"
                    "   -ref <file>\n"
                    "       Uses file as reference to compare tests againsts. Tests that have become worse will contain the string worse or WORSE\n"
                    "   -p <number between 0.0 and 1.0>\n"
                    "       The percentage of tests or comparisons to perform. Doing all tests will take long and generate over a hundred MB text output\n"
                    "       It is often convenient to perform a random subset\n"
                    "   -dst <pixfmt>\n"
                    "       Only test the specified destination pixel format\n"
                    "   -src <pixfmt>\n"
                    "       Only test the specified source pixel format\n"
                    "   -bench <iters>\n"
                    "       Run benchmarks with the specified number of iterations. This mode also increases the size of the test images\n"
                    "   -flags <flags>\n"
                    "       Test with a specific combination of flags\n"
                    "   -dither <mode>\n"
                    "       Test with a specific dither mode\n"
                    "   -unscaled <1 or 0>\n"
                    "       If 1, test only conversions that do not involve scaling\n"
                    "   -legacy <1 or 0>\n"
                    "       If 1, force using legacy swscale\n"
                    "   -threads <threads>\n"
                    "       Use the specified number of threads\n"
                    "   -cpuflags <cpuflags>\n"
                    "       Uses the specified cpuflags in the tests\n"
                    "   -v <level>\n"
                    "       Enable log verbosity at given level\n"
            );
            return 0;
        }
        if (argv[i][0] != '-' || i + 1 == argc)
            goto bad_option;
        if (!strcmp(argv[i], "-ref")) {
            *fp = fopen(argv[i + 1], "r");
            if (!*fp) {
                fprintf(stderr, "could not open '%s'\n", argv[i + 1]);
                goto error;
            }
        } else if (!strcmp(argv[i], "-cpuflags")) {
            unsigned flags = av_get_cpu_flags();
            int res = av_parse_cpu_caps(&flags, argv[i + 1]);
            if (res < 0) {
                fprintf(stderr, "invalid cpu flags %s\n", argv[i + 1]);
                goto error;
            }
            av_force_cpu_flags(flags);
        } else if (!strcmp(argv[i], "-src")) {
            opts->src_fmt = av_get_pix_fmt(argv[i + 1]);
            if (opts->src_fmt == AV_PIX_FMT_NONE) {
                fprintf(stderr, "invalid pixel format %s\n", argv[i + 1]);
                goto error;
            }
        } else if (!strcmp(argv[i], "-dst")) {
            opts->dst_fmt = av_get_pix_fmt(argv[i + 1]);
            if (opts->dst_fmt == AV_PIX_FMT_NONE) {
                fprintf(stderr, "invalid pixel format %s\n", argv[i + 1]);
                goto error;
            }
        } else if (!strcmp(argv[i], "-bench")) {
            opts->bench = 1;
            opts->iters = atoi(argv[i + 1]);
            opts->iters = FFMAX(opts->iters, 1);
            opts->w = 1920;
            opts->h = 1080;
        } else if (!strcmp(argv[i], "-flags")) {
            SwsContext *dummy = sws_alloc_context();
            const AVOption *flags_opt = av_opt_find(dummy, "sws_flags", NULL, 0, 0);
            ret = av_opt_eval_flags(dummy, flags_opt, argv[i + 1], &opts->flags);
            sws_free_context(&dummy);
            if (ret < 0) {
                fprintf(stderr, "invalid flags %s\n", argv[i + 1]);
                goto error;
            }
        } else if (!strcmp(argv[i], "-dither")) {
            opts->dither = atoi(argv[i + 1]);
        } else if (!strcmp(argv[i], "-unscaled")) {
            opts->unscaled = atoi(argv[i + 1]);
        } else if (!strcmp(argv[i], "-legacy")) {
            opts->legacy = atoi(argv[i + 1]);
        } else if (!strcmp(argv[i], "-threads")) {
            opts->threads = atoi(argv[i + 1]);
        } else if (!strcmp(argv[i], "-p")) {
            opts->prob = atof(argv[i + 1]);
        } else if (!strcmp(argv[i], "-v")) {
            av_log_set_level(atoi(argv[i + 1]));
        } else {
bad_option:
            fprintf(stderr, "bad option or argument missing (%s) see -help\n", argv[i]);
            goto error;
        }
    }

    ret = 0;

error:
    return ret;
}

int main(int argc, char **argv)
{
    struct options opts = {
        .src_fmt = AV_PIX_FMT_NONE,
        .dst_fmt = AV_PIX_FMT_NONE,
        .w       = 96,
        .h       = 96,
        .threads = 1,
        .iters   = 1,
        .prob    = 1.0,
        .flags   = -1,
        .dither  = -1,
    };

    AVFrame *ref = NULL;
    FILE *fp = NULL;
    AVLFG rand;
    int ret = -1;

    if (parse_options(argc, argv, &opts, &fp) < 0)
        goto error;

    ff_sfc64_init(&prng_state, 0, 0, 0, 12);
    av_lfg_init(&rand, 1);
    signal(SIGINT, exit_handler);

    sws_ref_src = sws_alloc_context();
    sws_src_dst = sws_alloc_context();
    sws_dst_out = sws_alloc_context();
    if (!sws_ref_src || !sws_src_dst || !sws_dst_out)
        goto error;
    sws_ref_src->flags = SWS_BILINEAR | SWS_BITEXACT;
    sws_dst_out->flags = SWS_BILINEAR | SWS_BITEXACT;

    ref = av_frame_alloc();
    if (!ref)
        goto error;
    ref->width  = opts.w;
    ref->height = opts.h;
    ref->format = AV_PIX_FMT_YUVA444P;

    if (initialize_reference_frame(ref, &opts) < 0)
        goto error;

    ret = fp ? run_file_tests(ref, fp, &opts)
             : run_self_tests(ref, &opts);

    /* fall through */
error:
    sws_free_context(&sws_ref_src);
    sws_free_context(&sws_src_dst);
    sws_free_context(&sws_dst_out);
    av_frame_free(&ref);
    if (fp)
        fclose(fp);
    exit_handler(ret);
}
