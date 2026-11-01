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

#ifndef AVCODEC_AARCH64_CABAC_H
#define AVCODEC_AARCH64_CABAC_H

#include "config.h"
#if HAVE_INLINE_ASM

#include "libavutil/attributes.h"
#include "libavutil/internal.h"
#include "libavcodec/cabac.h"

#define get_cabac_inline get_cabac_inline_aarch64
static av_always_inline int get_cabac_inline_aarch64(CABACContext *c,
                                                     uint8_t *const state)
{
    int bit;
    uint64_t lps;
    uint64_t tmp;
    uint64_t shift;
    void *ptr;

    __asm__ volatile(
        "ldrb       %w[bit]       , [%[state]]                      \n\t"
        "and        %w[tmp]       , %w[range]   , #0xC0             \n\t"
        "add        %w[tmp]       , %w[bit]     , %w[tmp], lsl #1   \n\t"
        "ldrb       %w[lps]       , [%[lps_tab], %w[tmp], UXTW]     \n\t"
        "sub        %w[range]     , %w[range]   , %w[lps]           \n\t"
        "lsl        %w[tmp]       , %w[range]   , #17               \n\t"
        "subs       %w[tmp]       , %w[low]     , %w[tmp]           \n\t"
        "csel       %w[low]       , %w[tmp]     , %w[low]  , hi     \n\t"
        "csel       %w[range]     , %w[lps]     , %w[range], hi     \n\t"
        "cinv       %w[bit]       , %w[bit]     , hi                \n\t"
        "clz        %w[shift]     , %w[range]                       \n\t"
        "ldrb       %w[tmp]       , [%[mlps_tab], %w[bit], SXTW]    \n\t"
        "sub        %w[shift]     , %w[shift]   , #23               \n\t"
        "lsl        %w[low]       , %w[low]     , %w[shift]         \n\t"
        "lsl        %w[range]     , %w[range]   , %w[shift]         \n\t"
        "uxth       %w[shift]     , %w[low]                         \n\t"
        "strb       %w[tmp]       , [%[state]]                      \n\t"
        "cbnz       %w[shift]     , 2f                              \n\t"
        "ldr        %[ptr]        , [%[c], %[byte]]                 \n\t"
        "ldr        %[lps]        , [%[c], %[end]]                  \n\t"
        "ldrh       %w[tmp]       , [%[ptr]]                        \n\t"
        "cmp        %[ptr]        , %[lps]                          \n\t"
        "b.ge       1f                                              \n\t"
        "add        %[lps]        , %[ptr]      , #2                \n\t"
        "str        %[lps]        , [%[c], %[byte]]                 \n\t"
        "1:                                                         \n\t"
        "rbit       %w[shift]     , %w[low]                         \n\t"
        "rev        %w[tmp]       , %w[tmp]                         \n\t"
        "clz        %w[shift]     , %w[shift]                       \n\t"
        "mov        %w[lps]       , #-0xFFFF                        \n\t"
        "sub        %w[shift]     , %w[shift]   , #16               \n\t"
        "add        %w[tmp]       , %w[lps]     , %w[tmp], lsr #15  \n\t"
        "lsl        %w[tmp]       , %w[tmp]     , %w[shift]         \n\t"
        "add        %w[low]       , %w[low]     , %w[tmp]           \n\t"
        "2:                                                         \n\t"
        :    [bit]"=&r"(bit),
             [low]"+&r"(c->low),
           [range]"+&r"(c->range),
             [lps]"=&r"(lps),
             [ptr]"=&r"(ptr),
           [shift]"=&r"(shift),
             [tmp]"=&r"(tmp)
        :        [c]"r"(c),
             [state]"r"(state),
           [lps_tab]"r"(ff_h264_cabac_tables + H264_LPS_RANGE_OFFSET),
          [mlps_tab]"r"(ff_h264_cabac_tables + H264_MLPS_STATE_OFFSET + 128),
              [byte]"i"(offsetof(CABACContext, bytestream)),
               [end]"i"(offsetof(CABACContext, bytestream_end))
        : "memory", "cc"
        );

    return bit & 1;
}

#endif /* HAVE_INLINE_ASM */

#endif /* AVCODEC_AARCH64_CABAC_H */
