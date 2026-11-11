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

/* Used both for the CPU level and for the ISA of a kernel */
typedef enum SwsX86ISA {
    SWS_X86_SSE4,
    SWS_X86_AVX2,
    SWS_X86_AVX512,
    SWS_X86_AVX512ICL,
    SWS_X86_ISA_NB,
} SwsX86ISA;

/* Vector register size in bytes */
static inline int ff_sws_mmsize_x86(SwsX86ISA isa)
{
    switch (isa) {
    case SWS_X86_SSE4: return 16;
    case SWS_X86_AVX2: return 32;
    default:           return 64;
    }
}

#endif /* SWSCALE_X86_UOPS_X86_H */
