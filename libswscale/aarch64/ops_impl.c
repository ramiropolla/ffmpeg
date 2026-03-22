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

// NOTE this file should not depend on ffmpeg. It will be built with hostcc and run at build time.

#include <assert.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

#include "ops_impl.h"

/*********************************************************************/
static const char pixel_types[AARCH64_SWS_OP_TYPE_NB][32] = {
    [AARCH64_PIXEL_U8 ] = "AARCH64_PIXEL_U8",
    [AARCH64_PIXEL_U16] = "AARCH64_PIXEL_U16",
    [AARCH64_PIXEL_U32] = "AARCH64_PIXEL_U32",
    [AARCH64_PIXEL_F32] = "AARCH64_PIXEL_F32",
};

const char *sws_aarch64_pixel_type(SwsAArch64PixelType fmt)
{
    if (fmt >= AARCH64_PIXEL_TYPE_NB) {
        assert(!"Invalid pixel type!");
        return NULL;
    }
    return pixel_types[fmt];
}

static const char pixel_type_names[AARCH64_SWS_OP_TYPE_NB][4] = {
    [AARCH64_PIXEL_U8 ] = "u8",
    [AARCH64_PIXEL_U16] = "u16",
    [AARCH64_PIXEL_U32] = "u32",
    [AARCH64_PIXEL_F32] = "f32",
};

const char *sws_aarch64_pixel_type_name(SwsAArch64PixelType fmt)
{
    if (fmt >= AARCH64_PIXEL_TYPE_NB) {
        assert(!"Invalid pixel type!");
        return NULL;
    }
    return pixel_type_names[fmt];
}

size_t sws_aarch64_pixel_size(SwsAArch64PixelType fmt)
{
    switch (fmt) {
    case AARCH64_PIXEL_U8:  return 1;
    case AARCH64_PIXEL_U16: return 2;
    case AARCH64_PIXEL_U32: return 4;
    case AARCH64_PIXEL_F32: return 4;
    default:
        assert(!"Invalid pixel type!");
        break;
    }
    return 0;
}

/*********************************************************************/
static const char op_types[AARCH64_SWS_OP_TYPE_NB][32] = {
    [AARCH64_SWS_OP_NONE          ] = "AARCH64_SWS_OP_NONE",
    [AARCH64_SWS_OP_PROCESS       ] = "AARCH64_SWS_OP_PROCESS",
    [AARCH64_SWS_OP_PROCESS_RETURN] = "AARCH64_SWS_OP_PROCESS_RETURN",
    [AARCH64_SWS_OP_READ_BIT      ] = "AARCH64_SWS_OP_READ_BIT",
    [AARCH64_SWS_OP_READ_NIBBLE   ] = "AARCH64_SWS_OP_READ_NIBBLE",
    [AARCH64_SWS_OP_READ_PACKED   ] = "AARCH64_SWS_OP_READ_PACKED",
    [AARCH64_SWS_OP_READ_PLANAR   ] = "AARCH64_SWS_OP_READ_PLANAR",
    [AARCH64_SWS_OP_WRITE_BIT     ] = "AARCH64_SWS_OP_WRITE_BIT",
    [AARCH64_SWS_OP_WRITE_NIBBLE  ] = "AARCH64_SWS_OP_WRITE_NIBBLE",
    [AARCH64_SWS_OP_WRITE_PACKED  ] = "AARCH64_SWS_OP_WRITE_PACKED",
    [AARCH64_SWS_OP_WRITE_PLANAR  ] = "AARCH64_SWS_OP_WRITE_PLANAR",
    [AARCH64_SWS_OP_SWAP_BYTES    ] = "AARCH64_SWS_OP_SWAP_BYTES",
    [AARCH64_SWS_OP_SWIZZLE       ] = "AARCH64_SWS_OP_SWIZZLE",
    [AARCH64_SWS_OP_UNPACK        ] = "AARCH64_SWS_OP_UNPACK",
    [AARCH64_SWS_OP_PACK          ] = "AARCH64_SWS_OP_PACK",
    [AARCH64_SWS_OP_LSHIFT        ] = "AARCH64_SWS_OP_LSHIFT",
    [AARCH64_SWS_OP_RSHIFT        ] = "AARCH64_SWS_OP_RSHIFT",
    [AARCH64_SWS_OP_CLEAR         ] = "AARCH64_SWS_OP_CLEAR",
    [AARCH64_SWS_OP_CONVERT       ] = "AARCH64_SWS_OP_CONVERT",
    [AARCH64_SWS_OP_EXPAND        ] = "AARCH64_SWS_OP_EXPAND",
    [AARCH64_SWS_OP_MIN           ] = "AARCH64_SWS_OP_MIN",
    [AARCH64_SWS_OP_MAX           ] = "AARCH64_SWS_OP_MAX",
    [AARCH64_SWS_OP_SCALE         ] = "AARCH64_SWS_OP_SCALE",
    [AARCH64_SWS_OP_LINEAR        ] = "AARCH64_SWS_OP_LINEAR",
    [AARCH64_SWS_OP_DITHER        ] = "AARCH64_SWS_OP_DITHER",
};

