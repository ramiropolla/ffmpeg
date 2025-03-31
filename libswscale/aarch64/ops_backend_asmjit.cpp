/**
 * Copyright (C) 2025 Ramiro Polla
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

// #define EMIT_BRK

extern "C" {
#include "libavutil/cpu.h"

#include "../ops_internal.h"
#include "../ops_backend.h"
}

#include <asmjit/core.h>
#include <asmjit/a64.h>

#include <iostream>
#include <vector>

using namespace asmjit;

struct AsmJitContext {
    JitRuntime m_rt;
    CodeHolder m_code;
    StringLogger m_logger;
    a64::Compiler *m_cc;

    FuncNode *m_func;
    BaseNode *m_prologue;
    BaseNode *m_tail;
    a64::Gp m_exec;
    a64::Vec m_orig_vl[4];
    a64::Vec m_orig_vh[4];
    a64::Vec m_vl[4];
    a64::Vec m_vh[4];
    a64::Vec m_vdata[4];

    AsmJitContext()
    {
        m_code.init(m_rt.environment(), m_rt.cpuFeatures());
        if (av_log_get_level() >= AV_LOG_DEBUG)
            m_code.setLogger(&m_logger);
        m_cc = new a64::Compiler(&m_code);
        a64::Compiler &cc = *m_cc;
        cc.addDiagnosticOptions(DiagnosticOptions::kRAAnnotate);
        m_func = cc.addFunc(FuncSignature::build<void, uint8_t *, uint8_t *, uint8_t *, uint8_t *>());
        m_prologue = cc.firstNode()->next();
        m_tail = cc.cursor();
#ifdef EMIT_BRK
        to_prologue();
        cc.brk(0xf000);
        from_prologue();
#endif
        m_exec = cc.newGpz();
        m_func->setArg(0, m_exec);
    }

    void to_prologue(void)
    {
        a64::Compiler &cc = *m_cc;
        m_tail = cc.setCursor(m_prologue);
    }

    void from_prologue(void)
    {
        a64::Compiler &cc = *m_cc;
        m_prologue = cc.setCursor(m_tail);
    }
};

/* Vector element type for given SwsOp */
struct VectorElementType {
    VectorElementType(const SwsOpChain *const chain)
        : m_chain(chain)
    {
    }

    a64::Vec operator()(const a64::Vec &vreg, const SwsOp &op) const
    {
        if (op.type == SWS_PIXEL_U8  && m_chain->block_w == 8)
            return vreg.b8(); /* half vector */
        if (op.type == SWS_PIXEL_U16 && m_chain->block_w == 8)
            return vreg.h8(); /* full vector */
        if (op.type == SWS_PIXEL_U32 && m_chain->block_w == 8)
            return vreg.s4(); /* full vector (TRUNCATE) */
        if (op.type == SWS_PIXEL_F32 && m_chain->block_w == 8)
            return vreg.s4(); /* full vector (TRUNCATE) */
        if (op.type == SWS_PIXEL_U8  && m_chain->block_w == 16)
            return vreg.b16(); /* full vector */
        if (op.type == SWS_PIXEL_U16 && m_chain->block_w == 16)
            return vreg.h8(); /* full vector (TRUNCATE) */
        if (op.type == SWS_PIXEL_U32 && m_chain->block_w == 16)
            return vreg.s4(); /* full vector (TRUNCATE) */
        if (op.type == SWS_PIXEL_F32 && m_chain->block_w == 16)
            return vreg.s4(); /* full vector (TRUNCATE) */
        printf("ERORROORORRORORO %d %d\n", op.type, m_chain->block_w);
        return vreg.b16();
    }

    int size(const SwsOp &op)
    {
        int elsize = (op.type == SWS_PIXEL_U8)  ? 1
                   : (op.type == SWS_PIXEL_U16) ? 2
                   :                              4;
        int ret = m_chain->block_w * elsize;
        return FFMIN(ret, 16);
    }

    const SwsOpChain *m_chain;
};

#define LOOP_USED(idx)                \
    for (int idx = 0; idx < 4; idx++) \
        if (!op.comps.unused[idx])

static Label emit_data(AsmJitContext *ctx, void *data, size_t size)
{
    a64::Compiler &cc = *ctx->m_cc;

    Label ldata = cc.newLabel();
    BaseNode *cursor = cc.cursor();
    cc.setCursor(ctx->m_func->endNode()->prev());
    cc.align(AlignMode::kData, 16);
    cc.bind(ldata);
    cc.embed(data, size);
    cc.setCursor(cursor);

    return ldata;
}

static void read_vdata(AsmJitContext *ctx, Label ldata, int count)
{
    a64::Compiler &cc = *ctx->m_cc;
    a64::Vec *vdata = ctx->m_vdata;
    a64::Gp rdata = cc.newGpz();

    for (int i = 0; i < count; i++) {
        vdata[i] = cc.newVecQ();
    }

    cc.adr(rdata, ldata);
    switch (count) {
    case 1: cc.ld1(vdata[0].b16(), a64::ptr(rdata)); break;
    case 2: cc.ld1(vdata[0].b16(), vdata[1].b16(), a64::ptr(rdata)); break;
    case 3: cc.ld1(vdata[0].b16(), vdata[1].b16(), vdata[2].b16(), a64::ptr(rdata)); break;
    case 4: cc.ld1(vdata[0].b16(), vdata[1].b16(), vdata[2].b16(), vdata[3].b16(), a64::ptr(rdata)); break;
    }
}

static inline uint32_t mask_from_i(int i)
{
    return (1 << i) | (1 << (i + 4));
}

