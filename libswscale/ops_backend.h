/**
 * Copyright (C) 2025 Niklas Haas
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

#ifndef SWSCALE_OPS_BACKEND_H
#define SWSCALE_OPS_BACKEND_H

#include <assert.h>
#include <float.h>
#include <stdint.h>

#include "libavutil/attributes.h"

#include "ops_internal.h"

#ifndef SWS_CHUNK_SIZE
#  define SWS_CHUNK_SIZE 8
#endif

#ifndef SWS_ALIGNMENT
#  define SWS_ALIGNMENT 32
#endif

#ifndef SWS_VECTOR_SIZE
#  define SWS_VECTOR_SIZE 16
#endif

#ifdef __clang__
#  define SWS_FUNC
#  define SWS_LOOP AV_PRAGMA(clang loop vectorize_width(SWS_CHUNK_SIZE))
#elif defined(__GNUC__)
#  define SWS_FUNC __attribute__((optimize("tree-vectorize")))
#  define SWS_LOOP AV_PRAGMA(GCC ivdep)
#endif

#if defined(__clang__)
#  define SWS_ASSUME(cond) __builtin_assume(cond)
#elif defined(__GNUC__)
#  define SWS_ASSUME(cond) { if (!(cond)) __builtin_unreachable(); }
#else
#  define SWS_ASSUME(cond) ((void) (cond))
#endif

#if defined(__clang__) || defined(__GNUC__)
#  define SWS_ASSUME_ALIGNED(ptr)  __builtin_assume_aligned(ptr, SWS_ALIGNMENT)
#else
#  define SWS_ASSUME_ALIGNED(ptr) ((void *) (ptr))
#endif

#define bitfn2(name, ext) name ## _ ## ext
#define bitfn(name, ext)  bitfn2(name, ext)

#define SUFFIX   AV_JOIN(FMT_CHAR, BIT_DEPTH)
#define bfn(name) bitfn(name, SUFFIX)

typedef struct OpImpl {
    SwsOp op;
    SwsOpFunc func;
    SwsOpFunc func_n;
    int (*setup)(const SwsOp *op, const void **out_priv); /* optional */
    void (*free)(void *priv);
} OpImpl;

/**
 * Use GCC vectors instead of arrays on recent enough GCC. These are
 * transparently backwards compatible with array syntax, but allow the
 * compiler to pass pixel data directly in SIMD registers
 **/
#if !AV_GCC_VERSION_AT_LEAST(14, 0)
#  undef  SWS_VECTOR_SIZE
#  define SWS_VECTOR_SIZE 0
#endif

#if SWS_VECTOR_SIZE >= 1 * SWS_CHUNK_SIZE
  typedef uint8_t u8vec_t __attribute__((vector_size(1 * SWS_CHUNK_SIZE)));
#else
  typedef uint8_t u8vec_t[SWS_CHUNK_SIZE];
#endif

#if SWS_VECTOR_SIZE >= 2 * SWS_CHUNK_SIZE
  typedef uint16_t u16vec_t __attribute__((vector_size(2 * SWS_CHUNK_SIZE)));
#else
  typedef uint16_t u16vec_t[SWS_CHUNK_SIZE];
#endif

#if SWS_VECTOR_SIZE >= 4 * SWS_CHUNK_SIZE
  typedef uint32_t u32vec_t __attribute__((vector_size(4 * SWS_CHUNK_SIZE)));
  typedef float    f32vec_t __attribute__((vector_size(4 * SWS_CHUNK_SIZE)));
#else
  typedef uint32_t u32vec_t[SWS_CHUNK_SIZE];
  typedef float    f32vec_t[SWS_CHUNK_SIZE];
#endif

/* Internal signature of continuations */
typedef void (*func_u8vec_t)(const SwsOpExec *exec, const SwsOpImpl *impl,
                             u8vec_t x, u8vec_t y, u8vec_t z, u8vec_t w);
typedef void (*func_u16vec_t)(const SwsOpExec *exec, const SwsOpImpl *impl,
                              u16vec_t x, u16vec_t y, u16vec_t z, u16vec_t w);
typedef void (*func_u32vec_t)(const SwsOpExec *exec, const SwsOpImpl *impl,
                              u32vec_t x, u32vec_t y, u32vec_t z, u32vec_t w);
typedef void (*func_f32vec_t)(const SwsOpExec *exec, const SwsOpImpl *impl,
                              f32vec_t x, f32vec_t y, f32vec_t z, f32vec_t w);