const char *sws_aarch64_op_type(SwsAArch64OpType op)
{
    if (op == AARCH64_SWS_OP_NONE || op >= AARCH64_SWS_OP_TYPE_NB) {
        assert(!"Invalid op type!");
        return NULL;
    }
    return op_types[op];
}

static const char op_type_names[AARCH64_SWS_OP_TYPE_NB][16] = {
    [AARCH64_SWS_OP_NONE          ] = "none",
    [AARCH64_SWS_OP_PROCESS       ] = "process",
    [AARCH64_SWS_OP_PROCESS_RETURN] = "process_return",
    [AARCH64_SWS_OP_READ_BIT      ] = "read_bit",
    [AARCH64_SWS_OP_READ_NIBBLE   ] = "read_nibble",
    [AARCH64_SWS_OP_READ_PACKED   ] = "read_packed",
    [AARCH64_SWS_OP_READ_PLANAR   ] = "read_planar",
    [AARCH64_SWS_OP_WRITE_BIT     ] = "write_bit",
    [AARCH64_SWS_OP_WRITE_NIBBLE  ] = "write_nibble",
    [AARCH64_SWS_OP_WRITE_PACKED  ] = "write_packed",
    [AARCH64_SWS_OP_WRITE_PLANAR  ] = "write_planar",
    [AARCH64_SWS_OP_SWAP_BYTES    ] = "swap_bytes",
    [AARCH64_SWS_OP_SWIZZLE       ] = "swizzle",
    [AARCH64_SWS_OP_UNPACK        ] = "unpack",
    [AARCH64_SWS_OP_PACK          ] = "pack",
    [AARCH64_SWS_OP_LSHIFT        ] = "lshift",
    [AARCH64_SWS_OP_RSHIFT        ] = "rshift",
    [AARCH64_SWS_OP_CLEAR         ] = "clear",
    [AARCH64_SWS_OP_CONVERT       ] = "convert",
    [AARCH64_SWS_OP_EXPAND        ] = "expand",
    [AARCH64_SWS_OP_MIN           ] = "min",
    [AARCH64_SWS_OP_MAX           ] = "max",
    [AARCH64_SWS_OP_SCALE         ] = "scale",
    [AARCH64_SWS_OP_LINEAR        ] = "linear",
    [AARCH64_SWS_OP_DITHER        ] = "dither",
};

const char *sws_aarch64_op_type_name(SwsAArch64OpType op)
{
    if (op == AARCH64_SWS_OP_NONE || op >= AARCH64_SWS_OP_TYPE_NB) {
        assert(!"Invalid op type!");
        return NULL;
    }
    return op_type_names[op];
}

/*********************************************************************/
static void buf_appendf(char **pbuf, size_t *prem, const char *fmt, ...)
{
    char *buf = *pbuf;
    size_t rem = *prem;
    if (!rem)
        return;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, rem, fmt, ap);
    va_end(ap);

    if (n > 0) {
        if (n < rem) {
            buf += n;
            rem -= n;
        } else {
            buf += rem - 1;
            rem = 0;
        }
        *pbuf = buf;
        *prem = rem;
    }
}

/*********************************************************************/
typedef struct ParamField {
    const char *name;
    size_t offset;
    size_t size;
    void (*print_str)(char **pbuf, size_t *prem, void *p);
    void (*print_val)(char **pbuf, size_t *prem, void *p);
    int (*cmp_val)(void *pa, void *pb);
} ParamField;

