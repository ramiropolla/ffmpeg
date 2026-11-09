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

#ifndef SWSCALE_X86_UOPS_X86_H
#define SWSCALE_X86_UOPS_X86_H

/**
 * Translation policy of the x86 backend, shared with the uop list generator.
 * This must not depend on the host architecture.
 */

#include <stdbool.h>

#include "libavutil/error.h"
#include "libavutil/macros.h"

#include "../uops.h"

/* Used both for the CPU level and for the ISA of a kernel */
typedef enum SwsX86ISA {
    SWS_X86_SSE4,
    SWS_X86_AVX2,
    SWS_X86_AVX512,
    SWS_X86_AVX512ICL,
    SWS_X86_ISA_NB,
} SwsX86ISA;

static inline SwsUOpFlags ff_sws_uop_flags_x86(bool fma)
{
    return SWS_UOP_FLAG_PSHUFB
         | SWS_UOP_FLAG_EXPAND_BIT
         | SWS_UOP_FLAG_READ_PALETTE
         | SWS_UOP_FLAG_ADD
         | SWS_UOP_FLAG_PUNNING
         | (fma ? SWS_UOP_FLAG_FMA : 0);
}

/* Vector register size in bytes */
static inline int ff_sws_mmsize_x86(SwsX86ISA isa)
{
    switch (isa) {
    case SWS_X86_SSE4: return 16;
    case SWS_X86_AVX2: return 32;
    default:           return 64;
    }
}

/**
 * Block size of a chain of uops. Uses at most two full YMM registers during
 * the widest precision section.
 */
static inline int ff_sws_uop_block_size_x86(SwsX86ISA level,
                                            const SwsUOpList *uops)
{
    return 2 * FFMIN(ff_sws_mmsize_x86(level), 32) / uops->pixel_size_max;
}

/* Size of the widest pixel type a uop keeps in vector registers */
static inline int ff_sws_uop_pixel_size_x86(const SwsUOp *uop)
{
    SwsPixelType out = uop->type;
    switch (uop->uop) {
    case SWS_UOP_TO_U8:       out = SWS_PIXEL_U8;  break;
    case SWS_UOP_TO_U16:      out = SWS_PIXEL_U16; break;
    case SWS_UOP_TO_U32:      out = SWS_PIXEL_U32; break;
    case SWS_UOP_TO_F32:      out = SWS_PIXEL_F32; break;
    case SWS_UOP_EXPAND_PAIR: out = SWS_PIXEL_U16; break;
    case SWS_UOP_EXPAND_QUAD: out = SWS_PIXEL_U32; break;
    case SWS_UOP_READ_PLANAR_FH:
    case SWS_UOP_READ_PLANAR_FV:
    case SWS_UOP_READ_PLANAR_FV_FMA:
        out = uop->par.filter.type;
        break;
    default:
        break;
    }

    return FFMAX(ff_sws_pixel_type_size(uop->type),
                 ff_sws_pixel_type_size(out));
}

/**
 * Rewrite a uop of a chain into the form of the kernel implementing it on
 * CPUs supporting `level`. uop->block_size must be set to the block size of
 * the chain, and is adjusted to match the kernel.
 *
 * Returns the ISA of the kernel, or AVERROR(ENOTSUP).
 */
static inline int ff_sws_uop_simplify_x86(SwsX86ISA level, SwsUOp *uop)
{
    switch (uop->uop) {
    case SWS_UOP_RW_SHUFFLE: /* only as a standalone uop */
    case SWS_UOP_LUT_3D:
        return AVERROR(ENOTSUP);
    case SWS_UOP_TO_F32:
    case SWS_UOP_TO_U32:
        /* The kernels use signed 32-bit conversions */
        if (uop->type == SWS_PIXEL_U32 || uop->type == SWS_PIXEL_F32)
            return AVERROR(ENOTSUP);
        break;
    case SWS_UOP_READ_PLANAR:
    case SWS_UOP_WRITE_PLANAR:
    case SWS_UOP_CLEAR:
        /* These only depend on the bit pattern of their data */
        uop->block_size *= ff_sws_pixel_type_size(uop->type);
        uop->type = SWS_PIXEL_U8;
        break;
    default:
        break;
    }

    /* Only 8-bit kernels have SSE4 versions, with one or two registers */
    const int pixel_size = ff_sws_uop_pixel_size_x86(uop);
    const int bytes = uop->block_size * pixel_size;
    if (level >= SWS_X86_AVX2 && (bytes == 32 || bytes == 64))
        return SWS_X86_AVX2;
    if (pixel_size == 1 && (bytes == 16 || bytes == 32))
        return SWS_X86_SSE4;
    return AVERROR(ENOTSUP);
}

/**
 * ISA of the kernel implementing a standalone SWS_UOP_RW_SHUFFLE on CPUs
 * supporting `level`. Shuffles which are not lane aligned need vpermb to
 * use vectors wider than XMM.
 */
static inline SwsX86ISA ff_sws_uop_shuffle_isa_x86(SwsX86ISA level,
                                                   const SwsUOp *uop)
{
    const SwsShuffleUOp *par = &uop->par.shuffle;
    const bool lane_aligned = par->read_size == par->write_size &&
                              16 % par->read_size == 0;
    if (!lane_aligned)
        return level == SWS_X86_AVX512ICL ? SWS_X86_AVX512ICL : SWS_X86_SSE4;
    return FFMIN(level, SWS_X86_AVX512);
}

#endif /* SWSCALE_X86_UOPS_X86_H */
