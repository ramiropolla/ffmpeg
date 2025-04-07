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
#define EMIT_LOOP

extern "C" {
#include "libavutil/cpu.h"

#include "../ops_internal.h"
#include "../ops_backend.h"
}

#include <asmjit/core.h>
#include <asmjit/a64.h>

#include <iostream>
#include <vector>

#define av_q2f(q) ((q).den ? (float) (q).num / (q).den : 0)
#define av_q2i(q) ((q).den ? (int32_t) (q).num / (q).den : 0)

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
    a64::Gp m_in[4];
    a64::Gp m_out[4];
    a64::Vec m_orig_vl[4];
    a64::Vec m_orig_vh[4];
    a64::Vec m_vl[4];
    a64::Vec m_vh[4];

    a64::Gp m_x;
    a64::Gp m_y;
    a64::Gp m_exec_x;
    a64::Gp m_exec_x_end;
    a64::Gp m_exec_y_end;

    const SwsOp *m_read_op;
    const SwsOp *m_dither_op;
    const SwsOp *m_write_op;

    /* const data */
    std::vector<uint32_t> m_data;
    std::vector<a64::Vec> m_vdata;

    a64::Vec vdata(size_t vidx)
    {
        int vdata_i = vidx >> 2;
        int vdata_j = vidx & 3;
        return m_vdata[vdata_i].s(vdata_j);
    }

    /* immediates */
    std::vector<std::pair<uint32_t, uint32_t>> m_imm;
    std::vector<a64::Vec> m_vimm;

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
        cc.comment("make asmjit happy");
        to_prologue();
        cc.comment("breakpoint");
        cc.brk(0x0f00);
        from_prologue();