#define PARAM_FIELD(name) #name, offsetof(SwsAArch64OpImplParams, name), sizeof(((SwsAArch64OpImplParams *)0)->name)

static void print_op_name(char **pbuf, size_t *prem, void *p)
{
    SwsAArch64OpType op = *(SwsAArch64OpType *)p;
    buf_appendf(pbuf, prem, "_%s", sws_aarch64_op_type_name(op));
}

static void print_op_val(char **pbuf, size_t *prem, void *p)
{
    SwsAArch64OpType op = *(SwsAArch64OpType *)p;
    buf_appendf(pbuf, prem, "%s", sws_aarch64_op_type(op));
}

static int cmp_op(void *pa, void *pb)
{
    int64_t ia = (int64_t) *((SwsAArch64OpType *) pa);
    int64_t ib = (int64_t) *((SwsAArch64OpType *) pb);
    int64_t diff = ia - ib;
    if (diff)
        return diff < 0 ? -1 : 1;
    return 0;
}

static void print_pixel_name(char **pbuf, size_t *prem, void *p)
{
    SwsAArch64PixelType type = *(SwsAArch64PixelType *)p;
    buf_appendf(pbuf, prem, "_%s", sws_aarch64_pixel_type_name(type));
}

static void print_pixel_val(char **pbuf, size_t *prem, void *p)
{
    SwsAArch64PixelType type = *(SwsAArch64PixelType *)p;
    buf_appendf(pbuf, prem, "%s", sws_aarch64_pixel_type(type));
}

static int cmp_pixel(void *pa, void *pb)
{
    int64_t ia = (int64_t) *((SwsAArch64PixelType *) pa);
    int64_t ib = (int64_t) *((SwsAArch64PixelType *) pb);
    int64_t diff = ia - ib;
    if (diff)
        return diff < 0 ? -1 : 1;
    return 0;
}

static void print_u8_name(char **pbuf, size_t *prem, void *p)
{
    uint8_t val = *(uint8_t *)p;
    buf_appendf(pbuf, prem, "_%u", val);
}

static void print_u8_val(char **pbuf, size_t *prem, void *p)
{
    uint8_t val = *(uint8_t *)p;
    buf_appendf(pbuf, prem, "%u", val);
}

static int cmp_u8(void *pa, void *pb)
{
    int64_t ia = (int64_t) *((uint8_t *) pa);
    int64_t ib = (int64_t) *((uint8_t *) pb);
    int64_t diff = ia - ib;
    if (diff)
        return diff < 0 ? -1 : 1;
    return 0;
}

static void print_u16_name(char **pbuf, size_t *prem, void *p)
{
    uint16_t val = *(uint16_t *)p;
    buf_appendf(pbuf, prem, "_%04x", val);
}

static void print_u16_val(char **pbuf, size_t *prem, void *p)
{
    uint16_t val = *(uint16_t *)p;
    buf_appendf(pbuf, prem, "0x%04x", val);
}

static int cmp_u16(void *pa, void *pb)
{
    int64_t ia = (int64_t) *((uint16_t *) pa);
    int64_t ib = (int64_t) *((uint16_t *) pb);
    int64_t diff = ia - ib;
    if (diff)
        return diff < 0 ? -1 : 1;
    return 0;
}

static void print_u40_name(char **pbuf, size_t *prem, void *p)
{
    uint64_t val = *(uint64_t *)p;
    buf_appendf(pbuf, prem, "_%010" PRIx64, val);
}

static void print_u40_val(char **pbuf, size_t *prem, void *p)
{
    uint64_t val = *(uint64_t *)p;
    buf_appendf(pbuf, prem, "0x%010" PRIx64 "ULL", val);
}

static int cmp_u40(void *pa, void *pb)
{
    int64_t ia = (int64_t) *((uint64_t *) pa);
    int64_t ib = (int64_t) *((uint64_t *) pb);
    int64_t diff = ia - ib;
    if (diff)
        return diff < 0 ? -1 : 1;
    return 0;
}

