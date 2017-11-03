/*
 * VC-1 and WMV3 encoder
 * copyright (c) 2007 Denis Fortin
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

#include "avcodec.h"
#include "msmpeg4data.h"
#include "vc1.h"
#include "vc1data.h"
#include "vc1enc.h"

/* msmpeg4 functions */
void ff_msmpeg4_encode_block(MpegEncContext * s, int16_t * block, int n);
void ff_msmpeg4_find_best_tables(MpegEncContext * s);
void ff_msmpeg4_code012(PutBitContext *pb, int n);

/**
 * Unquantize a block
 *
 * @param s Encoder context
 * @param block Block to quantize
 * @param n index of block
 * @param qscale quantizer scale
 */
static void vc1_unquantize_c(MpegEncContext *s, int16_t *block, int n, int qscale)
{
    VC1Context * const v = s->avctx->priv_data;
    int i, level, nCoeffs, q;
    ScanTable scantable;

    if ( s->pict_type == AV_PICTURE_TYPE_I )
    {
        scantable = s->intra_scantable;
    }
    else
    {
        scantable = s->inter_scantable;
        if ( s->pict_type == AV_PICTURE_TYPE_P )
            for ( i = 0; i < 64; i++ )
                block[i] += 128;
    }

    nCoeffs = s->block_last_index[n];

    if ( n < 4 )
        block[0] *= s->y_dc_scale;
    else
        block[0] *= s->c_dc_scale;

    q = 2 * qscale + v->halfpq;

    for ( i = 1; i <= nCoeffs; i++ )
    {
        int j = scantable.permutated[i];
        level = block[j];
        if ( level )
            level = level * q + v->pquantizer*(FFSIGN(block[j]) * qscale);
        block[j] = level;
    }

    for ( ; i < 64; i++ )
    {
        int j = scantable.permutated[i];
        block[j] = 0;
    }
}


/**
 * Transform and quantize a block
 *
 * @param s Encoder Context
 * @param block block to encode
 * @param n block index
 * @param qscale quantizer scale
 * @param overflow
 *
 * @return last significant coeff in zz order
 */
static int vc1_quantize_c(MpegEncContext *s, int16_t *block, int n, int qscale, int *overflow)
{
    VC1Context * const v= s->avctx->priv_data;
    const uint8_t *scantable;
    int q, i, j, level, last_non_zero, start_i;

    if (s->pict_type == AV_PICTURE_TYPE_I)
    {
        scantable = s->intra_scantable.scantable;
        last_non_zero = 0;
        start_i = 1;
    }
    else
    {
        scantable = s->inter_scantable.scantable;
        last_non_zero = -1;
        start_i = 0;
        if (s->mb_intra){
            for(i=0;i<64;i++)
                block[i] -= 128;
        }
    }

    v->vc1dsp.vc1_fwd_trans_8x8(block);

    if (n < 4)
        q = s->y_dc_scale;
    else
        q = s->c_dc_scale;

    block[0] /= q;
    q = 2 * qscale + v->halfpq;

    for(i=63;i>=start_i;i--) {
        j = scantable[i];
        level =  (block[j] - v->pquantizer*(FFSIGN(block[j]) * qscale)) / q;
        if (level) {
            last_non_zero = i;
            break;
        }
    }
    for(i=start_i; i<=last_non_zero; i++) {
        j = scantable[i];
        block[j] =  (block[j] - v->pquantizer*(FFSIGN(block[j]) * qscale)) / q ;
    }
    *overflow = 0;
    return last_non_zero;
}


/**
 * Intra picture MB layer bitstream encoder
 * @param s Mpeg encoder context
 * @param block macroblock to encode
 */