static inline uint32_t mask_from_used(const SwsOp &op)
{
    uint32_t mask = 0;
    LOOP_USED(i) {
        mask |= mask_from_i(i);
    }
    return mask;
}

static inline uint32_t mask_from_count(int count)
{
    uint32_t mask = 0;
    for (int i = 0; i < count; i++) {
        mask |= mask_from_i(i);
    }
    return mask;
}

static void save_vectors_mask(AsmJitContext *ctx, uint32_t mask)
{
    a64::Vec *orig_vl = ctx->m_orig_vl;
    a64::Vec *orig_vh = ctx->m_orig_vh;
    a64::Vec *vl = ctx->m_vl;
    a64::Vec *vh = ctx->m_vh;
    for (int i = 0; i < 4; i++) {
        if (mask & (1 << i)) {
            orig_vl[i] = vl[i];
        }
        if (mask & (1 << (i + 4))) {
            orig_vh[i] = vh[i];
        }
    }
}

static void new_vectors_mask(AsmJitContext *ctx, uint32_t mask)
{
    a64::Compiler &cc = *ctx->m_cc;
    a64::Vec *vl = ctx->m_vl;
    a64::Vec *vh = ctx->m_vh;
    for (int i = 0; i < 4; i++) {
        if (mask & (1 << i)) {
            vl[i] = cc.newVecQ();
        }
        if (mask & (1 << (i + 4))) {
            vh[i] = cc.newVecQ();
        }
    }
}

static void new_vectors_used(AsmJitContext *ctx, const SwsOp &op)
{
    new_vectors_mask(ctx, mask_from_used(op));
}

static void new_vectors_count(AsmJitContext *ctx, int count)
{
    new_vectors_mask(ctx, mask_from_count(count));
}

static void new_vector(AsmJitContext *ctx, int i, int mask = 0xff)
{
    new_vectors_mask(ctx, mask_from_i(i) & mask);
}

static void save_vectors_used(AsmJitContext *ctx, const SwsOp &op)
{
    save_vectors_mask(ctx, mask_from_used(op));
}

static void save_vectors_count(AsmJitContext *ctx, int count)
{
    save_vectors_mask(ctx, mask_from_count(count));
}

static void save_vector(AsmJitContext *ctx, int i, int mask = 0xff)
{
    save_vectors_mask(ctx, mask_from_i(i) & mask);
}

static void refresh_vectors_mask(AsmJitContext *ctx, uint32_t mask)
{
    save_vectors_mask(ctx, mask);
    new_vectors_mask(ctx, mask);
}

static void refresh_vectors_used(AsmJitContext *ctx, const SwsOp &op)
{
    refresh_vectors_mask(ctx, mask_from_used(op));
}

static void refresh_vectors_count(AsmJitContext *ctx, int count)
{
    refresh_vectors_mask(ctx, mask_from_count(count));
}

static void refresh_vector(AsmJitContext *ctx, int i, int mask = 0xff)
{
    refresh_vectors_mask(ctx, mask_from_i(i) & mask);
}

static int emit_convert(AsmJitContext *ctx, const SwsOpChain *chain, const SwsOp &op, SwsPixelType from, SwsPixelType to, bool expand)
{
    a64::Compiler &cc = *ctx->m_cc;
    a64::Vec *orig_vl = ctx->m_orig_vl;
    a64::Vec *orig_vh = ctx->m_orig_vh;
    a64::Vec *vl = ctx->m_vl;
    a64::Vec *vh = ctx->m_vh;
    int from_size = ff_sws_pixel_type_size(from);
    int to_size   = ff_sws_pixel_type_size(to);

    if (from == SWS_PIXEL_F32) {
        cc.comment("convert (f32 -> u32)");
        LOOP_USED(i) {
            refresh_vector(ctx, i);
            cc.fcvtzu(vl[i].s4(), orig_vl[i].s4());
            cc.fcvtzu(vh[i].s4(), orig_vh[i].s4());
        }
    }

    if (expand) {
        if        (from_size == 1 && to_size == 2 && chain->block_w == 8) {
            cc.comment("convert (u8 -> u16, expand, 8)");
            LOOP_USED(i) {
                refresh_vector(ctx, i, 0x0f);
                cc.zip1(vl[i].b16(), orig_vl[i].b16(), orig_vl[i].b16());
            }
        } else if (from_size == 1 && to_size == 2 && chain->block_w == 16) {
            cc.comment("convert (u8 -> u16, expand, 16)");
            LOOP_USED(i) {
                save_vector(ctx, i, 0x0f);
                new_vector(ctx, i);
                cc.zip1(vl[i].b16(), orig_vl[i].b16(), orig_vl[i].b16());
                cc.zip2(vh[i].b16(), orig_vl[i].b16(), orig_vl[i].b16());
            }
        } else {
            return AVERROR(ENOTSUP);
        }
    } else {
        if (from_size == 1 && to_size > from_size && chain->block_w == 8) {
            cc.comment("convert (u8 -> u16, !expand, 8)");
            LOOP_USED(i) {
                refresh_vector(ctx, i, 0x0f);
                cc.uxtl(vl[i].h8(), orig_vl[i].b8());
            }
            from_size = 2;
        }

        if (from_size == 1 && to_size > from_size && chain->block_w == 16) {
            cc.comment("convert (u8 -> u16, !expand, 16)");
            LOOP_USED(i) {
                save_vector(ctx, i, 0x0f);
                new_vector(ctx, i);
                cc.uxtl (vl[i].h8(), orig_vl[i].b8());
                cc.uxtl2(vh[i].h8(), orig_vl[i].b16());
            }
            from_size = 2;
        }

        if (from_size == 2 && to_size == 4 && chain->block_w == 8) {
            cc.comment("convert (u16 -> u32, !expand, 8)");
            LOOP_USED(i) {
                save_vector(ctx, i, 0x0f);
                new_vector(ctx, i);
                cc.uxtl (vl[i].s4(), orig_vl[i].h4());
                cc.uxtl2(vh[i].s4(), orig_vl[i].h8());
            }
            from_size = 4;
        }

        if (from_size == 4 && to_size < from_size && chain->block_w == 8) {
            cc.comment("convert (u32 -> u16, !expand, 8)");
            LOOP_USED(i) {
                refresh_vector(ctx, i);
                cc.xtn(vl[i].h4(), orig_vl[i].s4());
                cc.xtn(vh[i].h4(), orig_vh[i].s4());
            }
            LOOP_USED(i) {
                cc.ins(vl[i].d(1), vh[i].d(0));
            }
            from_size = 2;
        }

        if (from_size == 2 && to_size == 1 && chain->block_w == 8) {
            cc.comment("convert (u16 -> u8, !expand, 8)");
            LOOP_USED(i) {
                refresh_vector(ctx, i, 0x0f);
                cc.xtn(vl[i].b8(), orig_vl[i].h8());
            }
            from_size = 1;
        }

        if (from_size == 2 && to_size == 1 && chain->block_w == 16) {
            cc.comment("convert (u16 -> u8, !expand, 16)");
            LOOP_USED(i) {
                refresh_vector(ctx, i);
                cc.xtn(vl[i].b8(), orig_vl[i].h8());
                cc.xtn(vh[i].b8(), orig_vh[i].h8());
            }
            LOOP_USED(i) {
                cc.ins(vl[i].d(1), vh[i].d(0));
            }
            from_size = 1;
        }
    }

    if (to == SWS_PIXEL_F32) {
        cc.comment("convert (u32 -> f32)");
        LOOP_USED(i) {
            refresh_vector(ctx, i);
            cc.ucvtf(vl[i].s4(), orig_vl[i].s4());
            cc.ucvtf(vh[i].s4(), orig_vh[i].s4());
        }
    }

    return 0;
}