static const ParamField field_op               = { PARAM_FIELD(op),               print_op_name,    print_op_val,    cmp_op };
static const ParamField field_mask             = { PARAM_FIELD(mask),             print_u16_name,   print_u16_val,   cmp_u16 };
static const ParamField field_type             = { PARAM_FIELD(type),             print_pixel_name, print_pixel_val, cmp_pixel };
static const ParamField field_block_size       = { PARAM_FIELD(block_size),       print_u8_name,    print_u8_val,    cmp_u8 };
static const ParamField field_shift            = { PARAM_FIELD(shift),            print_u8_name,    print_u8_val,    cmp_u8 };
static const ParamField field_swizzle          = { PARAM_FIELD(swizzle),          print_u16_name,   print_u16_val,   cmp_u16 };
static const ParamField field_pack             = { PARAM_FIELD(pack),             print_u16_name,   print_u16_val,   cmp_u16 };
static const ParamField field_to_type          = { PARAM_FIELD(to_type),          print_pixel_name, print_pixel_val, cmp_pixel };
static const ParamField field_linear           = { PARAM_FIELD(linear),           print_u40_name,   print_u40_val,   cmp_u40 };
static const ParamField field_dither_y_offset  = { PARAM_FIELD(dither.y_offset),  print_u16_name,   print_u16_val,   cmp_u16 };
static const ParamField field_dither_size_log2 = { PARAM_FIELD(dither.size_log2), print_u8_name,    print_u8_val,    cmp_u8 };

#define MAX_LEVELS 8
static const ParamField *op_fields[AARCH64_SWS_OP_TYPE_NB][MAX_LEVELS] = {
    [AARCH64_SWS_OP_PROCESS       ] = { &field_op, &field_mask },
    [AARCH64_SWS_OP_PROCESS_RETURN] = { &field_op, &field_mask },
    [AARCH64_SWS_OP_READ_BIT      ] = { &field_op, &field_block_size, &field_type, &field_mask },
    [AARCH64_SWS_OP_READ_NIBBLE   ] = { &field_op, &field_block_size, &field_type, &field_mask },
    [AARCH64_SWS_OP_READ_PACKED   ] = { &field_op, &field_block_size, &field_type, &field_mask },
    [AARCH64_SWS_OP_READ_PLANAR   ] = { &field_op, &field_block_size, &field_type, &field_mask },
    [AARCH64_SWS_OP_WRITE_BIT     ] = { &field_op, &field_block_size, &field_type, &field_mask },
    [AARCH64_SWS_OP_WRITE_NIBBLE  ] = { &field_op, &field_block_size, &field_type, &field_mask },
    [AARCH64_SWS_OP_WRITE_PACKED  ] = { &field_op, &field_block_size, &field_type, &field_mask },
    [AARCH64_SWS_OP_WRITE_PLANAR  ] = { &field_op, &field_block_size, &field_type, &field_mask },
    [AARCH64_SWS_OP_SWAP_BYTES    ] = { &field_op, &field_block_size, &field_type, &field_mask },
    [AARCH64_SWS_OP_SWIZZLE       ] = { &field_op, &field_swizzle, &field_block_size, &field_type, &field_mask },
    [AARCH64_SWS_OP_UNPACK        ] = { &field_op, &field_pack, &field_block_size, &field_type, &field_mask },
    [AARCH64_SWS_OP_PACK          ] = { &field_op, &field_pack, &field_block_size, &field_type, &field_mask },
    [AARCH64_SWS_OP_LSHIFT        ] = { &field_op, &field_shift, &field_block_size, &field_type, &field_mask },
    [AARCH64_SWS_OP_RSHIFT        ] = { &field_op, &field_shift, &field_block_size, &field_type, &field_mask },
    [AARCH64_SWS_OP_CLEAR         ] = { &field_op, &field_block_size, &field_type, &field_mask },
    [AARCH64_SWS_OP_CONVERT       ] = { &field_op, &field_to_type, &field_block_size, &field_type, &field_mask },
    [AARCH64_SWS_OP_EXPAND        ] = { &field_op, &field_to_type, &field_block_size, &field_type, &field_mask },
    [AARCH64_SWS_OP_MIN           ] = { &field_op, &field_block_size, &field_type, &field_mask },
    [AARCH64_SWS_OP_MAX           ] = { &field_op, &field_block_size, &field_type, &field_mask },
    [AARCH64_SWS_OP_SCALE         ] = { &field_op, &field_block_size, &field_type, &field_mask },
    [AARCH64_SWS_OP_LINEAR        ] = { &field_op, &field_linear, &field_block_size, &field_type, &field_mask },
    [AARCH64_SWS_OP_DITHER        ] = { &field_op, &field_dither_y_offset, &field_dither_size_log2, &field_block_size, &field_type, &field_mask },
};

