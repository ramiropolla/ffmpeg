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
#define LOG_ASMJIT

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
    std::vector<BaseNode *> m_prologue;
    a64::Gp m_exec;
    a64::Vec m_vecl[4];
    a64::Vec m_vech[4];

    AsmJitContext()
    {
        m_code.init(m_rt.environment(), m_rt.cpuFeatures());
#ifdef LOG_ASMJIT
        m_code.setLogger(&m_logger);
#endif
        m_cc = new a64::Compiler(&m_code);
        a64::Compiler &cc = *m_cc;
        cc.addDiagnosticOptions(DiagnosticOptions::kRAAnnotate);
        m_func = cc.addFunc(FuncSignature::build<void, uint8_t *, uint8_t *, uint8_t *, uint8_t *>());
#ifdef EMIT_BRK
       cc.brk(0xf000);
       m_prologue.push_back(cc.cursor());
#endif
        m_exec = cc.newGpz();
        m_func->setArg(0, m_exec);
#if 0
        for (int i = 0; i < 4; i++) {
            m_vecl[i] = cc.newVecQ();
            m_vech[i] = cc.newVecQ();
        }
#endif
    }
};

static void *alloc_context(void)
{
    AsmJitContext *ctx = new AsmJitContext;
    return (void *) ctx;
}

#include <fstream>
#include <iomanip>
#include <unistd.h>
#include <sstream>

static void *compile_end(void *_ctx)
{
    AsmJitContext *ctx = static_cast<AsmJitContext *>(_ctx);
    a64::Compiler &cc = *ctx->m_cc;
    Error err;

    cc.ret();

    // move prologue instructions
    cc.setCursor(cc.firstNode()->next());
    for (BaseNode *node: ctx->m_prologue) {
        cc.removeNode(node);
        cc.addNode(node);
    }

    err = cc.endFunc();
    if (err) {
        std::cerr << "Failed to end function: " << DebugUtils::errorAsString(err) << "\n";
    }
    err = cc.finalize();
    if (err) {
        std::cerr << "Failed to finalize code: " << DebugUtils::errorAsString(err) << "\n";
    }

    void *ptr = nullptr;
    ctx->m_rt.add(&ptr, &ctx->m_code);

{
size_t funcSize = ctx->m_code.codeSize();
const char *name = "ffjit";

    std::ostringstream path;
    path << "/tmp/perf-" << getpid() << ".map";

    std::ofstream file(path.str(), std::ios::app); // append
    if (file.is_open()) {
        file << std::hex << reinterpret_cast<uintptr_t>(ptr) << ' '
             << std::hex << funcSize << ' '
             << name << '\n';
    }
}
    // At this point, logger already contains the output
    std::cout << ctx->m_logger.data() << "\n";

    return ptr;
}

static void free_context(void *_ctx)
{
    AsmJitContext *ctx = static_cast<AsmJitContext *>(_ctx);
    if (ctx->m_cc)
        delete(ctx->m_cc);
    delete ctx;
}

static int vsize(const SwsOp &op)
{
    int elsize = (op.type == SWS_PIXEL_U8)  ? 1
               : (op.type == SWS_PIXEL_U16) ? 2
               :                              4;
    int ret = op.vcount * elsize;
    return FFMIN(ret, 16);
}

static a64::Vec vop(const SwsOp &op, const a64::Vec &src)
{
    if (op.type == SWS_PIXEL_U8  && op.vcount == 8)
        return src.b8(); /* half vector */
    if (op.type == SWS_PIXEL_U16 && op.vcount == 8)
        return src.h8(); /* full vector */
    if (op.type == SWS_PIXEL_U32 && op.vcount == 8)
        return src.s4(); /* full vector (TRUNCATE) */
    if (op.type == SWS_PIXEL_F32 && op.vcount == 8)
        return src.s4(); /* full vector (TRUNCATE) */
    if (op.type == SWS_PIXEL_U8  && op.vcount == 16)
        return src.b16(); /* full vector */
    if (op.type == SWS_PIXEL_U16 && op.vcount == 16)
        return src.h8(); /* full vector (TRUNCATE) */
    if (op.type == SWS_PIXEL_U32 && op.vcount == 16)
        return src.s4(); /* full vector (TRUNCATE) */
    if (op.type == SWS_PIXEL_F32 && op.vcount == 16)
        return src.s4(); /* full vector (TRUNCATE) */
    printf("ERORROORORRORORO %d %d\n", op.type, op.vcount);
    return src.b16();
}