static int asmjit_compile_op(AsmJitContext *ctx, SwsOpList *ops, SwsOpChain *chain)
{
    a64::Compiler &cc = *ctx->m_cc;
    a64::Gp &exec = ctx->m_exec;
    a64::Vec *orig_vl = ctx->m_orig_vl;
    a64::Vec *orig_vh = ctx->m_orig_vh;
    a64::Vec *vl = ctx->m_vl;
    a64::Vec *vh = ctx->m_vh;
    a64::Vec *vdata = ctx->m_vdata;

    SwsOp *prev = &ops->ops[-1];
    SwsOp &op = ops->ops[0];
    SwsOp *next = &ops->ops[1];

    VectorElementType vet(chain);

    /* Optimize convert if followed by pack */
    if (op.op == SWS_OP_CONVERT && next->op == SWS_OP_PACK && op.convert.to != next->pack.type) {
        next->type = next->pack.type;
        op.convert.to = next->pack.type;
    }

    bool use_vh = ((op.type == SWS_PIXEL_U16) && chain->block_w == 16)
               || ((op.type == SWS_PIXEL_U32) && chain->block_w == 8)
               || ((op.type == SWS_PIXEL_F32) && chain->block_w == 8);

    switch (op.op) {
    /* Input/output handling */
    case SWS_OP_READ:            /* gather raw pixels from planes */
        if (op.rw.frac)
            return AVERROR(ENOTSUP);
        new_vectors_mask(ctx, mask_from_count(op.rw.elems) & (use_vh ? 0xff : 0x0f));
        cc.comment("read");
        if (!op.rw.packed) {
            /* Load input pointers in prologue */
            ctx->to_prologue();
            cc.comment("prologue (read)");
            a64::Gp in[4];
            for (int i = 0; i < op.rw.elems; i++) {
                in[i] = cc.newGpz();
                cc.ldr(in[i], a64::ptr(exec, offsetof(SwsOpExec, in) + sizeof(uint8_t *) * i));
            }
            ctx->from_prologue();
            /* Read vectors from input pointers */
            for (int i = 0; i < op.rw.elems; i++) {
                if (use_vh)
                    cc.ld1(vet(vl[i], op), vet(vh[i], op), a64::ptr(in[i]).post(vet.size(op) * 2));
                else
                    cc.ld1(vet(vl[i], op),                 a64::ptr(in[i]).post(vet.size(op) * 1));
            }
        } else {
            /* Load input pointer in prologue */
            ctx->to_prologue();
            cc.comment("prologue (read)");
            a64::Gp in = cc.newGpz();
            cc.ldr(in, a64::ptr(exec, offsetof(SwsOpExec, in)));
            ctx->from_prologue();
            /* Read vectors from input pointer */
            switch (op.rw.elems) {
            case 1:
                if (use_vh)
                    cc.ld1(vet(vl[0], op), vet(vh[0], op),                                 a64::ptr(in).post(vet.size(op) * 2));
                else
                    cc.ld1(vet(vl[0], op),                                                 a64::ptr(in).post(vet.size(op) * 1));
                break;
            case 2:
                cc.ld2    (vet(vl[0], op), vet(vl[1], op),                                 a64::ptr(in).post(vet.size(op) * 2));
                if (use_vh)
                    cc.ld2(vet(vh[0], op), vet(vh[1], op),                                 a64::ptr(in).post(vet.size(op) * 2));
                break;
            case 3:
                cc.ld3    (vet(vl[0], op), vet(vl[1], op), vet(vl[2], op),                 a64::ptr(in).post(vet.size(op) * 3));
                if (use_vh)
                    cc.ld3(vet(vh[0], op), vet(vh[1], op), vet(vh[2], op),                 a64::ptr(in).post(vet.size(op) * 3));
                break;
            case 4:
                cc.ld4    (vet(vl[0], op), vet(vl[1], op), vet(vl[2], op), vet(vl[3], op), a64::ptr(in).post(vet.size(op) * 4));
                if (use_vh)
                    cc.ld4(vet(vh[0], op), vet(vh[1], op), vet(vh[2], op), vet(vh[3], op), a64::ptr(in).post(vet.size(op) * 4));
                break;
            }
        }
        break;
    case SWS_OP_WRITE:           /* write raw pixels to planes */
        if (op.rw.frac)
            return AVERROR(ENOTSUP);
        cc.comment("write");
        if (!op.rw.packed) {
            /* Load output pointers in prologue */
            ctx->to_prologue();
            cc.comment("prologue (write)");
            a64::Gp out[4];
            for (int i = 0; i < op.rw.elems; i++) {
                out[i] = cc.newGpz();
                cc.ldr(out[i], a64::ptr(exec, offsetof(SwsOpExec, out) + sizeof(uint8_t *) * i));
            }
            ctx->from_prologue();
            /* Write vectors to output pointers */
            for (int i = 0; i < op.rw.elems; i++) {
                if (use_vh)
                    cc.st1(vet(vl[i], op), vet(vh[i], op), a64::ptr(out[i]).post(vet.size(op) * 2));
                else
                    cc.st1(vet(vl[i], op),                 a64::ptr(out[i]).post(vet.size(op) * 1));
            }
        } else {
            /* Load output pointer in prologue */
            ctx->to_prologue();
            cc.comment("prologue (write)");
            a64::Gp out = cc.newGpz();
            cc.ldr(out, a64::ptr(exec, offsetof(SwsOpExec, out)));
            ctx->from_prologue();
            /* Write vectors to output pointer */
            switch (op.rw.elems) {
            case 1:
                if (use_vh)
                    cc.st1(vet(vl[0], op), vet(vh[0], op),                                 a64::ptr(out).post(vet.size(op) * 2));
                else
                    cc.st1(vet(vl[0], op),                                                 a64::ptr(out).post(vet.size(op) * 1));
                break;
            case 2:
                cc.st2    (vet(vl[0], op), vet(vl[1], op),                                 a64::ptr(out).post(vet.size(op) * 2));
                if (use_vh)
                    cc.st2(vet(vh[0], op), vet(vh[1], op),                                 a64::ptr(out).post(vet.size(op) * 2));
                break;
            case 3:
                cc.st3    (vet(vl[0], op), vet(vl[1], op), vet(vl[2], op),                 a64::ptr(out).post(vet.size(op) * 3));
                if (use_vh)
                    cc.st3(vet(vh[0], op), vet(vh[1], op), vet(vh[2], op),                 a64::ptr(out).post(vet.size(op) * 3));
                break;
            case 4:
{
#if 0
                cc.st4    (vet(vl[0], op), vet(vl[1], op), vet(vl[2], op), vet(vl[3], op), a64::ptr(out).post(vet.size(op) * 4));
                if (use_vh)
                    cc.st4(vet(vh[0], op), vet(vh[1], op), vet(vh[2], op), vet(vh[3], op), a64::ptr(out).post(vet.size(op) * 4));
#else
// TODO help asmjit's register allocator
if (use_vh) {
    cc.st4(vet(vl[0], op), vet(vl[1], op), vet(vl[2], op), vet(vl[3], op), a64::ptr(out).post(vet.size(op) * 4));
    cc.mov(vl[0].b16(), vh[0].b16());
    cc.mov(vl[1].b16(), vh[1].b16());
    cc.mov(vl[2].b16(), vh[2].b16());
    cc.mov(vl[3].b16(), vh[3].b16());
    cc.st4(vet(vl[0], op), vet(vl[1], op), vet(vl[2], op), vet(vl[3], op), a64::ptr(out).post(vet.size(op) * 4));
} else {
    cc.st4(vet(vl[0], op), vet(vl[1], op), vet(vl[2], op), vet(vl[3], op), a64::ptr(out).post(vet.size(op) * 4));
}
#endif
}
                break;
            }
        }
        break;
    case SWS_OP_SWAP_BYTES:      /* swap byte order (for differing endianness) */
        if        (op.type == SWS_PIXEL_U16) {
            cc.comment("swap_bytes (u16)");
            LOOP_USED(i) {
                cc.rev16    (vl[i].b16(), vl[i].b16());
                if (use_vh)
                    cc.rev16(vh[i].b16(), vh[i].b16());
            }
        } else if (op.type == SWS_PIXEL_U32 || op.type == SWS_PIXEL_F32) {
            cc.comment("swap_bytes (u32)");
            LOOP_USED(i) {
                cc.rev32    (vl[i].b16(), vl[i].b16());
                if (use_vh)
                    cc.rev32(vh[i].b16(), vh[i].b16());
            }
        } else {
            return AVERROR(ENOTSUP);
        }
        break;
    case SWS_OP_UNPACK:          /* split tightly packed data into components */
        /* TODO this function is a mess */
        {
            int offsets[4] = {
                op.pack.pattern[3] + op.pack.pattern[2] + op.pack.pattern[1],
                op.pack.pattern[3] + op.pack.pattern[2],
                op.pack.pattern[3],
                0
            };

            use_vh = ((op.pack.type == SWS_PIXEL_U16) && chain->block_w == 16)
                  || ((op.pack.type == SWS_PIXEL_U32) && chain->block_w == 8)
                  || ((op.pack.type == SWS_PIXEL_F32) && chain->block_w == 8);

            /* Override op.comps.unused so that I can use LOOP_USED */
            for (int i = 0; i < 4; i++) {
                op.comps.unused[i] = !op.pack.pattern[i];
            }

            cc.comment("unpack");
            save_vector(ctx, 0);
            LOOP_USED(i) {
                new_vector(ctx, i);
                if (offsets[i]) {
                    cc.ushr    (vet(vl[i], *prev), vet(orig_vl[0], *prev), offsets[i]);
                    if (use_vh)
                        cc.ushr(vet(vh[i], *prev), vet(orig_vh[0], *prev), offsets[i]);
                } else {
                    vl[i] = orig_vl[0];
                    if (use_vh)
                        vh[i] = orig_vh[0];
                }
            }
            LOOP_USED(i) {
                uint32_t mask = (1u << op.pack.pattern[i]) - 1;
                a64::Vec vmask = cc.newVecQ();
                if (mask <= 255) {
                    cc.movi(vet(vmask, *prev), mask);
                } else {
                    a64::Gp rmask = cc.newGpz();
                    cc.mov(rmask, (1 << op.pack.pattern[i]) - 1);
                    cc.dup(vet(vmask, *prev), rmask);
                }
                cc.and_    (vl[i].b16(), vl[i].b16(), vmask.b16());
                if (use_vh)
                    cc.and_(vh[i].b16(), vh[i].b16(), vmask.b16());
            }
            /* TODO improve! */
            if (op.type != op.pack.type && emit_convert(ctx, chain, op, op.pack.type, op.type, false) < 0)
                return AVERROR(ENOTSUP);
        }
        break;
    case SWS_OP_PACK:            /* compress components into tightly packed data */
        {
            int offsets[4] = {
                op.pack.pattern[3] + op.pack.pattern[2] + op.pack.pattern[1],
                op.pack.pattern[3] + op.pack.pattern[2],
                op.pack.pattern[3],
                0
            };

            use_vh = ((next->type == SWS_PIXEL_U16) && chain->block_w == 16)
                  || ((next->type == SWS_PIXEL_U32) && chain->block_w == 8)
                  || ((next->type == SWS_PIXEL_F32) && chain->block_w == 8);

            cc.comment("pack");
            /* TODO ushll instead */
            if (op.type != op.pack.type && emit_convert(ctx, chain, op, op.type, op.pack.type, false) < 0)
                return AVERROR(ENOTSUP);
            LOOP_USED(i) {
                if (offsets[i]) {
                    cc.shl    (vet(vl[i], *next), vet(vl[i], *next), offsets[i]);
                    if (use_vh)
                        cc.shl(vet(vh[i], *next), vet(vh[i], *next), offsets[i]);
                }
            }
            LOOP_USED(i) {
                if (i != 0) {
                    cc.orr    (vl[0].b16(), vl[0].b16(), vl[i].b16());
                    if (use_vh)
                        cc.orr(vh[0].b16(), vh[0].b16(), vh[i].b16());
                }
            }
        }
        break;
    /* Pixel manipulation */
    case SWS_OP_CLEAR:           /* clear pixel values */
        /* Create output vectors */
        for (int i = 0; i < 4; i++) {
            if (op.clear.value[i].den) {
                vl[i] = cc.newVecQ();
                if (use_vh)
                    vh[i] = cc.newVecQ();
            }
        }
        /* Set vectors to constant value */
        if (op.type == SWS_PIXEL_U8 || op.type == SWS_PIXEL_U16 || op.type == SWS_PIXEL_U32) {
            cc.comment("clear (integer)");
            for (int i = 0; i < 4; i++) {
                if (op.clear.value[i].den) {
                    int val = op.clear.value[i].num / op.clear.value[i].den;
                    if (val <= 255) {
                        cc.movi(vet(vl[i], op), val);
                        if (use_vh)
                            cc.movi(vet(vh[i], op), val);
                    } else {
                        /* TODO load tmp only once if possible */
                        a64::Gp tmp = cc.newGpw();
                        cc.mov(tmp, val);
                        cc.dup(vet(vl[i], op), tmp);
                        if (use_vh)
                            cc.dup(vet(vh[i], op), tmp);
                    }
                }
            }
        } else if (op.type == SWS_PIXEL_F32) {
            /* Write const data after function */
            float fdata[4];
            for (int i = 0; i < 4; i++)
                fdata[i] = av_q2d(op.clear.value[i]);
            Label ldata = emit_data(ctx, fdata, sizeof(fdata));

            /* Read matrix data into vectors */
            ctx->to_prologue();
            cc.comment("prologue (clear)");
            read_vdata(ctx, ldata, 1);
            ctx->from_prologue();

            /* Do the salmon dance */
            cc.comment("clear (f32)");
            for (int i = 0; i < 4; i++) {
                if (op.clear.value[i].den) {
                    cc.dup(vl[i].s4(), vdata[0].s(i));
                    cc.dup(vh[i].s4(), vdata[0].s(i));
                }
            }
        } else {
            return AVERROR(ENOTSUP);
        }
        break;
    case SWS_OP_LSHIFT:          /* logical left shift of raw pixel values */
        if (op.type == SWS_PIXEL_U8 || op.type == SWS_PIXEL_U16 || op.type == SWS_PIXEL_U32) {
            cc.comment("lshift");
            refresh_vectors_used(ctx, op);
            LOOP_USED(i) {
                cc.shl    (vet(vl[i], op), vet(orig_vl[i], op), op.shift.amount);
                if (use_vh)
                    cc.shl(vet(vh[i], op), vet(orig_vh[i], op), op.shift.amount);
            }
        }
        break;
#if 0
    case SWS_OP_RSHIFT:          /* right shift of raw pixel values */
        break;
#endif
    case SWS_OP_SWIZZLE:         /* rearrange channel order, or duplicate channels */
        {
            bool reorder = true;
            bool used[4] = { false, false, false, false };
            for (int i = 0; i < 4; i++) {
                if (used[op.swizzle.in[i]]) {
                    reorder = false;
                    break;
                }
                used[op.swizzle.in[i]] = true;
            }

            save_vectors_count(ctx, 4);

            if (reorder) {
                cc.comment("swizzle (reorder)");
                /* It shouldn't matter if the vectors are initialized or not */
                for (int i = 0; i < 4; i++) {
                    vl[i] = orig_vl[op.swizzle.in[i]];
                    vh[i] = orig_vh[op.swizzle.in[i]];
                }
            } else {
                cc.comment("swizzle (copy)");

                /* Create output vectors */
                new_vectors_mask(ctx, use_vh ? 0xff : 0x0f);

                for (int i = 0; i < 4; i++) {
                    if (op.comps.unused[op.swizzle.in[i]])
                        continue;
                    if (i == op.swizzle.in[i]) {
                        vl[i] = orig_vl[op.swizzle.in[i]];
                        if (use_vh)
                            vh[i] = orig_vh[op.swizzle.in[i]];
                    } else {
                        cc.mov(vl[i].b16(), orig_vl[op.swizzle.in[i]].b16());
                        if (use_vh)
                            cc.mov(vh[i].b16(), orig_vh[op.swizzle.in[i]].b16());
                    }
                }
            }
        }
        break;
    case SWS_OP_CONVERT:         /* convert (cast) between formats */
        if (emit_convert(ctx, chain, op, op.type, op.convert.to, op.convert.expand) < 0)
            return AVERROR(ENOTSUP);
        break;
    case SWS_OP_DITHER:          /* add dithering noise */
        {
            cc.comment("dither");

            /* Write const data after function */
            int size = 1 << op.dither.size_log2;
            std::vector<float> fdata;
            fdata.resize(size * size);
            for (int i = 0; i < size * size; i++) {
                fdata[i] = av_q2d(op.dither.matrix[i]);
            }
            Label ldata = emit_data(ctx, fdata.data(), size * size * sizeof(float));

            a64::Gp rdatal = cc.newGpz();
            a64::Gp rdatah = cc.newGpz();
            a64::Gp x = cc.newGpz();
            a64::Gp y = cc.newGpz();

            cc.adr(rdatal, ldata);
            cc.ldr(x.r32(), a64::ptr(exec, offsetof(SwsOpExec, x)));
            cc.ldr(y.r32(), a64::ptr(exec, offsetof(SwsOpExec, y)));
            /* x = (x & ((1 << size_log2) - 1)) * sizeof(float32) */
            cc.ubfiz(x, x, 2, op.dither.size_log2);
            cc.add(rdatah, rdatal, 16);
            cc.add(rdatal, rdatal, x);
            cc.add(rdatah, rdatah, x);

            static const int y_off[4] = { 0, 3, 5, 7 };
            LOOP_USED(i) {
                // offset = ((((y + yoff[i]) & mask) << log2_size) + (x & mask)) * sizeof(float32);

                a64::Gp ry_off = cc.newGpz();
                a64::Gp ptrl = cc.newGpz();
                a64::Gp ptrh = cc.newGpz();
                a64::Vec dither_vl = cc.newVecQ();
                a64::Vec dither_vh = cc.newVecQ();

                if (y_off[i] == 0) {
                    cc.ubfiz(ry_off, y, op.dither.size_log2 + 2, op.dither.size_log2);
                } else {
                    cc.add(ry_off, y, y_off[i]);
                    cc.ubfiz(ry_off, ry_off, op.dither.size_log2 + 2, op.dither.size_log2);
                }
                cc.add(ptrl, rdatal, ry_off);
                cc.add(ptrh, rdatah, ry_off);

                cc.ld1(dither_vl.s4(), a64::ptr(ptrl));
                cc.ld1(dither_vh.s4(), a64::ptr(ptrh));
                cc.fadd(vl[i].s4(), vl[i].s4(), dither_vl.s4());
                cc.fadd(vh[i].s4(), vh[i].s4(), dither_vh.s4());
            }
        }
        break;
    case SWS_OP_CLAMP:           /* clamp pixel values to value range */
        if (next->op == SWS_OP_CONVERT && next->type == SWS_PIXEL_F32 && next->convert.to == SWS_PIXEL_U8) {
            LOOP_USED(i) {
                if (av_cmp_q(op.clamp.max[i], (AVRational) {255, 1}) != 0)
                    goto normal_clamp;
            }

            cc.comment("convert+clamp");
            if (emit_convert(ctx, chain, op, op.type, SWS_PIXEL_U16, false) < 0)
                return AVERROR(ENOTSUP);
            /* Saturating convert from u16 to u8 */
            LOOP_USED(i) {
                refresh_vector(ctx, i, 0x0f);
                cc.uqxtn(vl[i].b8(), orig_vl[i].h8());
            }
            ops->ops++;
            ops->num_ops--;
        } else if (next->op == SWS_OP_CONVERT && next->type == SWS_PIXEL_F32 && next->convert.to == SWS_PIXEL_U16) {
            LOOP_USED(i) {
                if (av_cmp_q(op.clamp.max[i], (AVRational) {65535, 1}) != 0)
                    goto normal_clamp;
            }

            cc.comment("convert+clamp");
            if (emit_convert(ctx, chain, op, op.type, SWS_PIXEL_U32, false) < 0)
                return AVERROR(ENOTSUP);
            /* Saturating convert from u32 to u16 */
            LOOP_USED(i) {
                refresh_vector(ctx, i);
                cc.uqxtn(vl[i].h4(), orig_vl[i].s4());
                cc.uqxtn(vh[i].h4(), orig_vh[i].s4());
            }
            LOOP_USED(i) {
                cc.ins(vl[i].d(1), vh[i].d(0));
            }
            ops->ops++;
            ops->num_ops--;
        } else {
normal_clamp:
            cc.comment("clamp");
            a64::Vec vmin = cc.newVecQ();
            a64::Vec vmax = cc.newVecQ();
            cc.movi(vmin.s4(), 0);
            AVRational last_max = { -1, 0 };
            LOOP_USED(i) {
                if (op.clamp.max[i].den) {
                    if (av_cmp_q(last_max, op.clamp.max[i]) != 0) {
                        int val = av_q2d(op.clamp.max[i]);
                        if (val <= 255) {
                            cc.movi(vmax.s4(), val);
                        } else {
                            a64::Gp tmp = cc.newGpw();
                            cc.mov(tmp, val);
                            cc.dup(vmax.s4(), tmp);
                        }
                        cc.ucvtf(vmax.s4(), vmax.s4());
                        last_max = op.clamp.max[i];
                    }
                    cc.fmax(vl[i].s4(), vl[i].s4(), vmin.s4());
                    cc.fmax(vh[i].s4(), vh[i].s4(), vmin.s4());
                    cc.fmin(vl[i].s4(), vl[i].s4(), vmax.s4());
                    cc.fmin(vh[i].s4(), vh[i].s4(), vmax.s4());
                }
            }
        }
        break;
    /* Arithmetic operations */
    case SWS_OP_LINEAR:          /* generalized linear affine transform */
#if 0
printf("[%08x][%08x]\n", op.lin.mask, SWS_MASK_MAT3 | SWS_MASK_OFF3);
#define Q(N) ((AVRational) { N, 1 })
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 5; j++) {
            printf(" [% 9d / % 9d]", op.lin.m[i][j].num, op.lin.m[i][j].den);
        }
        printf("\n");
    }
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 5; j++) {
            if (op.lin.m[i][j].num && av_cmp_q(op.lin.m[i][j], Q(i == j)))
//            if (av_cmp_q(op.lin.m[i][j], Q(i == j)))
                printf("x");
            else
                printf("_");
        }
        printf("\n");
    }