/*********************************************************************/
static void impl_func_name(char **buf, size_t *size, const SwsAArch64OpImplParams *params)
{
    buf_appendf(buf, size, "ff_sws");
    const ParamField **fields = op_fields[params->op];
    for (int i = 0; fields[i]; i++) {
        const ParamField *field = fields[i];
        void *p = (void *) (((uintptr_t) params) + field->offset);
        field->print_str(buf, size, p);
    }
    buf_appendf(buf, size, "_neon");
}

void sws_aarch64_op_impl_func_name(char *buf, size_t size, const SwsAArch64OpImplParams *params)
{
    impl_func_name(&buf, &size, params);
    assert(size);
}

void sws_aarch64_op_impl_serialize(char *buf, size_t size, const SwsAArch64OpImplParams *params)
{
    buf_appendf(&buf, &size, "{");
    const ParamField **fields = op_fields[params->op];
    for (int i = 0; fields[i]; i++) {
        const ParamField *field = fields[i];
        void *p = (void *) (((uintptr_t) params) + field->offset);
        if (i)
            buf_appendf(&buf, &size, ",");
        buf_appendf(&buf, &size, " .%s = ", field->name);
        field->print_val(&buf, &size, p);
    }
    buf_appendf(&buf, &size, " }");
    assert(size);
}

void sws_aarch64_op_impl_cond_str(char *buf, size_t size, const SwsAArch64OpImplParams *params,
                                  const SwsAArch64OpImplParams *prev, const char *p_str)
{
    int first_diff = 0;
    int prev_levels = 0;
    int levels = 0;

    /* Compute number of current levels. */
    if (params) {
        const ParamField **fields = op_fields[params->op];
        while (fields[levels])
            levels++;
    }

    /* Compute number of previous levels. */
    if (prev) {
        const ParamField **prev_fields = op_fields[prev->op];
        while (prev_fields[prev_levels])
            prev_levels++;
    }

    /* Walk up and check the conditions that match. */
    if (params && prev) {
        const ParamField **fields = op_fields[params->op];
        first_diff = -1;
        for (int i = 0; fields[i]; i++) {
            const ParamField *field = fields[i];
            if (first_diff < 0) {
                int diff = field->cmp_val((void  *) (((uintptr_t) params) + field->offset),
                                          (void  *) (((uintptr_t) prev) + field->offset));
                if (diff)
                    first_diff = i;
            }
        }
    }

    /* Walk back closing conditions. */
    if (prev) {
        for (int i = prev_levels - 2; i >= first_diff; i--) {
            buf_appendf(&buf, &size, "%*sreturn NULL;\n", 4 * (i + 2), "");
            buf_appendf(&buf, &size, "%*s}\n", 4 * (i + 1), "");
        }
    }

    /* Walk up adding conditions. */
    if (params) {
        const ParamField **fields = op_fields[params->op];
        for (int i = first_diff; i < levels; i++) {
            const ParamField *field = fields[i];
            void *p = (void *) (((uintptr_t) params) + field->offset);
            buf_appendf(&buf, &size, "%*sif (%s%s == ", 4 * (i + 1), "", p_str, field->name);
            field->print_val(&buf, &size, p);
            buf_appendf(&buf, &size, ")");
            if (i == (levels - 1)) {
                buf_appendf(&buf, &size, " return ");
                impl_func_name(&buf, &size, params);
                buf_appendf(&buf, &size, ";\n");
            } else {
                buf_appendf(&buf, &size, " {\n");
            }
        }
    }

    assert(size);
}

int sws_aarch64_op_impl_cmp(const void *a, const void *b)
{
    const SwsAArch64OpImplParams *pa = (const SwsAArch64OpImplParams *) a;
    const SwsAArch64OpImplParams *pb = (const SwsAArch64OpImplParams *) b;

    const ParamField **fields = op_fields[pa->op];
    for (int i = 0; fields[i]; i++) {
        const ParamField *field = fields[i];
        int diff = field->cmp_val((void  *) (((uintptr_t) pa) + field->offset),
                                  (void  *) (((uintptr_t) pb) + field->offset));
        if (diff)
            return diff;
    }
    return 0;
}
