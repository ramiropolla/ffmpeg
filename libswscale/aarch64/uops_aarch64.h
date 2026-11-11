/**
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

#ifndef SWSCALE_AARCH64_UOPS_AARCH64_H
#define SWSCALE_AARCH64_UOPS_AARCH64_H

/**
 * Translation policy of the aarch64 backend, shared with the uop list
 * generator. This must not depend on the host architecture.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

#include "libavutil/attributes.h"
#include "libavutil/error.h"

#include "../swscale.h"
#include "../uops.h"
#include "ops_impl.h"

static inline SwsUOpFlags ff_sws_uop_flags_aarch64(bool bitexact)
{
    return SWS_UOP_FLAG_PUNNING | SWS_UOP_FLAG_ADD |
           (bitexact ? 0 : SWS_UOP_FLAG_FMA);
}

/* Use at most two full vregs during the widest precision section */
static inline int ff_sws_uop_block_size_aarch64(const SwsUOpList *uops)
{
    return uops->pixel_size_max == 4 ? 8 : 16;
}

/**
 * Rewrite a uop of a chain into the form of the kernel implementing it.
 * uop->block_size must be set to the block size of the chain, and is
 * adjusted to match the kernel. Parameters which make no difference to the
 * generated code are normalized, to prevent identical CPS functions from
 * being instantiated multiple times under different names.
 *
 * Returns 0, or AVERROR(ENOTSUP) if the uop has no implementation.
 */
static inline int ff_sws_uop_simplify_aarch64(SwsUOp *uop)
{
    switch (uop->uop) {
    case SWS_UOP_READ_PLANAR_FH:
    case SWS_UOP_READ_PLANAR_FV:
    case SWS_UOP_READ_PLANAR_FV_FMA:
    case SWS_UOP_READ_PALETTE:
    case SWS_UOP_RW_SHUFFLE:
    case SWS_UOP_EXPAND_BIT:
    case SWS_UOP_LUT_3D:
        return AVERROR(ENOTSUP);
    case SWS_UOP_PERMUTE:
    case SWS_UOP_COPY:
        /* Recompute mask taking identity swizzle into account */
        uop->mask = 0;
        for (int i = 0; i < uop->par.move.num_moves; i++) {
            int dst = uop->par.move.dst[i];
            if (dst >= 0)
                uop->mask |= SWS_COMP(dst);
        }

        /* The element size and type don't matter. */
        uop->block_size *= ff_sws_pixel_type_size(uop->type);
        uop->type = SWS_PIXEL_U8;
        break;
    case SWS_UOP_LINEAR_FMA:
        /* par.lin.exact is currently unused by asmgen_op_linear(). */
        uop->par.lin.exact = 0;
        break;
    case SWS_UOP_DITHER:
        /* asmgen_op_dither() reads whole blocks from each matrix row */
        if ((1 << uop->par.dither.size_log2) < uop->block_size)
            return AVERROR(ENOTSUP);
        break;
    default:
        break;
    }

    return 0;
}

/* Helpers for kernel names; each nibble or bit pair describes one element */
static inline uint16_t aarch64_clear_mask(const SwsClearUOp *clear)
{
    uint16_t mask = 0;
    for (int i = 0; i < 4; i++) {
        if (clear->zero & SWS_COMP(i)) {
            /* no-op */
        } else if (clear->one & SWS_COMP(i)) {
            NIBBLE_SET(mask, i, 1);
        } else {
            NIBBLE_SET(mask, i, 0xf);
        }
    }
    return mask;
}

static inline uint64_t aarch64_move_mask(const SwsMoveUOp *move)
{
    uint64_t mask = 0;
    for (int i = 0; i < move->num_moves; i++) {
        uint8_t dst = move->dst[i] < 0 ? 0xf : move->dst[i];
        uint8_t src = move->src[i] < 0 ? 0xf : move->src[i];
        uint64_t pair = src | (dst << 4);
        mask |= pair << (i * 8);
    }
    return mask;
}

static inline uint16_t aarch64_pack_mask(const SwsPackUOp *pack)
{
    uint16_t mask = 0;
    for (int i = 0; i < 4; i++)
        NIBBLE_SET(mask, i, pack->pattern[i]);
    return mask;
}

static inline uint64_t aarch64_linear_mask(const SwsLinearUOp *linear)
{
    uint64_t mask = 0;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 5; j++) {
            int jj = (j == 0) ? 4 : (j - 1);
            if (linear->one & SWS_MASK(i, jj))
                mask |= 1ULL << (2 * ((5 * i + j)));
            else if (!(linear->zero & SWS_MASK(i, jj)))
                mask |= 3ULL << (2 * ((5 * i + j)));
        }
    }
    return mask;
}

static inline uint16_t aarch64_dither_mask(SwsCompMask comps,
                                           const SwsDitherUOp *dither)
{
    uint16_t mask = 0;
    for (int i = 0; i < 4; i++) {
        if (comps & SWS_COMP(i)) {
            NIBBLE_SET(mask, i, dither->y_offset[i]);
        } else {
            NIBBLE_SET(mask, i, 0xf);
        }
    }
    return mask;
}