#endif
        m_exec = cc.newGpz();
        m_func->setArg(0, m_exec);

        m_x          = cc.newGpz();
        m_y          = cc.newGpz();
        m_exec_x     = cc.newGpz();
        m_exec_x_end = cc.newGpz();
        m_exec_y_end = cc.newGpz();

        m_read_op   = nullptr;
        m_dither_op = nullptr;
        m_write_op  = nullptr;
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

    /* immediates */
    size_t push_imm32(uint32_t val, int len = 4)
    {
        /* First check if we already have it */
        for (size_t i = 0; i < m_imm.size(); i++) {
            if (m_imm[i].first == val) {
                return i;
            }
        }
        /* Check if data can be represented by repeating smaller value */
        int repeat_len;
        int small_value;
        union {
            uint32_t u32;
            uint16_t u16[2];
            uint8_t  u8[4];
        } u;
        u.u32 = val;
        if (u.u16[0] != u.u16[1]) {
            repeat_len = 4;
            small_value = (u.u32 < 0x100);
        } else if (u.u8[0] != u.u8[1]) {
            repeat_len = 2;
            small_value = (u.u16[0] < 0x100);
        } else {
            repeat_len = 1;
            small_value = 1;
        }
        /* Add it to our data and create a new vector */
        size_t ret = m_imm.size();
        m_imm.push_back(std::make_pair(val, (small_value << 16) | (repeat_len << 8) | len));
        m_vimm.push_back(m_cc->newVecQ());
        return ret;
    }

    size_t push_imm16(uint16_t _val, int len = 2)
    {
        uint32_t val = _val;
        val |= val << 16;
        return push_imm32(val, len);
    }

    size_t push_imm8(uint8_t _val, int len = 1)
    {
        uint16_t val = _val;
        val |= val << 8;
        return push_imm16(val, len);
    }

    size_t push_imm32_op(const SwsOp &op, uint32_t val)
    {
        if (op.type == SWS_PIXEL_U8)
            return push_imm8(val);
        if (op.type == SWS_PIXEL_U16)
            return push_imm16(val);
        return push_imm32(val);
    }

    size_t push_immq(AVRational q)
    {
        union {
            uint32_t u32;
            float    f32;
        } u;
        u.f32 = av_q2f(q);
        return push_imm32(u.u32);
    }

    void load_immediates(void)
    {
        size_t size = m_imm.size();
        if (size == 0)
            return;

        a64::Compiler &cc = *m_cc;
        to_prologue();
        cc.comment("prologue (immediates)");
        std::vector<a64::Gp> tmp(size);
        /* First load immediates larger than 0xff into temporary registers */
        for (size_t i = 0; i < size; i++) {
            int small_value = m_imm[i].second >> 16;
            uint8_t repeat_len = m_imm[i].second >> 8;
            if (!small_value && repeat_len != 1)
            {
                tmp[i] = cc.newGpz();
                switch (repeat_len) {
                case 2: cc.mov(tmp[i], m_imm[i].first & 0xffff); break;
                case 4: cc.mov(tmp[i], m_imm[i].first         ); break;
                }
           }
        }
        /* Then load small immediates directly into vectors */
        for (size_t i = 0; i < size; i++) {
            int small_value = m_imm[i].second >> 16;
            if (small_value)
            {
                uint8_t repeat_len = m_imm[i].second >> 8;
                switch (repeat_len) {
                case 1: cc.movi(m_vimm[i].b16(), m_imm[i].first & 0xff); break;
                case 2: cc.movi(m_vimm[i].h8 (), m_imm[i].first & 0xff); break;
                case 4: cc.movi(m_vimm[i].s4 (), m_imm[i].first & 0xff); break;
                }
            }
        }
        /* Then dup the temporary registers into vectors */
        for (size_t i = 0; i < size; i++) {
            int small_value = m_imm[i].second >> 16;
            uint8_t repeat_len = m_imm[i].second >> 8;
            if (!small_value && repeat_len != 1)
            {
                uint8_t len = m_imm[i].second;
                switch (len) {
                case 2: cc.dup(m_vimm[i].h8(), tmp[i]); break;
                case 4: cc.dup(m_vimm[i].s4(), tmp[i]); break;
                }
            }
        }
        from_prologue();
    }

    /* const data */
    size_t push_u32(uint32_t val)
    {
        /* First check if we already have it */
        for (size_t i = 0; i < m_data.size(); i++) {
            if (m_data[i] == val) {
                return i;
            }
        }
        /* Add it to our data and create a new vector if necessary */
        size_t ret = m_data.size();
        m_data.push_back(val);
        if ((ret & 3) == 0) {
            m_vdata.push_back(m_cc->newVecQ());
        }
        return ret;
    }

    size_t push_q(AVRational q)
    {
        union {
            uint32_t u32;
            float    f32;
        } u;
        u.f32 = av_q2f(q);
        return push_u32(u.u32);
    }

    Label emit_data(void *data, size_t size)
    {
        a64::Compiler &cc = *m_cc;

        Label ldata = cc.newLabel();
        BaseNode *cursor = cc.cursor();
        cc.setCursor(m_func->endNode()->prev());
        cc.align(AlignMode::kData, 16);
        cc.bind(ldata);
        cc.embed(data, size);
        cc.setCursor(cursor);

        return ldata;
    }

    void emit_const()
    {
        if (m_vdata.size() == 0)
            return;

        /* Write const data after function */
        Label ldata = emit_data(m_data.data(), m_data.size() * sizeof(uint32_t));

        /* Read matrix data into vectors */
        a64::Compiler &cc = *m_cc;
        a64::Gp rdata = cc.newGpz();

        to_prologue();
        cc.comment("prologue (const data)");
        cc.adr(rdata, ldata);
        switch (m_vdata.size()) {
        case 1: cc.ld1(m_vdata[0].b16(),                                                       a64::ptr(rdata)); break;
        case 2: cc.ld1(m_vdata[0].b16(), m_vdata[1].b16(),                                     a64::ptr(rdata)); break;
        case 3: cc.ld1(m_vdata[0].b16(), m_vdata[1].b16(), m_vdata[2].b16(),                   a64::ptr(rdata)); break;
        case 4: cc.ld1(m_vdata[0].b16(), m_vdata[1].b16(), m_vdata[2].b16(), m_vdata[3].b16(), a64::ptr(rdata)); break;
        case 5:
            /* help out asmjit with smaller ld1 */
            cc.ld1(m_vdata[0].b16(), m_vdata[1].b16(),                   a64::ptr(rdata).post(32));
            cc.ld1(m_vdata[2].b16(), m_vdata[3].b16(), m_vdata[4].b16(), a64::ptr(rdata));
            break;
        default:
            __builtin_trap();
            break;
        }
        from_prologue();
    }

    void emit_loop(const SwsOpChain *const chain)
    {
#ifdef EMIT_LOOP
        a64::Compiler &cc = *m_cc;
        a64::Gp width = cc.newGpz();
        a64::Gp in_padding[4];
        a64::Gp out_padding[4];
        Label hloop = cc.newLabel();
        Label vloop = cc.newLabel();
        bool xy_unused = (m_dither_op == nullptr);

        to_prologue();
        cc.comment("prologue (vertical)");
        cc.ldr(m_y         .r32(), a64::ptr(m_exec, offsetof(SwsOpExec, y)));
        cc.ldr(m_exec_y_end.r32(), a64::ptr(m_exec, offsetof(SwsOpExec, y_end)));
        if (xy_unused) {
            cc.sub(m_y.r32(), m_exec_y_end.r32(), m_y.r32());
        }
        cc.comment("prologue (horizontal)");
        cc.ldr(m_exec_x    .r32(), a64::ptr(m_exec, offsetof(SwsOpExec, x)));
        cc.ldr(m_exec_x_end.r32(), a64::ptr(m_exec, offsetof(SwsOpExec, x_end)));
        cc.sub(width.r32(), m_exec_x_end.r32(), m_exec_x.r32());
        cc.comment("prologue (padding)");
        for (int i = 0; i < (m_read_op->rw.packed ? 1 : m_read_op->rw.elems); i++) {
            in_padding[i] = cc.newGpz();
            cc.ldr(in_padding[i], a64::ptr(m_exec, offsetof(SwsOpExec, in_padding) + (i * sizeof(ptrdiff_t))));
        }
        for (int i = 0; i < (m_write_op->rw.packed ? 1 : m_write_op->rw.elems); i++) {
            out_padding[i] = cc.newGpz();
            cc.ldr(out_padding[i], a64::ptr(m_exec, offsetof(SwsOpExec, out_padding) + (i * sizeof(ptrdiff_t))));
        }
        cc.bind(vloop);
        cc.comment("horizontal loop");
        if (xy_unused) {
            cc.mov(m_x.r32(), width.r32());
        } else {
            cc.mov(m_x.r32(), m_exec_x.r32());
        }
        cc.bind(hloop);
        from_prologue();

        cc.comment("horizontal loop back");
        if (xy_unused) {
            cc.subs(m_x.r32(), m_x.r32(), chain->block_w);
            cc.b(a64::CondCode::kGT, hloop);
        } else {
            cc.add(m_x.r32(), m_x.r32(), chain->block_w);
            cc.cmp(m_x.r32(), m_exec_x_end.r32());
            cc.b(a64::CondCode::kLO, hloop);
        }

        cc.comment("padding (read)");
        for (int i = 0; i < (m_read_op->rw.packed ? 1 : m_read_op->rw.elems); i++) {
            cc.add(m_in[i], m_in[i], in_padding[i]);
        }
        cc.comment("padding (write)");
        for (int i = 0; i < (m_write_op->rw.packed ? 1 : m_write_op->rw.elems); i++) {
            cc.add(m_out[i], m_out[i], out_padding[i]);
        }

        cc.comment("vertical loop back");
        if (xy_unused) {
            cc.subs(m_y.r32(), m_y.r32(), chain->block_h);
            cc.b(a64::CondCode::kGT, vloop);
        } else {
            cc.add(m_y.r32(), m_y.r32(), chain->block_h);
            cc.cmp(m_y.r32(), m_exec_y_end.r32());
            cc.b(a64::CondCode::kLO, vloop);
        }

        cc.comment("epilogue");
        /* Write exec.[xy]_end to exec.[xy] to signal we have converted the entire image */
        if (xy_unused) {
            cc.ldr(m_exec_y_end.r32(), a64::ptr(m_exec, offsetof(SwsOpExec, y_end)));
            cc.ldr(m_exec_x_end.r32(), a64::ptr(m_exec, offsetof(SwsOpExec, x_end)));
            cc.str(m_exec_y_end.r32(), a64::ptr(m_exec, offsetof(SwsOpExec, y)));
            cc.str(m_exec_x_end.r32(), a64::ptr(m_exec, offsetof(SwsOpExec, x)));
        } else {
            cc.str(m_y.r32(), a64::ptr(m_exec, offsetof(SwsOpExec, y)));
            cc.str(m_x.r32(), a64::ptr(m_exec, offsetof(SwsOpExec, x)));
        }
#endif
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

#define LOOP_ARRAY(idx, arr)          \
    for (int idx = 0; idx < 4; idx++) \
        if (arr[idx])
#define LOOP_OUT(idx) LOOP_ARRAY(idx, !next->comps.unused)
#define LOOP_IN(idx)  LOOP_ARRAY(idx, !op.comps.unused)

static inline uint32_t mask_from_i(int i)
{
    return (1 << i) | (1 << (i + 4));
}

static inline void save_vectors_mask(AsmJitContext *ctx, uint32_t mask)
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

static inline void new_vectors_mask(AsmJitContext *ctx, uint32_t mask)
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

static inline void new_vector(AsmJitContext *ctx, int i, int mask = 0xff)
{
    mask &= mask_from_i(i);
    new_vectors_mask(ctx, mask);
}

static inline void save_vector(AsmJitContext *ctx, int i, int mask = 0xff)
{
    mask &= mask_from_i(i);
    save_vectors_mask(ctx, mask);
}

static inline void refresh_vector(AsmJitContext *ctx, int i, int mask = 0xff)
{
    mask &= mask_from_i(i);
    save_vectors_mask(ctx, mask);
    new_vectors_mask(ctx, mask);
}

static int emit_convert(AsmJitContext *ctx, const SwsOpChain *chain, const SwsOp *next, SwsPixelType from, SwsPixelType to, bool expand)
{
    a64::Compiler &cc = *ctx->m_cc;
    a64::Vec *orig_vl = ctx->m_orig_vl;
    a64::Vec *orig_vh = ctx->m_orig_vh;
    a64::Vec *vl = ctx->m_vl;
    a64::Vec *vh = ctx->m_vh;
    int from_size = ff_sws_pixel_type_size(from);
    int to_size   = ff_sws_pixel_type_size(to);
    char buf[128];

    if (expand) {
        snprintf(buf, sizeof(buf), "expand(%s -> %s, block_w %d)", ff_sws_pixel_type_name(from), ff_sws_pixel_type_name(to), chain->block_w);
        cc.comment(buf);
        if        (from_size == 1 && to_size == 2 && chain->block_w == 8) {
            LOOP_OUT(i) {
                refresh_vector(ctx, i, 0x0f);
                cc.zip1(vl[i].b16(), orig_vl[i].b16(), orig_vl[i].b16());
            }
        } else if (from_size == 1 && to_size == 2 && chain->block_w == 16) {
            LOOP_OUT(i) {
                save_vector(ctx, i, 0x0f);
                new_vector(ctx, i);
                cc.zip1(vl[i].b16(), orig_vl[i].b16(), orig_vl[i].b16());
                cc.zip2(vh[i].b16(), orig_vl[i].b16(), orig_vl[i].b16());
            }
        } else if (from_size == 1 && to_size == 4 && chain->block_w == 8) {
            LOOP_OUT(i) {
                refresh_vector(ctx, i, 0x0f);
                cc.zip1(vl[i].b16(), orig_vl[i].b16(), orig_vl[i].b16());
            }
            LOOP_OUT(i) {
                save_vector(ctx, i, 0x0f);
                new_vector(ctx, i);
                cc.zip1(vl[i].b16(), orig_vl[i].b16(), orig_vl[i].b16());
                cc.zip2(vh[i].b16(), orig_vl[i].b16(), orig_vl[i].b16());
            }
        } else {
            return AVERROR(ENOTSUP);
        }
    } else {
        snprintf(buf, sizeof(buf), "convert(%s -> %s, block_w %d)", ff_sws_pixel_type_name(from), ff_sws_pixel_type_name(to), chain->block_w);
        cc.comment(buf);
        if (from == SWS_PIXEL_F32) {
            LOOP_OUT(i) {
                refresh_vector(ctx, i);
                cc.fcvtzu(vl[i].s4(), orig_vl[i].s4());
                cc.fcvtzu(vh[i].s4(), orig_vh[i].s4());
            }
        }
        if (chain->block_w == 8) {
            if        (from_size == 1 && to_size > from_size) {
                LOOP_OUT(i) {
                    refresh_vector(ctx, i, 0x0f);
                    cc.uxtl(vl[i].h8(), orig_vl[i].b8());
                }
                from_size = 2;
            } else if (from_size == 4 && to_size < from_size) {
                LOOP_OUT(i) {
                    refresh_vector(ctx, i);
                    cc.xtn(vl[i].h4(), orig_vl[i].s4());
                    cc.xtn(vh[i].h4(), orig_vh[i].s4());
                }
                LOOP_OUT(i) {
                    cc.ins(vl[i].d(1), vh[i].d(0));
                }
                from_size = 2;
            }
            if        (from_size == 2 && to_size == 4) {
                LOOP_OUT(i) {
                    save_vector(ctx, i, 0x0f);
                    new_vector(ctx, i);
                    cc.uxtl (vl[i].s4(), orig_vl[i].h4());
                    cc.uxtl2(vh[i].s4(), orig_vl[i].h8());
                }
                from_size = 4;
            } else if (from_size == 2 && to_size == 1) {
                LOOP_OUT(i) {
                    refresh_vector(ctx, i, 0x0f);
                    cc.xtn(vl[i].b8(), orig_vl[i].h8());
                }
                from_size = 1;
            }
        } else /* if (chain->block_w == 16) */ {
            if        (from_size == 1 && to_size == 2) {
                LOOP_OUT(i) {
                    save_vector(ctx, i, 0x0f);
                    new_vector(ctx, i);
                    cc.uxtl (vl[i].h8(), orig_vl[i].b8());
                    cc.uxtl2(vh[i].h8(), orig_vl[i].b16());
                }
            } else if (from_size == 2 && to_size == 1) {
                LOOP_OUT(i) {
                    refresh_vector(ctx, i);
                    cc.xtn(vl[i].b8(), orig_vl[i].h8());
                    cc.xtn(vh[i].b8(), orig_vh[i].h8());
                }
                LOOP_OUT(i) {
                    cc.ins(vl[i].d(1), vh[i].d(0));
                }
            }
        }
        if (to == SWS_PIXEL_F32) {
            LOOP_OUT(i) {
                refresh_vector(ctx, i);
                cc.ucvtf(vl[i].s4(), orig_vl[i].s4());
                cc.ucvtf(vh[i].s4(), orig_vh[i].s4());
            }
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
    std::vector<a64::Vec> &vimm = ctx->m_vimm;

    SwsOp &op = ops->ops[0];
    SwsOp *next = &ops->ops[1];

    VectorElementType vet(chain);

    bool use_vh = ((op.type == SWS_PIXEL_U16) && chain->block_w == 16)
               || ((op.type == SWS_PIXEL_U32) && chain->block_w == 8)
               || ((op.type == SWS_PIXEL_F32) && chain->block_w == 8);

    switch (op.op) {
    /* Input/output handling */
    case SWS_OP_READ:            /* gather raw pixels from planes */
        if (op.rw.frac)
            return AVERROR(ENOTSUP);
        ctx->m_read_op = &op;
        cc.comment("read");
        if (!op.rw.packed) {
            /* Load input pointers in prologue */
            ctx->to_prologue();
            cc.comment("prologue (read)");
            LOOP_OUT(i) {
                ctx->m_in[i] = cc.newGpz();
                cc.ldr(ctx->m_in[i], a64::ptr(exec, offsetof(SwsOpExec, in) + sizeof(uint8_t *) * i));
            }
            ctx->from_prologue();
            /* Read vectors from input pointers */
            LOOP_OUT(i) {
                new_vector(ctx, i, use_vh ? 0xff : 0x0f);
                if (use_vh)
                    cc.ld1(vet(vl[i], op), vet(vh[i], op), a64::ptr(ctx->m_in[i]).post(vet.size(op) * 2));
                else
                    cc.ld1(vet(vl[i], op),                 a64::ptr(ctx->m_in[i]).post(vet.size(op) * 1));
            }
        } else {
            /* Load input pointer in prologue */
            ctx->to_prologue();
            cc.comment("prologue (read)");
            ctx->m_in[0] = cc.newGpz();
            cc.ldr(ctx->m_in[0], a64::ptr(exec, offsetof(SwsOpExec, in)));
            ctx->from_prologue();
            /* Read vectors from input pointer */
            for (int i = 0; i < op.rw.elems; i++) {
                new_vector(ctx, i, use_vh ? 0xff : 0x0f);
            }
            switch (op.rw.elems) {
            case 1:
                if (use_vh)
                    cc.ld1(vet(vl[0], op), vet(vh[0], op),                                 a64::ptr(ctx->m_in[0]).post(vet.size(op) * 2));
                else
                    cc.ld1(vet(vl[0], op),                                                 a64::ptr(ctx->m_in[0]).post(vet.size(op) * 1));
                break;
            case 2:
                cc.ld2    (vet(vl[0], op), vet(vl[1], op),                                 a64::ptr(ctx->m_in[0]).post(vet.size(op) * 2));
                if (use_vh)
                    cc.ld2(vet(vh[0], op), vet(vh[1], op),                                 a64::ptr(ctx->m_in[0]).post(vet.size(op) * 2));
                break;
            case 3:
                cc.ld3    (vet(vl[0], op), vet(vl[1], op), vet(vl[2], op),                 a64::ptr(ctx->m_in[0]).post(vet.size(op) * 3));
                if (use_vh)
                    cc.ld3(vet(vh[0], op), vet(vh[1], op), vet(vh[2], op),                 a64::ptr(ctx->m_in[0]).post(vet.size(op) * 3));
                break;
            case 4:
                cc.ld4    (vet(vl[0], op), vet(vl[1], op), vet(vl[2], op), vet(vl[3], op), a64::ptr(ctx->m_in[0]).post(vet.size(op) * 4));
                if (use_vh)
                    cc.ld4(vet(vh[0], op), vet(vh[1], op), vet(vh[2], op), vet(vh[3], op), a64::ptr(ctx->m_in[0]).post(vet.size(op) * 4));
                break;
            }
        }
        break;
    case SWS_OP_WRITE:           /* write raw pixels to planes */
        if (op.rw.frac)
            return AVERROR(ENOTSUP);
        ctx->m_write_op = &op;
        cc.comment("write");
        if (!op.rw.packed) {
            /* Load output pointers in prologue */
            ctx->to_prologue();
            cc.comment("prologue (write)");
            LOOP_IN(i) {
                ctx->m_out[i] = cc.newGpz();
                cc.ldr(ctx->m_out[i], a64::ptr(exec, offsetof(SwsOpExec, out) + sizeof(uint8_t *) * i));
            }
            ctx->from_prologue();
            /* Write vectors to output pointers */
            LOOP_IN(i) {
                if (use_vh)
                    cc.st1(vet(vl[i], op), vet(vh[i], op), a64::ptr(ctx->m_out[i]).post(vet.size(op) * 2));
                else
                    cc.st1(vet(vl[i], op),                 a64::ptr(ctx->m_out[i]).post(vet.size(op) * 1));
            }
        } else {
            /* Load output pointer in prologue */
            ctx->to_prologue();
            cc.comment("prologue (write)");
            ctx->m_out[0] = cc.newGpz();
            cc.ldr(ctx->m_out[0], a64::ptr(exec, offsetof(SwsOpExec, out)));
            ctx->from_prologue();
            /* Write vectors to output pointer */
            switch (op.rw.elems) {
            case 1:
                if (use_vh)
                    cc.st1(vet(vl[0], op), vet(vh[0], op),                                 a64::ptr(ctx->m_out[0]).post(vet.size(op) * 2));
                else
                    cc.st1(vet(vl[0], op),                                                 a64::ptr(ctx->m_out[0]).post(vet.size(op) * 1));
                break;
            case 2:
                cc.st2    (vet(vl[0], op), vet(vl[1], op),                                 a64::ptr(ctx->m_out[0]).post(vet.size(op) * 2));
                if (use_vh)
                    cc.st2(vet(vh[0], op), vet(vh[1], op),                                 a64::ptr(ctx->m_out[0]).post(vet.size(op) * 2));
                break;
            case 3:
                cc.st3    (vet(vl[0], op), vet(vl[1], op), vet(vl[2], op),                 a64::ptr(ctx->m_out[0]).post(vet.size(op) * 3));
                if (use_vh)
                    cc.st3(vet(vh[0], op), vet(vh[1], op), vet(vh[2], op),                 a64::ptr(ctx->m_out[0]).post(vet.size(op) * 3));
                break;
            case 4:
{
#if 0
                cc.st4    (vet(vl[0], op), vet(vl[1], op), vet(vl[2], op), vet(vl[3], op), a64::ptr(ctx->m_out[0]).post(vet.size(op) * 4));
                if (use_vh)
                    cc.st4(vet(vh[0], op), vet(vh[1], op), vet(vh[2], op), vet(vh[3], op), a64::ptr(ctx->m_out[0]).post(vet.size(op) * 4));
#else
// TODO help asmjit's register allocator
if (use_vh) {
    cc.st4(vet(vl[0], op), vet(vl[1], op), vet(vl[2], op), vet(vl[3], op), a64::ptr(ctx->m_out[0]).post(vet.size(op) * 4));
    cc.mov(vl[0].b16(), vh[0].b16());
    cc.mov(vl[1].b16(), vh[1].b16());
    cc.mov(vl[2].b16(), vh[2].b16());
    cc.mov(vl[3].b16(), vh[3].b16());
    cc.st4(vet(vl[0], op), vet(vl[1], op), vet(vl[2], op), vet(vl[3], op), a64::ptr(ctx->m_out[0]).post(vet.size(op) * 4));
} else {
    cc.st4(vet(vl[0], op), vet(vl[1], op), vet(vl[2], op), vet(vl[3], op), a64::ptr(ctx->m_out[0]).post(vet.size(op) * 4));
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
            LOOP_OUT(i) {
                refresh_vector(ctx, i, use_vh ? 0xff : 0x0f);
                cc.rev16    (vl[i].b16(), orig_vl[i].b16());
                if (use_vh)
                    cc.rev16(vh[i].b16(), orig_vh[i].b16());
            }
        } else if (op.type == SWS_PIXEL_U32 || op.type == SWS_PIXEL_F32) {
            cc.comment("swap_bytes (u32)");
            LOOP_OUT(i) {
                refresh_vector(ctx, i, use_vh ? 0xff : 0x0f);
                cc.rev32    (vl[i].b16(), orig_vl[i].b16());
                if (use_vh)
                    cc.rev32(vh[i].b16(), orig_vh[i].b16());
            }
        }
        break;
    case SWS_OP_UNPACK:          /* split tightly packed data into components */
        {
            int offsets[4] = {
                op.pack.pattern[3] + op.pack.pattern[2] + op.pack.pattern[1],
                op.pack.pattern[3] + op.pack.pattern[2],
                op.pack.pattern[3],
                0
            };

            cc.comment("unpack");
            save_vector(ctx, 0);
            LOOP_ARRAY(i, op.pack.pattern) {
                if (!offsets[i]) {
                    /* Move element with no offset */
                    vl[i] = orig_vl[0];
                    if (use_vh)
                        vh[i] = orig_vh[0];
                } else {
                    new_vector(ctx, i, use_vh ? 0xff : 0x0f);
                    cc.ushr    (vet(vl[i], op), vet(orig_vl[0], op), offsets[i]);
                    if (use_vh)
                        cc.ushr(vet(vh[i], op), vet(orig_vh[0], op), offsets[i]);
                }
            }
            LOOP_ARRAY(i, op.pack.pattern) {
                uint32_t mask = (1u << op.pack.pattern[i]) - 1;
                size_t vidx = ctx->push_imm32_op(op, mask);
                refresh_vector(ctx, i, use_vh ? 0xff : 0x0f);
                cc.and_    (vl[i].b16(), orig_vl[i].b16(), vimm[vidx].b16());
                if (use_vh)
                    cc.and_(vh[i].b16(), orig_vh[i].b16(), vimm[vidx].b16());
            }
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

            cc.comment("pack");
            LOOP_ARRAY(i, offsets) {
                refresh_vector(ctx, i, use_vh ? 0xff : 0x0f);
                cc.shl    (vet(vl[i], op), vet(orig_vl[i], op), offsets[i]);
                if (use_vh)
                    cc.shl(vet(vh[i], op), vet(orig_vh[i], op), offsets[i]);
            }
            LOOP_IN(i) {
                if (i != 0) {
                    refresh_vector(ctx, 0, use_vh ? 0xff : 0x0f);
                    cc.orr    (vl[0].b16(), orig_vl[0].b16(), vl[i].b16());
                    if (use_vh)
                        cc.orr(vh[0].b16(), orig_vh[0].b16(), vh[i].b16());
                }
            }
        }
        break;
    /* Pixel manipulation */
    case SWS_OP_CLEAR:           /* clear pixel values */
        /* Set vectors to constant value */
        if (op.type == SWS_PIXEL_U8 || op.type == SWS_PIXEL_U16 || op.type == SWS_PIXEL_U32) {
            cc.comment("clear (integer)");
            for (int i = 0; i < 4; i++) {
                if (op.clear.value[i].den) {
                    size_t vidx = ctx->push_imm32_op(op, av_q2i(op.clear.value[i]));
                    if (next->op == SWS_OP_WRITE) {
                        /* TODO astmjit's register allocator sometimes fails, so we relieve some pressure */
                        new_vector(ctx, i, 0x0f);
                        cc.mov(vet(vl[i], op), vet(vimm[vidx], op));
                    } else {
                        vl[i] = vimm[vidx];
                    }
                    if (use_vh)
                        vh[i] = vimm[vidx];
                }
            }
        } else if (op.type == SWS_PIXEL_F32) {
            /* Add const data */
            size_t vpos[4];
            for (int i = 0; i < 4; i++)
                if (op.clear.value[i].den)
                    vpos[i] = ctx->push_q(op.clear.value[i]);

            /* Do the salmon dance */
            cc.comment("clear (f32)");
            for (int i = 0; i < 4; i++) {
                if (op.clear.value[i].den) {
                    new_vector(ctx, i);
                    cc.dup(vl[i].s4(), ctx->vdata(vpos[i]));
                    cc.dup(vh[i].s4(), ctx->vdata(vpos[i]));
                }
            }
        }
        break;
    case SWS_OP_LSHIFT:          /* logical left shift of raw pixel values */
        if (op.type == SWS_PIXEL_U8 || op.type == SWS_PIXEL_U16 || op.type == SWS_PIXEL_U32) {
            cc.comment("lshift");
            LOOP_OUT(i) {
                refresh_vector(ctx, i, use_vh ? 0xff : 0x0f);
                cc.shl    (vet(vl[i], op), vet(orig_vl[i], op), op.shift.amount);
                if (use_vh)
                    cc.shl(vet(vh[i], op), vet(orig_vh[i], op), op.shift.amount);
            }
        } else {
            return AVERROR(ENOTSUP);
        }
        break;
    case SWS_OP_RSHIFT:          /* right shift of raw pixel values */
        if (op.type == SWS_PIXEL_U8 || op.type == SWS_PIXEL_U16 || op.type == SWS_PIXEL_U32) {
            cc.comment("rshift");
            LOOP_OUT(i) {
                refresh_vector(ctx, i, use_vh ? 0xff : 0x0f);
                cc.ushr    (vet(vl[i], op), vet(orig_vl[i], op), op.shift.amount);
                if (use_vh)
                    cc.ushr(vet(vh[i], op), vet(orig_vh[i], op), op.shift.amount);
            }
        } else {
            return AVERROR(ENOTSUP);
        }
        break;
    case SWS_OP_SWIZZLE:         /* rearrange channel order, or duplicate channels */
        {
            bool reorder = true;
            bool used[4] = { false, false, false, false };
            LOOP_OUT(i) {
                if (used[op.swizzle.in[i]]) {
                    reorder = false;
                    break;
                }
                used[op.swizzle.in[i]] = true;
            }

            LOOP_IN(i) {
                save_vector(ctx, i);
            }
            if (reorder) {
                cc.comment("swizzle (reorder)");
                LOOP_OUT(i) {
                    vl[i] = orig_vl[op.swizzle.in[i]];
                    vh[i] = orig_vh[op.swizzle.in[i]];
                }
            } else {
                cc.comment("swizzle (copy)");
                LOOP_OUT(i) {
                    if (i == op.swizzle.in[i]) {
                        vl[i] = orig_vl[op.swizzle.in[i]];
                        if (use_vh)
                            vh[i] = orig_vh[op.swizzle.in[i]];
                    } else {
                        new_vector(ctx, i, use_vh ? 0xff : 0x0f);
                        cc.mov    (vl[i].b16(), orig_vl[op.swizzle.in[i]].b16());
                        if (use_vh)
                            cc.mov(vh[i].b16(), orig_vh[op.swizzle.in[i]].b16());
                    }
                }
            }
        }
        break;
    case SWS_OP_CONVERT:         /* convert (cast) between formats */
        if (op.type != op.convert.to && emit_convert(ctx, chain, next, op.type, op.convert.to, op.convert.expand) < 0)
            return AVERROR(ENOTSUP);
        break;
    case SWS_OP_DITHER:          /* add dithering noise */
        if (op.dither.size_log2 == 0) {
            /* TODO dither(none) + convert(f32->u) use rounding convert instead */
            cc.comment("dither (none)");

            size_t vidx = ctx->push_immq(op.dither.matrix[0]);
            LOOP_OUT(i) {
                refresh_vector(ctx, i, use_vh ? 0xff : 0x0f);
                cc.fadd(vl[i].s4(), orig_vl[i].s4(), vet(vimm[vidx], op));
                cc.fadd(vh[i].s4(), orig_vh[i].s4(), vet(vimm[vidx], op));
            }
        } else {
            cc.comment("dither");

            /* Used by emit_loop to optimize away the use of x and y */
            ctx->m_dither_op = &op;

            /* Write const data after function */
            int size = 1 << op.dither.size_log2;
            std::vector<float> fdata;
            fdata.resize(size * size);
            for (int i = 0; i < size * size; i++) {
                fdata[i] = av_q2f(op.dither.matrix[i]);
            }
            Label ldata = ctx->emit_data(fdata.data(), size * size * sizeof(float));

            a64::Gp rdatal = cc.newGpz();
            a64::Gp rdatah = cc.newGpz();
            a64::Gp x = cc.newGpz();
            a64::Gp y = cc.newGpz();

            cc.adr(rdatal, ldata);
#ifndef EMIT_LOOP
            cc.ldr(ctx->m_x.r32(), a64::ptr(ctx->m_exec, offsetof(SwsOpExec, x)));
            cc.ldr(ctx->m_y.r32(), a64::ptr(ctx->m_exec, offsetof(SwsOpExec, y)));
#endif
            /* x = (x & ((1 << size_log2) - 1)) * sizeof(float32) */
            cc.ubfiz(x, ctx->m_x, 2, op.dither.size_log2);
            cc.add(rdatah, rdatal, 16);
            cc.add(rdatal, rdatal, x);
            cc.add(rdatah, rdatah, x);

            static const int y_off[4] = { 0, 3, 5, 7 };
            LOOP_OUT(i) {
                // offset = ((((y + yoff[i]) & mask) << log2_size) + (x & mask)) * sizeof(float32);

                a64::Gp ptrl = cc.newGpz();
                a64::Gp ptrh = cc.newGpz();
                a64::Vec dither_vl = cc.newVecQ();
                a64::Vec dither_vh = cc.newVecQ();

                if (y_off[i] == 0) {
                    cc.ubfiz(y, ctx->m_y, op.dither.size_log2 + 2, op.dither.size_log2);
                } else {
                    cc.add  (y, ctx->m_y, y_off[i]);
                    cc.ubfiz(y, y, op.dither.size_log2 + 2, op.dither.size_log2);
                }
                cc.add(ptrl, rdatal, y);
                cc.add(ptrh, rdatah, y);

                cc.ld1(dither_vl.s4(), a64::ptr(ptrl));
                cc.ld1(dither_vh.s4(), a64::ptr(ptrh));

                refresh_vector(ctx, i, use_vh ? 0xff : 0x0f);
                cc.fadd(vl[i].s4(), orig_vl[i].s4(), dither_vl.s4());
                cc.fadd(vh[i].s4(), orig_vh[i].s4(), dither_vh.s4());
            }
        }
        break;
    case SWS_OP_CLAMP:           /* clamp pixel values to value range */
        if (next->op == SWS_OP_CONVERT && next->type == SWS_PIXEL_F32 && next->convert.to == SWS_PIXEL_U8) {
            LOOP_OUT(i) {
                if (av_cmp_q(op.clamp.max[i], (AVRational) {255, 1}) != 0)
                    goto normal_clamp;
            }

            cc.comment("convert+clamp");
            if (emit_convert(ctx, chain, next, op.type, SWS_PIXEL_U16, false) < 0)
                return AVERROR(ENOTSUP);
            /* Saturating convert from u16 to u8 */
            LOOP_OUT(i) {
                refresh_vector(ctx, i, 0x0f);
                cc.uqxtn(vl[i].b8(), orig_vl[i].h8());
            }
            ops->ops++;
            ops->num_ops--;
        } else if (next->op == SWS_OP_CONVERT && next->type == SWS_PIXEL_F32 && next->convert.to == SWS_PIXEL_U16) {
            LOOP_OUT(i) {
                if (av_cmp_q(op.clamp.max[i], (AVRational) {65535, 1}) != 0)
                    goto normal_clamp;
            }

            cc.comment("convert+clamp");
            if (emit_convert(ctx, chain, next, op.type, SWS_PIXEL_U32, false) < 0)
                return AVERROR(ENOTSUP);
            /* Saturating convert from u32 to u16 */
            LOOP_OUT(i) {
                refresh_vector(ctx, i);
                cc.uqxtn(vl[i].h4(), orig_vl[i].s4());
                cc.uqxtn(vh[i].h4(), orig_vh[i].s4());
            }
            LOOP_OUT(i) {
                cc.ins(vl[i].d(1), vh[i].d(0));
            }
            ops->ops++;
            ops->num_ops--;
        } else {
normal_clamp:
            if (op.type == SWS_PIXEL_F32) {
                /* Check whether we need to clamp negative values */
                bool clamp_negative_values = true;
                for (int i = 0; i < ops->num_ops; i++) {
                    if (ops->ops[i].op == SWS_OP_CONVERT && ops->ops[i].convert.to != SWS_PIXEL_F32) {
                        clamp_negative_values = false;
                        break;
                    }
                }

                cc.comment("clamp");
                size_t vidx_min = ctx->push_imm32(0);
                LOOP_OUT(i) {
                    if (op.clamp.max[i].den) {
                        if (clamp_negative_values) {
                            refresh_vector(ctx, i, use_vh ? 0xff : 0x0f);
                            cc.fmax    (vl[i].s4(), orig_vl[i].s4(), vimm[vidx_min].s4());
                            if (use_vh)
                                cc.fmax(vh[i].s4(), orig_vh[i].s4(), vimm[vidx_min].s4());
                        }
                        size_t vidx_max = ctx->push_immq(op.clamp.max[i]);
                        refresh_vector(ctx, i, use_vh ? 0xff : 0x0f);
                        cc.fmin    (vl[i].s4(), orig_vl[i].s4(), vimm[vidx_max].s4());
                        if (use_vh)
                            cc.fmin(vh[i].s4(), orig_vh[i].s4(), vimm[vidx_max].s4());
                    }
                }
            } else if (op.type == SWS_PIXEL_U8 || op.type == SWS_PIXEL_U16 || op.type == SWS_PIXEL_U32) {
                cc.comment("clamp");
                LOOP_OUT(i) {
                    if (op.clamp.max[i].den) {
                        size_t vidx_max = ctx->push_imm32_op(op, av_q2i(op.clamp.max[i]));
                        refresh_vector(ctx, i, use_vh ? 0xff : 0x0f);
                        cc.umin    (vet(vl[i], op), vet(orig_vl[i], op), vet(vimm[vidx_max], op));
                        if (use_vh)
                            cc.umin(vet(vh[i], op), vet(orig_vh[i], op), vet(vimm[vidx_max], op));
                    }
                }
            }
        }
        break;
    /* Arithmetic operations */
    case SWS_OP_LINEAR:          /* generalized linear affine transform */
        {
            /* Start with offset and then the coefficients */
            const int fdata_swizzle[5] = { 4, 0, 1, 2, 3 };

            /* Check which vectors are used after this operation */
            int used[4] = { 0 };
            LOOP_IN(i) {
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
            int vpos[4][5];
            for (int i = 0; i < 4; i++) {
                int count = 0;
                for (int j = 0; j < 5; j++) {
                    int sj = fdata_swizzle[j];
                    if (op.lin.m[i][sj].num) {
                        if (count == 0 && i == sj && (op.lin.m[i][sj].num == 1 && op.lin.m[i][sj].den == 1)) {
                            /* Don't emit identify coefficient where a mov will be used */
                            vpos[i][sj] = -2;
                        } else {
                            vpos[i][sj] = ctx->push_q(op.lin.m[i][sj]);
                        }
                        count++;
                    } else {
                        vpos[i][sj] = -1;
                    }
                }
            }

            /* Do the salmon dance */
            cc.comment("linear");
            LOOP_IN(i) {
                save_vector(ctx, i);
            }
            LOOP_ARRAY(i, used) {
                new_vector(ctx, i);
                int count = 0;
                for (int j = 0; j < 5; j++) {
                    int sj = fdata_swizzle[j];
                    int vidx = vpos[i][sj];
                    if (vidx != -1) {
                        if (j == 0)
                            cc.dup(vl[i].s4(), ctx->vdata(vidx));
                        else if (vidx == -2)
                            cc.mov(vl[i].s4(), orig_vl[sj].s4());
                        else if (count == 0)
                            cc.fmul(vl[i].s4(), orig_vl[sj].s4(), ctx->vdata(vidx));
                        else
                            cc.fmla(vl[i].s4(), orig_vl[sj].s4(), ctx->vdata(vidx));
                        count++;
                    }
                }
                count = 0;
                for (int j = 0; j < 5; j++) {
                    int sj = fdata_swizzle[j];
                    int vidx = vpos[i][sj];
                    if (vidx != -1) {
                        if (j == 0)
                            cc.dup(vh[i].s4(), ctx->vdata(vidx));
                        else if (vidx == -2)
                            cc.mov(vh[i].s4(), orig_vh[sj].s4());
                        else if (count == 0)
                            cc.fmul(vh[i].s4(), orig_vh[sj].s4(), ctx->vdata(vidx));
                        else
                            cc.fmla(vh[i].s4(), orig_vh[sj].s4(), ctx->vdata(vidx));
                        count++;
                    }
                }
            }
        }
        break;
    case SWS_OP_SCALE:           /* multiplication by scalar */
        if (op.type == SWS_PIXEL_F32) {
            /* Add const data */
            size_t vidx = ctx->push_q(op.scale.factor);

            /* Do the salmon dance */
            cc.comment("scale (f32)");
            LOOP_OUT(i) {
                refresh_vector(ctx, i);
                cc.fmul(vl[i].s4(), orig_vl[i].s4(), ctx->vdata(vidx));
                cc.fmul(vh[i].s4(), orig_vh[i].s4(), ctx->vdata(vidx));
            }
        } else if (op.type == SWS_PIXEL_U8 || op.type == SWS_PIXEL_U16 || op.type == SWS_PIXEL_U32) {
            /* Add immediate */
            size_t vidx = ctx->push_imm32_op(op, av_q2i(op.scale.factor));

            /* Do the salmon dance */
            cc.comment("scale (integer)");
            LOOP_OUT(i) {
                refresh_vector(ctx, i, use_vh ? 0xff : 0x0f);
                cc.mul    (vet(vl[i], op), vet(orig_vl[i], op), vet(vimm[vidx], op));
                if (use_vh)
                    cc.mul(vet(vh[i], op), vet(orig_vh[i], op), vet(vimm[vidx], op));
            }
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
    const unsigned cpu_flags = av_get_cpu_flags();
    if (!(cpu_flags & AV_CPU_FLAG_NEON))
        return AVERROR(ENOTSUP);

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

    ctx->emit_const();
    ctx->load_immediates();
    ctx->emit_loop(chain);

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