/* Helper macros to make writing common function signatures less painful */
#define DECL_FUNC(NAME, ...) \
    static av_always_inline void bfn(NAME)(const SwsOpExec *restrict exec,       \
                                          const SwsOpImpl *restrict impl,       \
                                          vec_t x, vec_t y, vec_t z, vec_t w,   \
                                          __VA_ARGS__)

#define DECL_READ(NAME, ...)                                                    \
    static av_always_inline void bfn(NAME)(const SwsOpExec *restrict exec,       \
                                          const SwsOpImpl *restrict impl,       \
                                          const pixel_t *restrict in0,          \
                                          const pixel_t *restrict in1,          \
                                          const pixel_t *restrict in2,          \
                                          const pixel_t *restrict in3,          \
                                          __VA_ARGS__)

#define DECL_WRITE(NAME, ...)                                                   \
    DECL_FUNC(NAME, pixel_t *restrict out0, pixel_t *restrict out1,             \
                    pixel_t *restrict out2, pixel_t *restrict out3,             \
                    __VA_ARGS__)

#define DECL_FUNC_PATTERN(NAME) \
    DECL_FUNC(NAME, const bool X, const bool Y, const bool Z, const bool W)

/* Helper macros to call into functions declared with DECL_FUNC_* */
#define CALL_READONLY(FUNC, ...) \
    bfn(FUNC)(exec, impl, __VA_ARGS__)

#define CALL(FUNC, ...)                                                         \
    CALL_READONLY(FUNC, x, y, z, w, __VA_ARGS__)

/* Helper macros to declare continuation functions */
#define DECL_IMPL_READONLY(NAME)                                                \
    static SWS_FUNC void bfn(NAME)(const SwsOpExec *restrict exec,               \
                                  const SwsOpImpl *restrict impl)               \

#define DECL_IMPL(NAME)                                                         \
    static SWS_FUNC void bfn(NAME)(const SwsOpExec *restrict exec,               \
                                  const SwsOpImpl *restrict impl,               \
                                  vec_t x, vec_t y, vec_t z, vec_t w)

/* Helper macro to call into the next continuation with a given type */
#define CONTINUE(VTYPE, ...) \
    ((AV_GLUE(func_, VTYPE)) impl->next)(exec, &impl[1], __VA_ARGS__)

/* Helper macros for common op setup code */
#define DECL_SETUP(NAME)                                                        \
    static int bfn(setup_##NAME)(const SwsOp *op, const void **out_priv)

#define SETUP_MEMDUP(c) ff_setup_memdup(&c, sizeof(c), out_priv)
static inline int ff_setup_memdup(const void *c, size_t size, const void **out)
{
    *out = av_memdup(c, size);
    return *out ? 0 : AVERROR(ENOMEM);
}

/* Helper macros for declaring op table entries */
#define DECL_ENTRY(NAME, ...)                                                   \
    static const OpImpl bfn(op_##NAME) = {                                       \
        .op.type = PIXEL_TYPE,                                                  \
        .func    = (SwsOpFunc) bfn(NAME),                                        \
        __VA_ARGS__                                                             \
    }

#define DECL_ENTRY_SIMPLE(NAME, ...)                                            \
    static const OpImpl bfn(op_##NAME) = {                                       \
        .op = {                                                                 \
            .type = PIXEL_TYPE,                                                 \
            __VA_ARGS__                                                         \
        },                                                                      \
        .func    = (SwsOpFunc) bfn(NAME),                                        \
    }

/* Helpers for dealing with (common) subsets of operations (Y, YA, YUV, YUVA) */
#define WRAP_PATTERN(FUNC, X, Y, Z, W, ...)                                     \
    DECL_IMPL(FUNC##_##X##Y##Z##W)                                              \
    {                                                                           \
        CALL(FUNC, X, Y, Z, W);                                                 \
    }                                                                           \
                                                                                \
    DECL_ENTRY(FUNC##_##X##Y##Z##W,                                             \
        .op.comps.unused = { !X, !Y, !Z, !W },                                  \
        __VA_ARGS__                                                             \
    )

#define WRAP_COMMON_PATTERNS(FUNC, ...)                                         \
    WRAP_PATTERN(FUNC, 1, 0, 0, 0, __VA_ARGS__);                                \
    WRAP_PATTERN(FUNC, 1, 0, 0, 1, __VA_ARGS__);                                \
    WRAP_PATTERN(FUNC, 1, 1, 1, 0, __VA_ARGS__);                                \
    WRAP_PATTERN(FUNC, 1, 1, 1, 1, __VA_ARGS__)

/* Miscellaneous helpers */
#define av_q2pixel(q) ((q).den ? (pixel_t) (q).num / (q).den : 0)

#endif