/* Name of the exported function implementing a simplified uop */
static inline void ff_sws_uop_name_aarch64(const SwsUOp *uop,
                                           char buf[SWS_UOP_NAME_MAX])
{
    static const char uop_names[SWS_UOP_TYPE_NB][16] = {
        [SWS_UOP_READ_BIT]     = "read_bit",
        [SWS_UOP_READ_NIBBLE]  = "read_nibble",
        [SWS_UOP_READ_PACKED]  = "read_packed",
        [SWS_UOP_READ_PLANAR]  = "read_planar",
        [SWS_UOP_WRITE_BIT]    = "write_bit",
        [SWS_UOP_WRITE_NIBBLE] = "write_nibble",
        [SWS_UOP_WRITE_PACKED] = "write_packed",
        [SWS_UOP_WRITE_PLANAR] = "write_planar",
        [SWS_UOP_SWAP_BYTES]   = "swap_bytes",
        [SWS_UOP_PERMUTE]      = "permute",
        [SWS_UOP_COPY]         = "copy",
        [SWS_UOP_UNPACK]       = "unpack",
        [SWS_UOP_PACK]         = "pack",
        [SWS_UOP_LSHIFT]       = "lshift",
        [SWS_UOP_RSHIFT]       = "rshift",
        [SWS_UOP_CLEAR]        = "clear",
        [SWS_UOP_TO_U8]        = "to_u8",
        [SWS_UOP_TO_U16]       = "to_u16",
        [SWS_UOP_TO_U32]       = "to_u32",
        [SWS_UOP_TO_F32]       = "to_f32",
        [SWS_UOP_EXPAND_PAIR]  = "expand_pair",
        [SWS_UOP_EXPAND_QUAD]  = "expand_quad",
        [SWS_UOP_ADD]          = "add",
        [SWS_UOP_MIN]          = "min",
        [SWS_UOP_MAX]          = "max",
        [SWS_UOP_SCALE]        = "scale",
        [SWS_UOP_LINEAR]       = "linear",
        [SWS_UOP_LINEAR_FMA]   = "linear_fma",
        [SWS_UOP_DITHER]       = "dither",
    };

    static const char type_names[SWS_PIXEL_TYPE_NB][4] = {
        [SWS_PIXEL_U8 ] = "u8",
        [SWS_PIXEL_U16] = "u16",
        [SWS_PIXEL_U32] = "u32",
        [SWS_PIXEL_F32] = "f32",
    };

    const SwsUOpParams *par = &uop->par;
    char suffix[24] = "";
    switch (uop->uop) {
    case SWS_UOP_PERMUTE:
    case SWS_UOP_COPY:
        snprintf(suffix, sizeof(suffix), "_%012" PRIx64, aarch64_move_mask(&par->move));
        break;
    case SWS_UOP_UNPACK:
    case SWS_UOP_PACK:
        snprintf(suffix, sizeof(suffix), "_%04x", aarch64_pack_mask(&par->pack));
        break;
    case SWS_UOP_LSHIFT:
    case SWS_UOP_RSHIFT:
        snprintf(suffix, sizeof(suffix), "_%u", par->shift.amount);
        break;
    case SWS_UOP_CLEAR:
        snprintf(suffix, sizeof(suffix), "_%04x", aarch64_clear_mask(&par->clear));
        break;
    case SWS_UOP_LINEAR:
    case SWS_UOP_LINEAR_FMA:
        snprintf(suffix, sizeof(suffix), "_%010" PRIx64, aarch64_linear_mask(&par->lin));
        break;
    case SWS_UOP_DITHER:
        snprintf(suffix, sizeof(suffix), "_%04x_%u",
                 aarch64_dither_mask(uop->mask, &par->dither), par->dither.size_log2);
        break;
    default:
        break;
    }

    snprintf(buf, SWS_UOP_NAME_MAX, "ff_sws_%s%s_%d_%s_%04x_neon",
             uop_names[uop->uop], suffix, uop->block_size,
             type_names[uop->type], nibble_mask(uop->mask));
}

static inline SwsUOpFlags aarch64_policy_uop_flags(const SwsUOpTarget *target)
{
    return ff_sws_uop_flags_aarch64(target->flags & SWS_BITEXACT);
}

static inline int aarch64_policy_block_size(const SwsUOpTarget *target,
                                            const SwsUOpList *uops)
{
    return ff_sws_uop_block_size_aarch64(uops);
}

static inline int aarch64_policy_simplify(const SwsUOpTarget *target,
                                          SwsUOp *uop)
{
    return ff_sws_uop_simplify_aarch64(uop);
}

av_unused static const SwsUOpPolicy ff_sws_uop_policy_aarch64 = {
    .uop_flags  = aarch64_policy_uop_flags,
    .block_size = aarch64_policy_block_size,
    .simplify   = aarch64_policy_simplify,
};

#endif /* SWSCALE_AARCH64_UOPS_AARCH64_H */
