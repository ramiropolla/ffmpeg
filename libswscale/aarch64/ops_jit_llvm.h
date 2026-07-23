/*
 * Copyright (C) 2026 Ramiro Polla
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

#ifndef SWSCALE_AARCH64_OPS_JIT_LLVM_H
#define SWSCALE_AARCH64_OPS_JIT_LLVM_H

#include <stddef.h>
#include <stdint.h>

/**
 * Assemble AArch64 GAS-syntax text into a read+exec memory buffer.
 *
 * On success, sets *out_text to executable memory of *out_size bytes
 * that must be released with ff_sws_jit_free(*out_text, *out_size) and returns 0.
 * Returns a negative AVERROR code on failure.
 */
int ff_sws_jit_assemble_llvm(const char *asm_src, uint8_t **out_text, size_t *out_size);

#endif /* SWSCALE_AARCH64_OPS_JIT_LLVM_H */
