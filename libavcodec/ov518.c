/*
 * OV518 decoder
 *
 * Copyright (c) 2023 Ramiro Polla
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

#include "libavutil/intreadwrite.h"
#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "jpegtables.h"
#include "mjpegdec.h"
#include "simple_idct.h"

typedef struct {
    VLC vlc_dc_y;
    VLC vlc_dc_uv;
    VLC vlc_ac_y;
    VLC vlc_ac_uv;

    int last_dc[3];
    int16_t quant_matrix_y[32];
    int16_t quant_matrix_uv[32];

    uint8_t *clean;
    unsigned int clean_size;
    unsigned int clean_length;
} OV518Context;

/* Similar to ff_zigzag_direct, but for 8x4 blocks */
static const uint8_t ov518_scantable[32] = {
    0,   1,  8, 16,  9,  2,  3, 10,
    17, 24, 25, 18, 11,  4,  5, 12,
    19, 26, 27, 20, 13,  6,  7, 14,
    21, 28, 29, 22, 15, 23, 30, 31
};

/* The webcam sends a bunch of 8-byte sequences of zeros interspersed
 * with the actual data to be decoded. This function cleans up the raw
 * data by removing all such sequences of zeros. */
static int ov518_cleanup_raw_data(OV518Context *ctx, AVPacket *avpkt)
{
    uint64_t *clean;
    uint64_t *o_ptr;

    /* allocate enough memory for clean buffer */
    clean = av_fast_realloc(ctx->clean, &ctx->clean_size, avpkt->size);
    if (!clean)
        return AVERROR(ENOMEM);
    ctx->clean = (uint8_t *) clean;

    /* cleanup raw data */
    o_ptr = clean;
    for (uint64_t *i_ptr = (uint64_t *) avpkt->data,
         *i_ptr_end = (uint64_t *) (avpkt->data + avpkt->size);
         i_ptr < i_ptr_end;
         i_ptr++) {
        if (*i_ptr)
            *o_ptr++ = *i_ptr;
    }
    ctx->clean_length = (o_ptr - clean) << 3;

    return 0;
}

/* setup quantization matrices */
static int ov518_setup_quant_matrices(OV518Context *ctx)
{
    uint8_t *qmat_y;
    uint8_t *qmat_uv;

    if (ctx->clean_length < 64)
        return AVERROR_INVALIDDATA;

    qmat_y = ctx->clean + ctx->clean_length - 64;
    qmat_uv = qmat_y + 32;
    for (int i = 0; i < 32; i++) {
        ctx->quant_matrix_y[i] = (qmat_y[i] + 1);
        ctx->quant_matrix_uv[i] = (qmat_uv[i] + 1);
    }

    return 0;
}

static int ov518_decode_block(OV518Context *ctx, GetBitContext *gb, uint8_t *dst, int stride, int component)
{
    const VLC *vlc_dc = !component ? &ctx->vlc_dc_y : &ctx->vlc_dc_uv;
    const VLC *vlc_ac = !component ? &ctx->vlc_ac_y : &ctx->vlc_ac_uv;
    int16_t *quant_matrix = !component ? ctx->quant_matrix_y : ctx->quant_matrix_uv;
    int16_t block[8 * 4];
    int code;
    int nbits;
    int val;
    int pos;
    int j;

    memset(block, 0, sizeof(block));

    /* dc */
    code = get_vlc2(gb, vlc_dc->table, 9, 2);
    if (code < 0)
        return AVERROR_INVALIDDATA;
    nbits = (code & 0x0F);
    val = nbits ? get_xbits(gb, nbits) : 0;
    val += ctx->last_dc[component];
    ctx->last_dc[component] = val;
    block[0] = val * quant_matrix[0];

    /* ac */
    pos = 0;
    do {
        code = get_vlc2(gb, vlc_ac->table, 9, 2);
        if (code < 0)
            return AVERROR_INVALIDDATA;
        pos += (code >> 4);
        nbits = (code & 0x0F);
        if (nbits) {
            if (pos > 31)
                return AVERROR_INVALIDDATA;
            val = get_xbits(gb, nbits);
            j        = ov518_scantable[pos];
            block[j] = val * quant_matrix[pos];
        }
    } while (pos < 31);

    /* The first AC coefficient for Y planes is halved. Go figure... */
    if (!component)
        block[1] >>= 1;

    ff_simple_idct84_put(dst, stride, block);

    return 0;
}

