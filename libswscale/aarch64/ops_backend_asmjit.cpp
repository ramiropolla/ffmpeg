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
    a64::Vec m_vec[4];

    AsmJitContext()
    {
        m_code.init(m_rt.environment(), m_rt.cpuFeatures());
        m_code.setLogger(&m_logger);
        m_cc = new a64::Compiler(&m_code);
        a64::Compiler &cc = *m_cc;
        cc.addDiagnosticOptions(DiagnosticOptions::kRAAnnotate);
        m_func = cc.addFunc(FuncSignature::build<void, uint8_t *, uint8_t *, uint8_t *, uint8_t *>());
        m_exec = cc.newGpz();
        m_func->setArg(0, m_exec);
        for (int i = 0; i < 4; i++)
            m_vec[i] = cc.newVecQ();
    }
};

static void *alloc_context(void)
{
    AsmJitContext *ctx = new AsmJitContext;
    return (void *) ctx;
}

static void *compile_end(void *_ctx)
{
    AsmJitContext *ctx = static_cast<AsmJitContext *>(_ctx);
    a64::Compiler &cc = *ctx->m_cc;

    cc.ret();

    // move prologue instructions
    cc.setCursor(cc.firstNode()->next());
    for (BaseNode *node: ctx->m_prologue) {
        cc.removeNode(node);
        cc.addNode(node);
    }

    cc.endFunc();
    cc.finalize();

    void *ptr = nullptr;
    ctx->m_rt.add(&ptr, &ctx->m_code);

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

static int vpost(const SwsOp &op)
{
    if (op.type == SWS_PIXEL_U8)
        return op.vcount;
    if (op.type == SWS_PIXEL_U16)
        return op.vcount * 2;
    return op.vcount * 4;
}

static a64::Vec vop(const SwsOp &op, const a64::Vec &src)
{
    if (op.type == SWS_PIXEL_U8)
        return src.b16();
    if (op.type == SWS_PIXEL_U16)
        return src.h8();
    return src.s4();
}

static int compile_asmjit(void *_ctx, SwsOpList *ops, SwsCompiledOp *out_compiled)
{
    AsmJitContext *ctx = static_cast<AsmJitContext *>(_ctx);
    a64::Compiler &cc = *ctx->m_cc;
    a64::Gp &exec = ctx->m_exec;
    a64::Vec *v = ctx->m_vec;

    SwsOp op = ops->ops[0];
    switch (op.op) {
    /* Input/output handling */
    case SWS_OP_READ:            /* gather raw pixels from planes */
        if (op.rw.planar) {
            a64::Gp in[4];
            for (int i = 0; i < op.rw.elems; i++) {
                in[i] = cc.newGpz();
                cc.ldr(in[i], a64::ptr(exec, offsetof(SwsOpExec, in) + offsetof(SwsImg, data) + sizeof(uint8_t *) * i));
                ctx->m_prologue.push_back(cc.cursor());
            }
            for (int i = 0; i < op.rw.elems; i++)
                cc.ld1(vop(op, v[i]), a64::ptr(in[i]).post(vpost(op)));
        } else {
            a64::Gp in = cc.newGpz();
            cc.ldr(in, a64::ptr(exec, offsetof(SwsOpExec, in) + offsetof(SwsImg, data)));
            ctx->m_prologue.push_back(cc.cursor());
            switch (op.rw.elems) {
            case 1: cc.ld1(vop(op, v[0]),                                              a64::ptr(in).post(vpost(op) * 1)); break;
            case 2: cc.ld2(vop(op, v[0]), vop(op, v[1]),                               a64::ptr(in).post(vpost(op) * 2)); break;
            case 3: cc.ld3(vop(op, v[0]), vop(op, v[1]), vop(op, v[2]),                a64::ptr(in).post(vpost(op) * 3)); break;
            case 4: cc.ld4(vop(op, v[0]), vop(op, v[1]), vop(op, v[2]), vop(op, v[3]), a64::ptr(in).post(vpost(op) * 4)); break;
            }
        }
        break;
    case SWS_OP_WRITE:           /* write raw pixels to planes */
        if (op.rw.planar) {
            a64::Gp out[4];
            for (int i = 0; i < op.rw.elems; i++) {
                out[i] = cc.newGpz();
                cc.ldr(out[i], a64::ptr(exec, offsetof(SwsOpExec, out) + offsetof(SwsImg, data) + sizeof(uint8_t *) * i));
                ctx->m_prologue.push_back(cc.cursor());
            }
            for (int i = 0; i < op.rw.elems; i++)
                cc.st1(vop(op, v[i]), a64::ptr(out[i]).post(vpost(op)));
        } else {
            a64::Gp out = cc.newGpz();
            cc.ldr(out, a64::ptr(exec, offsetof(SwsOpExec, out) + offsetof(SwsImg, data)));
            ctx->m_prologue.push_back(cc.cursor());
            switch (op.rw.elems) {
            case 1: cc.st1(vop(op, v[0]),                                              a64::ptr(out).post(vpost(op) * 1)); break;
            case 2: cc.st2(vop(op, v[0]), vop(op, v[1]),                               a64::ptr(out).post(vpost(op) * 2)); break;
            case 3: cc.st3(vop(op, v[0]), vop(op, v[1]), vop(op, v[2]),                a64::ptr(out).post(vpost(op) * 3)); break;
            case 4: cc.st4(vop(op, v[0]), vop(op, v[1]), vop(op, v[2]), vop(op, v[3]), a64::ptr(out).post(vpost(op) * 4)); break;
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
    /* Pixel manipulation */
    case SWS_OP_CLEAR:           /* clear pixel values */
        break;
    case SWS_OP_LSHIFT:          /* logical left shift of raw pixel values */
        break;
    case SWS_OP_RSHIFT:          /* right shift of raw pixel values */
        break;
#endif
    case SWS_OP_SWIZZLE:         /* rearrange channel order, or duplicate channels */
        {
            a64::Vec orig[4] = { v[0], v[1], v[2], v[3] };
            for (int i = 0; i < 4; i++)
                v[i] = orig[op.swizzle.in[i]];
        }
        break;
#if 0
    case SWS_OP_CONVERT:         /* convert (cast) between formats */
        break;
    case SWS_OP_DITHER:          /* add dithering noise */
        break;
    case SWS_OP_CLAMP:           /* clamp pixel values to value range */
        break;
    /* Arithmetic operations */
    case SWS_OP_LINEAR:          /* generalized linear affine transform */
        break;
    case SWS_OP_SCALE:           /* multiplication by scalar */
        break;
#else
    default:
        return AVERROR(ENOTSUP);
#endif
    }

    *out_compiled = (SwsCompiledOp) {
        .chunk_size = 16,
        .alignment  = 16,
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