static int compile_asmjit(void *_ctx, SwsOpList *ops, SwsCompiledOp *out_compiled)
{
    AsmJitContext *ctx = static_cast<AsmJitContext *>(_ctx);
    a64::Compiler &cc = *ctx->m_cc;
    a64::Gp &exec = ctx->m_exec;
    a64::Vec *vl = ctx->m_vecl;
    a64::Vec *vh = ctx->m_vech;

    const SwsOp &op = ops->ops[0];
    int vcount = op.vcount;
    bool use_vh = ((op.type == SWS_PIXEL_U16) && op.vcount == 16)
               || ((op.type == SWS_PIXEL_U32) && op.vcount == 8)
               || ((op.type == SWS_PIXEL_F32) && op.vcount == 8);
    switch (op.op) {
    /* Input/output handling */
    case SWS_OP_READ:            /* gather raw pixels from planes */
        cc.comment("read");
        if (op.rw.planar) {
            /* Load input pointers in prologue */
            cc.comment("prologue (read)");
            ctx->m_prologue.push_back(cc.cursor());
            a64::Gp in[4];
            for (int i = 0; i < op.rw.elems; i++) {
                in[i] = cc.newGpz();
                cc.ldr(in[i], a64::ptr(exec, offsetof(SwsOpExec, in) + offsetof(SwsImg, data) + sizeof(uint8_t *) * i));
                ctx->m_prologue.push_back(cc.cursor());
            }
            /* Create input vectors */
            for (int i = 0; i < op.rw.elems; i++) {
                vl[i] = cc.newVecQ();
                if (use_vh)
                    vh[i] = cc.newVecQ();
            }
            /* Read vectors from input pointers */
            for (int i = 0; i < op.rw.elems; i++) {
                if (use_vh)
                    cc.ld1(vop(op, vl[i]), vop(op, vh[i]), a64::ptr(in[i]).post(vsize(op) * 2));
                else
                    cc.ld1(vop(op, vl[i]),                 a64::ptr(in[i]).post(vsize(op) * 1));
            }
        } else {
            /* Load input pointer in prologue */
            cc.comment("prologue (read)");
            ctx->m_prologue.push_back(cc.cursor());
            a64::Gp in = cc.newGpz();
            cc.ldr(in, a64::ptr(exec, offsetof(SwsOpExec, in) + offsetof(SwsImg, data)));
            ctx->m_prologue.push_back(cc.cursor());
            /* Create input vectors */
            for (int i = 0; i < op.rw.elems; i++) {
                vl[i] = cc.newVecQ();
                if (use_vh)
                    vh[i] = cc.newVecQ();
            }
            /* Read vectors from input pointer */
            switch (op.rw.elems) {
            case 1:
                if (use_vh)
                    cc.ld1(vop(op, vl[0]), vop(op, vh[0]),                                 a64::ptr(in).post(vsize(op) * 2));
                else
                    cc.ld1(vop(op, vl[0]),                                                 a64::ptr(in).post(vsize(op) * 1));
                break;
            case 2:
                cc.ld2    (vop(op, vl[0]), vop(op, vl[1]),                                 a64::ptr(in).post(vsize(op) * 2));
                if (use_vh)
                    cc.ld2(vop(op, vh[0]), vop(op, vh[1]),                                 a64::ptr(in).post(vsize(op) * 2));
                break;
            case 3:
                cc.ld3    (vop(op, vl[0]), vop(op, vl[1]), vop(op, vl[2]),                 a64::ptr(in).post(vsize(op) * 3));
                if (use_vh)
                    cc.ld3(vop(op, vh[0]), vop(op, vh[1]), vop(op, vh[2]),                 a64::ptr(in).post(vsize(op) * 3));
                break;
            case 4:
                cc.ld4    (vop(op, vl[0]), vop(op, vl[1]), vop(op, vl[2]), vop(op, vl[3]), a64::ptr(in).post(vsize(op) * 4));
                if (use_vh)
                    cc.ld4(vop(op, vh[0]), vop(op, vh[1]), vop(op, vh[2]), vop(op, vh[3]), a64::ptr(in).post(vsize(op) * 4));
                break;
            }
        }
        break;
    case SWS_OP_WRITE:           /* write raw pixels to planes */
        cc.comment("write");
        if (op.rw.planar) {
            /* Load output pointers in prologue */
            cc.comment("prologue (write)");
            ctx->m_prologue.push_back(cc.cursor());
            a64::Gp out[4];
            for (int i = 0; i < op.rw.elems; i++) {
                out[i] = cc.newGpz();
                cc.ldr(out[i], a64::ptr(exec, offsetof(SwsOpExec, out) + offsetof(SwsImg, data) + sizeof(uint8_t *) * i));
                ctx->m_prologue.push_back(cc.cursor());
            }
            /* Write vectors to output pointers */
            for (int i = 0; i < op.rw.elems; i++) {
                if (use_vh)
                    cc.st1(vop(op, vl[i]), vop(op, vh[i]), a64::ptr(out[i]).post(vsize(op) * 2));
                else
                    cc.st1(vop(op, vl[i]),                 a64::ptr(out[i]).post(vsize(op) * 1));
            }
        } else {
            /* Load output pointer in prologue */
            cc.comment("prologue (write)");
            ctx->m_prologue.push_back(cc.cursor());
            a64::Gp out = cc.newGpz();
            cc.ldr(out, a64::ptr(exec, offsetof(SwsOpExec, out) + offsetof(SwsImg, data)));
            ctx->m_prologue.push_back(cc.cursor());
            /* Write vectors to output pointer */
            switch (op.rw.elems) {
            case 1:
                if (use_vh)
                    cc.st1(vop(op, vl[0]), vop(op, vh[0]),                                 a64::ptr(out).post(vsize(op) * 2));
                else
                    cc.st1(vop(op, vl[0]),                                                 a64::ptr(out).post(vsize(op) * 1));
                break;
            case 2:
                cc.st2    (vop(op, vl[0]), vop(op, vl[1]),                                 a64::ptr(out).post(vsize(op) * 2));
                if (use_vh)
                    cc.st2(vop(op, vh[0]), vop(op, vh[1]),                                 a64::ptr(out).post(vsize(op) * 2));
                break;
            case 3:
                cc.st3    (vop(op, vl[0]), vop(op, vl[1]), vop(op, vl[2]),                 a64::ptr(out).post(vsize(op) * 3));
                if (use_vh)
                    cc.st3(vop(op, vh[0]), vop(op, vh[1]), vop(op, vh[2]),                 a64::ptr(out).post(vsize(op) * 3));
                break;
            case 4:
                cc.st4    (vop(op, vl[0]), vop(op, vl[1]), vop(op, vl[2]), vop(op, vl[3]), a64::ptr(out).post(vsize(op) * 4));
                if (use_vh)
                    cc.st4(vop(op, vh[0]), vop(op, vh[1]), vop(op, vh[2]), vop(op, vh[3]), a64::ptr(out).post(vsize(op) * 4));
                break;
            }
        }
        break;
#if 0
    case SWS_OP_SWAP_BYTES:      /* swap byte order (for differing endianness) */
        break;
    case SWS_OP_UNPACK:          /* split tightly packed data into components */
        break;
    case SWS_OP_PACK:            /* compress components into tightly packed data */
        break;
#endif
    /* Pixel manipulation */
    case SWS_OP_CLEAR:           /* clear pixel values */
        cc.comment("clear");
        /* Create output vectors */
        for (int i = 0; i < 4; i++) {
            if (op.clear.value[i].den) {
                vl[i] = cc.newVecQ();
                if (use_vh)
                    vh[i] = cc.newVecQ();
            }
        }
        /* Set vectors to constant value */
        for (int i = 0; i < 4; i++) {
            if (op.clear.value[i].den) {
                int val = op.clear.value[i].num / op.clear.value[i].den;
                cc.movi(vop(op, vl[i]), val);
                if (use_vh)
                    cc.movi(vop(op, vh[i]), val);
            }
        }
        break;
#if 0
    case SWS_OP_LSHIFT:          /* logical left shift of raw pixel values */
        break;
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

            if (reorder) {
                cc.comment("swizzle (reorder)");
                /* It shouldn't matter if the vectors are initialized or not */
                a64::Vec orig_vl[4] = { vl[0], vl[1], vl[2], vl[3] };
                a64::Vec orig_vh[4] = { vh[0], vh[1], vh[2], vh[3] };
                for (int i = 0; i < 4; i++) {
                    vl[i] = orig_vl[op.swizzle.in[i]];
                    vh[i] = orig_vh[op.swizzle.in[i]];
                }
            } else {
                cc.comment("swizzle (copy)");

                /* Create output vectors */
                a64::Vec orig_vl[4] = { vl[0], vl[1], vl[2], vl[3] };
                // a64::Vec orig_vh[4] = { vh[0], vh[1], vh[2], vh[3] };
                for (int i = 0; i < 4; i++) {
                    vl[i] = cc.newVecQ();
                    // vh[i] = cc.newVecQ();
                }

                for (int i = 0; i < 4; i++) {
                    if (op.comps.unused[op.swizzle.in[i]])
                        continue;
                    if (i == op.swizzle.in[i]) {
                        vl[i] = orig_vl[op.swizzle.in[i]];
                    } else {
                        cc.mov(vl[i].b16(), orig_vl[op.swizzle.in[i]].b16());
                    }
                }
            }
        }
        break;
    case SWS_OP_CONVERT:         /* convert (cast) between formats */
        {
            /* Create output vectors */
            a64::Vec orig_vl[4] = { vl[0], vl[1], vl[2], vl[3] };
            a64::Vec orig_vh[4] = { vh[0], vh[1], vh[2], vh[3] };
            for (int i = 0; i < 4; i++) {
                if (!op.comps.unused[i]) {
                    vl[i] = cc.newVecQ();
                    vh[i] = cc.newVecQ();
                }
            }

            cc.comment("convert");

            if        (op.type == SWS_PIXEL_U8 && op.convert.to == SWS_PIXEL_U16 && op.convert.expand && vcount == 8) {
                /* Convert 8 from u8 to u16 (expand) */
                for (int i = 0; i < 4; i++) {
                    if (!op.comps.unused[i]) {
                        cc.zip1(vl[i].b16(), orig_vl[i].b16(), orig_vl[i].b16());
                    }
                }
            } else if (op.type == SWS_PIXEL_U8 && op.convert.to == SWS_PIXEL_U16 && op.convert.expand && vcount == 16) {
                /* Convert 16 from u8 to u16 (expand) */
                for (int i = 0; i < 4; i++) {
                    if (!op.comps.unused[i]) {
                        cc.zip1(vl[i].b16(), orig_vl[i].b16(), orig_vl[i].b16());
                        cc.zip2(vh[i].b16(), orig_vl[i].b16(), orig_vl[i].b16());
                    }
                }
#if 0
// TODO it should expand
            } else if (op.type == SWS_PIXEL_U8 && op.convert.to == SWS_PIXEL_U32 && !op.convert.expand && vcount == 8) {
                /* Convert 8 from u8 to u16 (no expand) */
                for (int i = 0; i < 4; i++) {
                    if (!op.comps.unused[i]) {
                        cc.uxtl(orig_vl[i].h8(), orig_vl[i].b8());
                    }
                }
                /* Convert 8 from u16 to u32 (no expand) */
                for (int i = 0; i < 4; i++) {
                    if (!op.comps.unused[i]) {
                        cc.uxtl (vl[i].s4(), orig_vl[i].h4());
                        cc.uxtl2(vh[i].s4(), orig_vl[i].h8());
                    }
                }
#endif
            } else if (op.type == SWS_PIXEL_U8 && op.convert.to == SWS_PIXEL_F32 && !op.convert.expand && vcount == 8) {
                /* Convert 8 from u8 to u16 (no expand) */
                for (int i = 0; i < 4; i++) {
                    if (!op.comps.unused[i]) {
                        cc.uxtl(orig_vl[i].h8(), orig_vl[i].b8());
                    }
                }
                /* Convert 8 from u16 to u32 (no expand) */
                for (int i = 0; i < 4; i++) {
                    if (!op.comps.unused[i]) {
                        cc.uxtl (vl[i].s4(), orig_vl[i].h4());
                        cc.uxtl2(vh[i].s4(), orig_vl[i].h8());
                    }
                }
                /* Convert from u32 to f32 */
                for (int i = 0; i < 4; i++) {
                    if (!op.comps.unused[i]) {
                        cc.ucvtf(vl[i].s4(), vl[i].s4());
                        cc.ucvtf(vh[i].s4(), vh[i].s4());
                    }
                }
            } else if (op.type == SWS_PIXEL_U16 && op.convert.to == SWS_PIXEL_F32 && !op.convert.expand && vcount == 8) {
                /* Convert 8 from u16 to u32 (no expand) */
                for (int i = 0; i < 4; i++) {
                    if (!op.comps.unused[i]) {
                        cc.uxtl (vl[i].s4(), orig_vl[i].h4());
                        cc.uxtl2(vh[i].s4(), orig_vl[i].h8());
                    }
                }
                /* Convert from u32 to f32 */
                for (int i = 0; i < 4; i++) {
                    if (!op.comps.unused[i]) {
                        cc.ucvtf(vl[i].s4(), vl[i].s4());
                        cc.ucvtf(vh[i].s4(), vh[i].s4());
                    }
                }
            } else if (op.type == SWS_PIXEL_F32 && op.convert.to == SWS_PIXEL_U8 && !op.convert.expand && vcount == 8) {
                /* Convert from f32 to u32 */
                for (int i = 0; i < 4; i++) {
                    if (!op.comps.unused[i]) {
                        cc.fcvtzu(vl[i].s4(), orig_vl[i].s4());
                        cc.fcvtzu(vh[i].s4(), orig_vh[i].s4());
                    }
                }
                /* Convert from u32 to u16 */
                for (int i = 0; i < 4; i++) {
                    if (!op.comps.unused[i]) {
                        cc.xtn(vl[i].h4(), vl[i].s4());
                        cc.xtn(vh[i].h4(), vh[i].s4());
                    }
                }
                /* Convert from u16 to u8 */
                for (int i = 0; i < 4; i++) {
                    if (!op.comps.unused[i]) {
                        cc.xtn(vl[i].b8(), vl[i].h8());
                        cc.xtn(vh[i].b8(), vh[i].h8());
                    }
                }
                /* Merge vl and vh into vl */
                for (int i = 0; i < 4; i++) {
                    if (!op.comps.unused[i]) {
                        cc.zip1(vl[i].s2(), vl[i].s2(), vh[i].s2());
                    }
                }
            } else {
                return AVERROR(ENOTSUP);
            }
        }
        break;
    case SWS_OP_DITHER:          /* add dithering noise */
        cc.comment("dither");
    {
        /* Write matrix data after function */
        Label ldata = cc.newLabel();
        BaseNode *cursor = cc.cursor();
        cc.setCursor(ctx->m_func->endNode()->prev());
        cc.align(AlignMode::kData, 16);
        cc.bind(ldata);
        int size = 1 << op.dither.size_log2;
        std::vector<float> fdata;
        fdata.resize(size * size);
        for (int i = 0; i < size * size; i++) {
            fdata[i] = av_q2d(op.dither.matrix[i]);
        }
        cc.embed(fdata.data(), size * size * sizeof(float));
        cc.setCursor(cursor);

        cc.comment("prologue (dither)");
        ctx->m_prologue.push_back(cc.cursor());

        a64::Gp rdata = cc.newGpz();
        cc.adr(rdata, ldata);
        ctx->m_prologue.push_back(cc.cursor());

        int mask = (size - 1);

        /* x */
        a64::Gp wx = cc.newGpw();
        a64::Gp wy = cc.newGpw();
        a64::Gp x = cc.newGpz();
        a64::Gp y = cc.newGpz();
        cc.ldr(wx, a64::ptr(exec, offsetof(SwsOpExec, x)));
        ctx->m_prologue.push_back(cc.cursor());
        cc.ldr(wy, a64::ptr(exec, offsetof(SwsOpExec, y)));
        ctx->m_prologue.push_back(cc.cursor());
        cc.sxtw(x, wx);
        ctx->m_prologue.push_back(cc.cursor());
        cc.sxtw(y, wy);
        ctx->m_prologue.push_back(cc.cursor());
        cc.and_(x, x, mask);
        ctx->m_prologue.push_back(cc.cursor());

        static const int y_off[4] = { 0, 3, 5, 7 };
        for (int i = 0; i < 4; i++) {
            if (!op.comps.unused[i]) {
                a64::Gp z = cc.newGpz();
                cc.add(z, y, y_off[i]);
                cc.and_(z, z, mask);
                cc.lsl(z, z, op.dither.size_log2);
                cc.add(z, z, x);
                cc.lsl(z, z, 2);
                cc.add(z, z, rdata);
                // offset = ((((y + yoff[i]) & mask) << log2_size) + (x & mask)) * sizeof(float32);

                a64::Vec v = cc.newVecQ();
                cc.ld1(v.s4(), a64::ptr(z).post(16));
                cc.fadd(vl[i].s4(), vl[i].s4(), v.s4());
                cc.ld1(v.s4(), a64::ptr(z));
                cc.fadd(vh[i].s4(), vh[i].s4(), v.s4());
            }
        }
    }
#if 0
{
    int size = 1 << op.dither.size_log2;

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++)
            printf(" %12.9g", av_q2d(op.dither.matrix[y * size + x]));
        printf("\n");
        // for (int x = size; x < SWS_CHUNK_SIZE; x++)
        //     c.matrix[y][x] = c.matrix[y][x % size]; /* pad to chunk size */
    }
}
#endif
        break;
    case SWS_OP_CLAMP:           /* clamp pixel values to value range */
        {
            const SwsOp *next = &ops->ops[1];
            if (next->op == SWS_OP_CONVERT && next->type == SWS_PIXEL_F32 && next->convert.to == SWS_PIXEL_U8) {
                cc.comment("convert+clamp");
                a64::Vec orig_vl[4] = { vl[0], vl[1], vl[2], vl[3] };
                a64::Vec orig_vh[4] = { vh[0], vh[1], vh[2], vh[3] };
                /* Convert from f32 to u32 */
                for (int i = 0; i < 4; i++) {
                    if (!op.comps.unused[i]) {
                        cc.fcvtzu(vl[i].s4(), orig_vl[i].s4());
                        cc.fcvtzu(vh[i].s4(), orig_vh[i].s4());
                    }
                }
                /* Convert from u32 to u16 */
                for (int i = 0; i < 4; i++) {
                    if (!op.comps.unused[i]) {
                        cc.xtn(vl[i].h4(), vl[i].s4());
                        cc.xtn(vh[i].h4(), vh[i].s4());
                    }
                }
                /* Saturating convert from u16 to u8 */
                for (int i = 0; i < 4; i++) {
                    if (!op.comps.unused[i]) {
                        cc.uqxtn(vl[i].b8(), vl[i].h8());
                        cc.uqxtn(vh[i].b8(), vh[i].h8());
                    }
                }
                /* Merge vl and vh into vl */
                for (int i = 0; i < 4; i++) {
                    if (!op.comps.unused[i]) {
                        cc.zip1(vl[i].s2(), vl[i].s2(), vh[i].s2());
                    }
                }
                ops->ops++;
                ops->num_ops--;
                break;
            }
        }
        cc.comment("clamp");