#endif
        {
            /* Start with offset and the coeffs */
            const int fdata_swizzle[5] = { 4, 0, 1, 2, 3 };

            /* Check which vectors are used after this operation */
            int used[4] = { 0 };
            LOOP_USED(i) {
                used[i] = 1;
            }
            for (int i = 0; i < 4; i++) {
                bool is_identity = true;
                if (!used[i])
                    continue;
                for (int j = 0; j < 5; j++) {
                    if (i == j) {
                        if (op.lin.m[i][j].num != 1 || op.lin.m[i][j].den != 1) {
                            is_identity = false;
                            break;
                        }
                    } else {
                        if (op.lin.m[i][j].num != 0) {
                            is_identity = false;
                            break;
                        }
                    }
                }
                if (is_identity)
                    used[i] = 0;
            }

            /* Write const data after function */
            std::vector<AVRational> qdata;
            std::vector<float> fdata;
            bool identity[4][5];
            int vpos[4][5];
            for (int i = 0; i < 4; i++) {
                for (int j = 0; j < 5; j++) {
                    int sj = fdata_swizzle[j];
                    if (op.lin.m[i][sj].num) {
                        bool repeated = false;
                        for (size_t k = 0; k < fdata.size(); k++) {
                            if (av_cmp_q(qdata[k], op.lin.m[i][sj]) == 0) {
                                vpos[i][sj] = k;
                                repeated = true;
                                break;
                            }
                        }
                        if (!repeated) {
                            vpos[i][sj] = fdata.size();
                            fdata.push_back(av_q2d(op.lin.m[i][sj]));
                            qdata.push_back(op.lin.m[i][sj]);
                        }
                        /* TODO don't emit identity data */
                        identity[i][sj] = (op.lin.m[i][sj].num == 1 && op.lin.m[i][sj].den == 1);
                    } else {
                        vpos[i][sj] = -1;
                        identity[i][sj] = false;
                    }
                }
            }
            Label ldata = emit_data(ctx, fdata.data(), fdata.size() * sizeof(float));

            /* Read matrix data into vectors */
            ctx->to_prologue();
            cc.comment("prologue (linear)");
            int vdata_count = (fdata.size() + 3) >> 2;
            read_vdata(ctx, ldata, vdata_count);
            ctx->from_prologue();

            /* Do the salmon dance */
            cc.comment("linear");
            refresh_vectors_count(ctx, 4);
            for (int i = 0; i < 4; i++) {
                if (!used[i]) {
                    vl[i] = orig_vl[i];
                    vh[i] = orig_vh[i];
                    continue;
                }
                int count = 0;
                for (int j = 0; j < 5; j++) {
                    int sj = fdata_swizzle[j];
                    int vidx = vpos[i][sj];
                    if (vidx != -1) {
                        int vdata_i = vidx >> 2;
                        int vdata_j = vidx & 3;
                        if (j == 0)
                            cc.dup(vl[i].s4(), vdata[vdata_i].s(vdata_j));
                        else if (count == 0 && identity[i][sj])
                            cc.mov(vl[i].s4(), orig_vl[sj].s4());
                        else if (count == 0)
                            cc.fmul(vl[i].s4(), orig_vl[sj].s4(), vdata[vdata_i].s(vdata_j));
                        else
                            cc.fmla(vl[i].s4(), orig_vl[sj].s4(), vdata[vdata_i].s(vdata_j));
                        count++;
                    }
                }
                count = 0;
                for (int j = 0; j < 5; j++) {
                    int sj = fdata_swizzle[j];
                    int vidx = vpos[i][sj];
                    if (vidx != -1) {
                        int vdata_i = vidx >> 2;
                        int vdata_j = vidx & 3;
                        if (j == 0)
                            cc.dup(vh[i].s4(), vdata[vdata_i].s(vdata_j));
                        else if (count == 0 && identity[i][sj])
                            cc.mov(vh[i].s4(), orig_vh[sj].s4());
                        else if (count == 0)
                            cc.fmul(vh[i].s4(), orig_vh[sj].s4(), vdata[vdata_i].s(vdata_j));
                        else
                            cc.fmla(vh[i].s4(), orig_vh[sj].s4(), vdata[vdata_i].s(vdata_j));
                        count++;
                    }
                }
            }
        }
        break;
    case SWS_OP_SCALE:           /* multiplication by scalar */
        if (op.type == SWS_PIXEL_F32) {
            /* Write const data after function */
            float fdata[1];
            fdata[0] = av_q2d(op.scale.factor);
            Label ldata = emit_data(ctx, fdata, sizeof(fdata));

            /* Read matrix data into vectors */
            ctx->to_prologue();
            cc.comment("prologue (scale)");
            vdata[0] = cc.newVecQ();
            a64::Gp rdata = cc.newGpz();
            cc.adr(rdata, ldata);
            cc.ld1r(vdata[0].s4(), a64::ptr(rdata));
            ctx->from_prologue();

            /* Do the salmon dance */
            cc.comment("scale (f32)");
            refresh_vectors_used(ctx, op);
            LOOP_USED(i) {
                cc.fmul(vl[i].s4(), orig_vl[i].s4(), vdata[0].s4());
                cc.fmul(vh[i].s4(), orig_vh[i].s4(), vdata[0].s4());
            }
        } else if (op.type == SWS_PIXEL_U8 || op.type == SWS_PIXEL_U16 || op.type == SWS_PIXEL_U32) {
            cc.comment("scale (integer)");

            int32_t factor = op.scale.factor.num / op.scale.factor.den;
            vdata[0] = cc.newVecQ();
            if (factor <= 255) {
                cc.movi(vet(vdata[0], op), factor);
            } else {
                a64::Gp tmp = cc.newGpz();
                cc.mov(tmp, factor);
                cc.dup(vet(vdata[0], op), tmp);
            }

            /* Do the salmon dance */
            refresh_vectors_used(ctx, op);
            LOOP_USED(i) {
                cc.mul(vet(vl[i], op), vet(orig_vl[i], op), vet(vdata[0], op));
                cc.mul(vet(vh[i], op), vet(orig_vh[i], op), vet(vdata[0], op));
            }
        } else {
            return AVERROR(ENOTSUP);
        }
        break;

    default:
        return AVERROR(ENOTSUP);
    }

    ops->ops++;
    ops->num_ops--;
    return ops->num_ops ? AVERROR(EAGAIN) : 0;
}