static void vc1_intra_picture_encode_mb(MpegEncContext * s, int16_t block[6][64])
{
    int cbp, coded_cbp, i;
    uint8_t *coded_block;

    /* compute cbp */
    cbp = 0;
    coded_cbp = 0;
    for (i = 0; i < 6; i++) {
        int val, pred;
        val = (s->block_last_index[i] >= 1);
        cbp |= val << (5 - i);
        if (i < 4) {
            /* predict value for close blocks only for luma */
            pred = vc1_coded_block_pred(s, i, &coded_block);
            *coded_block = val;
            val = val ^ pred;
        }
        coded_cbp |= val << (5 - i);
    }
    put_bits(&s->pb,ff_msmp4_mb_i_table[coded_cbp][1],
             ff_msmp4_mb_i_table[coded_cbp][0]);//CBPCY

    // predict dc_val and dc_direction for each block

    // brute force test to switch ACPRED on/off
    put_bits(&s->pb, 1, 0); //ACPRED

    for ( i = 0; i < 6; i++ )
        ff_msmpeg4_encode_block(s, block[i], i);

    s->i_tex_bits += get_bits_diff(s);
    s->i_count++;
}

/**
 * MB layer bitstream encoder
 * @param s Mpeg encoder context
 * @param block macroblock to encode
 * @param motion_x x component of mv's macroblock
 * @param motion_y y component of mv's macroblock
 */
void ff_vc1_encode_mb(MpegEncContext * s, int16_t block[6][64],
                      int motion_x, int motion_y)
{
    if ( s->pict_type == AV_PICTURE_TYPE_I )
        vc1_intra_picture_encode_mb(s, block);
}

/**
 * Progressive I picture layer bitstream encoder for Simple and Main Profile
 * @param s Mpeg encoder context
 * @param picture_number number of the frame to encode
 */
static void vc1_encode_i_sm_picture_header(MpegEncContext * s, int picture_number)
{
    VC1Context * const v= s->avctx->priv_data;
    if (v->finterpflag)
    {
        v->interpfrm = 0; // INTERPFRM
        put_bits(&s->pb, 1, v->interpfrm);
    }

    put_bits(&s->pb,2,picture_number);//FRMCNT

    if ( v->rangered )
    {
        v->rangeredfrm = 0;//RANGEREDFRM
        put_bits(&s->pb,1,v->rangeredfrm);
    }

    put_bits(&s->pb,1,s->pict_type-1);//PTYPE

    put_bits(&s->pb,7,50);//BF

    v->pquantizer = 1;//always use non uniform quantizer

    v->halfpq = (s->qscale > 8) ? 0 : 1;

    if ( v->quantizer_mode == QUANT_FRAME_IMPLICIT )
    {
        // TODO create table
        // put_bits(&s->pb,5,v->pqindex);//PQINDEX
    }
    else
    {
        v->pqindex = s->qscale;
        put_bits(&s->pb,5,v->pqindex);//PQINDEX
    }

    if ( v->pqindex <= 8 )
        put_bits(&s->pb,1,v->halfpq);//HALFQP

    if ( v->quantizer_mode == QUANT_FRAME_EXPLICIT )
        put_bits(&s->pb,1,v->pquantizer);//PQUANTIZER : NON_UNIFOMR 0 / UNIFORM 1

    if ( v->extended_mv )
    {
        v->mvrange = 0;
        put_bits(&s->pb,1,v->mvrange); // TODO fix this: num bits is not fixed
    }

    if ( v->multires )
    {
        v->respic = 0;
        put_bits(&s->pb,2,v->respic);
    }

    if ( v->pqindex<=8 )
    {
        ff_msmpeg4_code012(&s->pb, s->rl_chroma_table_index%3);//TRANSACFRM (UV)
        ff_msmpeg4_code012(&s->pb, s->rl_table_index%3); //TRANSACFRM2 (Y)
    } else {
        ff_msmpeg4_code012(&s->pb, s->rl_chroma_table_index);//TRANSACFRM (UV)
        ff_msmpeg4_code012(&s->pb, s->rl_table_index); //TRANSACFRM2 (Y)
    }

    put_bits(&s->pb, 1, s->dc_table_index);//TRANSDCTAB
}



/**
 * Picture layer bitstream encoder
 * @param s Mpeg encoder context
 * @param picture_number number of the frame to encode
 */
void ff_vc1_encode_picture_header(MpegEncContext * s, int picture_number)
{
    ff_msmpeg4_find_best_tables(s);

    if ( s->pict_type == AV_PICTURE_TYPE_I )
        vc1_encode_i_sm_picture_header(s, picture_number) ;

    s->esc3_level_length = 0;
    s->esc3_run_length = 0;
}


/**
 * Sequence layer bitstream encoder
 * @param v VC1 context
 */