#if 1
        {
            a64::Vec vzer = cc.newVecQ();
            a64::Vec v255 = cc.newVecQ();
            cc.movi(vzer.s4(), 0);
            cc.movi(v255.s4(), 0xff);
            cc.ucvtf(v255.s4(), v255.s4());
            for (int i = 0; i < 4; i++) {
                if (!op.comps.unused[i]) {
                    cc.fmax(vl[i].s4(), vl[i].s4(), vzer.s4());
                    cc.fmax(vh[i].s4(), vh[i].s4(), vzer.s4());
                    cc.fmin(vl[i].s4(), vl[i].s4(), v255.s4());
                    cc.fmin(vh[i].s4(), vh[i].s4(), v255.s4());
                }
            }
        }
#endif
        break;
    /* Arithmetic operations */
    case SWS_OP_LINEAR:          /* generalized linear affine transform */
        cc.comment("linear");
        if (op.lin.mask == (SWS_MASK_MAT3 | SWS_MASK_OFF3)) {
            /* Write matrix data after function */
            Label ldata = cc.newLabel();
            BaseNode *cursor = cc.cursor();
            cc.setCursor(ctx->m_func->endNode()->prev());
            cc.align(AlignMode::kData, 16);
            cc.bind(ldata);
            float fdata[12];
            for (int i = 0; i < 3; i++) {
                fdata[(i * 4) + 0] = av_q2d(op.lin.m[i][4]);
                fdata[(i * 4) + 1] = av_q2d(op.lin.m[i][0]);
                fdata[(i * 4) + 2] = av_q2d(op.lin.m[i][1]);
                fdata[(i * 4) + 3] = av_q2d(op.lin.m[i][2]);
            }
            cc.embed(fdata, sizeof(fdata));
            cc.setCursor(cursor);

            /* Read matrix data into vectors */
            cc.comment("prologue (linear)");
            ctx->m_prologue.push_back(cc.cursor());
            a64::Vec vdata[3];
            for (int i = 0; i < 3; i++)
                vdata[i] = cc.newVecQ();
            a64::Gp rdata = cc.newGpz();
            cc.adr(rdata, ldata);
            ctx->m_prologue.push_back(cc.cursor());
            cc.ld1(vdata[0].b16(), vdata[1].b16(), vdata[2].b16(), a64::ptr(rdata));
            ctx->m_prologue.push_back(cc.cursor());

            /* Create new output vectors */
            a64::Vec orig_vl[3] = { vl[0], vl[1], vl[2] };
            a64::Vec orig_vh[3] = { vh[0], vh[1], vh[2] };
            for (int i = 0; i < 3; i++) {
                vl[i] = cc.newVecQ();
                vh[i] = cc.newVecQ();
            }

            /* Do the salmon dance */
            for (int i = 0; i < 3; i++) {
                cc.dup (vl[i].s4(),                  vdata[i].s(0));
                cc.fmla(vl[i].s4(), orig_vl[0].s4(), vdata[i].s(1));
                cc.fmla(vl[i].s4(), orig_vl[1].s4(), vdata[i].s(2));
                cc.fmla(vl[i].s4(), orig_vl[2].s4(), vdata[i].s(3));
                cc.dup (vh[i].s4(),                  vdata[i].s(0));
                cc.fmla(vh[i].s4(), orig_vh[0].s4(), vdata[i].s(1));
                cc.fmla(vh[i].s4(), orig_vh[1].s4(), vdata[i].s(2));
                cc.fmla(vh[i].s4(), orig_vh[2].s4(), vdata[i].s(3));
            }
        } else {
            return AVERROR(ENOTSUP);
        }
        break;
#if 0
    case SWS_OP_SCALE:           /* multiplication by scalar */
        break;
#endif

    default:
        return AVERROR(ENOTSUP);
    }

    *out_compiled = (SwsCompiledOp) {
        .chunk_size = op.vcount,
        .alignment  = op.vcount,
        .func       = nullptr,
        .func_n     = nullptr,
        .priv       = nullptr,
        .free_priv  = nullptr,
    };

    ops->ops++;
    ops->num_ops--;
    return 0;
}

SwsOpBackend backend_asmjit = {
    .name    = "AsmJitNeon",
    .alloc_context = alloc_context,
    .compile_end = compile_end,
    .free_context = free_context,
    .compile = compile_asmjit,
};