static int ov518_decode_yuv420(AVCodecContext *avctx, AVFrame *frame)
{
    OV518Context *ctx = avctx->priv_data;
    int height = avctx->height;
    int width = avctx->width;
    int mb_h = (height + 7) >> 3;
    int mb_w = (width + 15) >> 4;
    uint8_t *y_ptr = frame->data[0];
    uint8_t *u_ptr = frame->data[1];
    uint8_t *v_ptr = frame->data[2];
    int y_stride = frame->linesize[0];
    int u_stride = frame->linesize[1];
    int v_stride = frame->linesize[2];
    GetBitContext gb;
    int ret;

    /* These values were determined empirically while adapting to
     * FFmpeg's IDCT. */
    ctx->last_dc[0] = 80;
    ctx->last_dc[1] = 40;
    ctx->last_dc[2] = 40;

    ret = init_get_bits8(&gb, ctx->clean + 8, ctx->clean_length - 8);
    if (ret < 0)
        return ret;

    for (int mb_y = 0; mb_y < mb_h; mb_y++) {
        for (int mb_x = 0; mb_x < mb_w; mb_x++) {
            if (ov518_decode_block(ctx, &gb, y_ptr + 8 * ((mb_x << 1) + 0), y_stride, 0) < 0)
                return AVERROR_INVALIDDATA;
            if (ov518_decode_block(ctx, &gb, y_ptr + 8 * ((mb_x << 1) + 1), y_stride, 0) < 0)
                return AVERROR_INVALIDDATA;
        }
        y_ptr += 4 * y_stride;
        for (int mb_x = 0; mb_x < mb_w; mb_x++) {
            if (ov518_decode_block(ctx, &gb, u_ptr + 8 * mb_x, u_stride, 1) < 0)
                return AVERROR_INVALIDDATA;
            if (ov518_decode_block(ctx, &gb, v_ptr + 8 * mb_x, v_stride, 2) < 0)
                return AVERROR_INVALIDDATA;
            if (ov518_decode_block(ctx, &gb, y_ptr + 8 * ((mb_x << 1) + 0), y_stride, 0) < 0)
                return AVERROR_INVALIDDATA;
            if (ov518_decode_block(ctx, &gb, y_ptr + 8 * ((mb_x << 1) + 1), y_stride, 0) < 0)
                return AVERROR_INVALIDDATA;
        }
        y_ptr += 4 * y_stride;
        u_ptr += 4 * u_stride;
        v_ptr += 4 * v_stride;
    }

    return 0;
}

static int ov518_decode_frame(AVCodecContext *avctx, AVFrame *frame,
                              int *got_frame, AVPacket *avpkt)
{
    OV518Context *ctx = avctx->priv_data;
    int ret;

    ret = ff_get_buffer(avctx, frame, 0);
    if (ret < 0)
        return ret;

    frame->pict_type = AV_PICTURE_TYPE_I;
    frame->flags |= AV_FRAME_FLAG_KEY;

    ret = ov518_cleanup_raw_data(ctx, avpkt);
    if (ret < 0)
        return ret;

    ret = ov518_setup_quant_matrices(ctx);
    if (ret < 0)
        return ret;

    ret = ov518_decode_yuv420(avctx, frame);
    if (ret < 0)
        return ret;

    *got_frame = 1;

    return avpkt->size;
}

static av_cold int ov518_decode_init(AVCodecContext *avctx)
{
    OV518Context *ctx = avctx->priv_data;
    struct {
        VLC *vlc;
        const uint8_t *bits;
        const uint8_t *values;
        int is_ac;
    } ht[] = {
        { &ctx->vlc_dc_y,  ff_mjpeg_bits_dc_luminance,   ff_mjpeg_val_dc,             0 },
        { &ctx->vlc_dc_uv, ff_mjpeg_bits_dc_chrominance, ff_mjpeg_val_dc,             0 },
        { &ctx->vlc_ac_y,  ff_mjpeg_bits_ac_luminance,   ff_mjpeg_val_ac_luminance,   1 },
        { &ctx->vlc_ac_uv, ff_mjpeg_bits_ac_chrominance, ff_mjpeg_val_ac_chrominance, 1 },
    };
    for (int i = 0; i < FF_ARRAY_ELEMS(ht); i++) {
        int ret = ff_mjpeg_build_vlc(ht[i].vlc, ht[i].bits, ht[i].values, ht[i].is_ac, avctx);
        if (ret < 0)
            return ret;
    }

    avctx->pix_fmt = AV_PIX_FMT_YUV420P;

    return 0;
}

static av_cold int ov518_decode_end(AVCodecContext *avctx)
{
    OV518Context *ctx = avctx->priv_data;

    ff_vlc_free(&ctx->vlc_dc_y);
    ff_vlc_free(&ctx->vlc_dc_uv);
    ff_vlc_free(&ctx->vlc_ac_y);
    ff_vlc_free(&ctx->vlc_ac_uv);

    av_freep(&ctx->clean);

    return 0;
}

const FFCodec ff_ov518_decoder = {
    .p.name         = "ov518",
    CODEC_LONG_NAME("OV518 video format"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_OV518,
    .priv_data_size = sizeof(OV518Context),
    .init           = ov518_decode_init,
    .close          = ov518_decode_end,
    FF_CODEC_DECODE_CB(ov518_decode_frame),
    .p.capabilities = AV_CODEC_CAP_DR1,
};