static void vc1_encode_ext_header(VC1Context *v)
{
    MpegEncContext * const s= &v->s;
    PutBitContext pb;

    init_put_bits(&pb, s->avctx->extradata, s->avctx->extradata_size);

    v->profile = PROFILE_MAIN;
    put_bits(&pb, 2, v->profile); //Profile
    if ( v->profile == PROFILE_ADVANCED )
    {
        v->level = 2;
        put_bits(&pb, 3, v->level); //Level
        v->chromaformat = 1; //4:2:0
        put_bits(&pb, 2, v->chromaformat);
    }
    else
    {
        put_bits(&pb, 2, 0); //Unused
    }

    put_bits(&pb, 3, 0); // TODO: Q frame rate
    put_bits(&pb, 5, 0); // TODO: Q bit rate

    s->loop_filter = 0; // TODO: loop_filter
    put_bits(&pb, 1, s->loop_filter);

    if (v->profile < PROFILE_ADVANCED)
    {
        put_bits(&pb, 1, 0); // Reserved
        put_bits(&pb, 1, 0); // Multires
        put_bits(&pb, 1, 1); // Reserved
    }

    v->fastuvmc = 0; // TODO: FAST U/V MC
    put_bits(&pb, 1, v->fastuvmc);

    v->extended_mv = 0; // TODO: Extended MV
    put_bits(&pb, 1, v->extended_mv);

    v->dquant = 0; // TODO: MB dequant
    put_bits(&pb, 2, v->dquant);

    v->vstransform = 0; // TODO: Variable size transform
    put_bits(&pb, 1, v->vstransform);

    if ( v->profile < PROFILE_ADVANCED )
        put_bits(&pb, 1, 0); //Reserved

    v->overlap = 0; // TODO: Overlap
    put_bits(&pb, 1, v->overlap);

    if ( v->profile < PROFILE_ADVANCED )
    {
        v->resync_marker = 0; // TODO: Resync marker
        put_bits(&pb, 1, v->resync_marker);
        v->rangered = 0;// TODO: Range red
        put_bits(&pb, 1, v->rangered);
    }

    put_bits(&pb, 3, 0); // Max B-frames

    v->quantizer_mode = QUANT_FRAME_EXPLICIT;
    put_bits(&pb, 2, v->quantizer_mode); // Quantizer

    if ( v->profile < PROFILE_ADVANCED )
    {
        v->finterpflag = 0; // TODO: Frame interpol
        put_bits(&pb, 1, v->finterpflag);
        put_bits(&pb, 1, 1); //Reserved
    }

    flush_put_bits(&pb);
}


static int vc1_encode_init(AVCodecContext *avctx)
{
    VC1Context * const v = avctx->priv_data;
    MpegEncContext *s = &v->s;

    ff_vc1dsp_init(&v->vc1dsp);

    avctx->idct_algo = FF_IDCT_VC1;

    if ( ff_mpv_encode_init(avctx) < 0 )
        return -1;

    avctx->extradata_size = 32;
    avctx->extradata = av_mallocz(avctx->extradata_size + 10);

    s->dct_quantize = vc1_quantize_c;
    s->dct_unquantize_intra = vc1_unquantize_c;
    s->dct_unquantize_inter = vc1_unquantize_c;
    s->idsp.idct_put = v->vc1dsp.idct_put;
    s->idsp.perm_type = v->vc1dsp.idct_perm;

    vc1_encode_ext_header(v);

    return 0;
}

static const AVClass wmv3_class = {
    .class_name = "wmv3 encoder",
    .item_name  = av_default_item_name,
    .option     = ff_mpv_generic_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_wmv3_encoder = {
    .name           = "wmv3",
    .long_name      = NULL_IF_CONFIG_SMALL("Windows Media Video 9"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_WMV3,
    .priv_data_size = sizeof(VC1Context),
    .priv_class     = &wmv3_class,
    .init           = vc1_encode_init,
    .encode2        = ff_mpv_encode_picture,
    .close          = ff_mpv_encode_end,
    .pix_fmts       = (const enum AVPixelFormat[]) { AV_PIX_FMT_YUV420P,
                                                     AV_PIX_FMT_NONE },
};