static av_cold void free_context(void *_ctx)
{
    AsmJitContext *ctx = static_cast<AsmJitContext *>(_ctx);
    if (ctx->m_cc)
        delete(ctx->m_cc);
    delete ctx;
}

static av_cold int asmjit_compile(SwsOpList *ops, SwsOpChain *chain)
{
    AsmJitContext *ctx = new AsmJitContext;
    a64::Compiler &cc = *ctx->m_cc;
    Error err;
    int ret;

    /* Use at most two full vregs during the widest precision section */
    chain->block_w = (ff_sws_op_list_max_size(ops) == 4) ? 8 : 16;
    chain->block_h = 1;

    do {
        ret = asmjit_compile_op(ctx, ops, chain);
    } while (ret == AVERROR(EAGAIN));
    if (ret < 0)
        goto error;

    cc.ret();

    err = cc.endFunc();
    if (err) {
        std::cout << "Failed to end function: " << DebugUtils::errorAsString(err) << "\n";
        goto error;
    }
    err = cc.finalize();
    if (err) {
        std::cout << "Failed to finalize code: " << DebugUtils::errorAsString(err) << "\n";
        goto error;
    }

    ctx->m_rt.add(&chain->entry, &ctx->m_code);
    if (chain->entry == nullptr)
        goto error;

    // At this point, logger already contains the output
    if (av_log_get_level() >= AV_LOG_DEBUG)
        std::cout << ctx->m_logger.data() << "\n";

    chain->impl[0].priv.ptr = ctx;
    chain->free[0] = free_context;
    chain->num_impl++;

    return 0;

error:
    free_context(ctx);
    return AVERROR(ENOTSUP);
}

SwsOpBackend backend_asmjit = {
    .name    = "asmjit",
    .compile = asmjit_compile,
};
