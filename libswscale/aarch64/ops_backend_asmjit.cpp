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

/* Enable this #define for better performance on in-order cores. */
// #define IN_ORDER_CORE

// #define EMIT_BRK
#define WRITE_PERF_MAP

extern "C" {
#include "libavutil/cpu.h"

#include "../ops_internal.h"
#include "../ops_backend.h"
}

#include <asmjit/core.h>
#include <asmjit/a64.h>

#ifdef WRITE_PERF_MAP
#  include <unistd.h>
#endif

#include <iostream>
#include <vector>

#define av_q2f(q) ((q).den ? (float) (q).num / (q).den : 0)
#define av_q2i(q) ((q).den ? (int32_t) (q).num / (q).den : 0)

#define REGID_TMP_PTR 1
/* in: 4, 5, 6, 7 */
#define REGID_IN      4
/* out: 10, 11, 12, 13 */
#define REGID_OUT    10
#define REGID_X      14
#define REGID_Y      15

#define REGID_VSTX   20
#define REGID_VDATA  28

/* returns log2(x) only if x is a power of two, or 0 otherwise */
static int exact_log2(const int x)
{
    int p;
    if (x <= 0)
        return 0;
    p = av_log2(x);
    return (1 << p) == x ? p : 0;
}

#define LOOP_ARRAY(idx, arr)          \
    for (int idx = 0; idx < 4; idx++) \
        if (arr[idx])
#define LOOP_OUT(idx) LOOP_ARRAY(idx, !next->comps.unused)
#define LOOP_IN(idx)  LOOP_ARRAY(idx, !op.comps.unused)

using namespace asmjit;

struct AsmJitContext {
    int m_block_size;
    char m_func_name[64];

    JitRuntime m_rt;
    CodeHolder m_code;
    StringLogger m_logger;
    a64::Compiler *m_cc;

    FuncNode *m_func;
    BaseNode *m_prologue;
    BaseNode *m_tail;
    a64::Gp m_exec;
    a64::Gp m_num_blocks;
    a64::Gp m_num_lines;
    a64::Gp m_in[4];
    a64::Gp m_out[4];
    a64::Vec m_orig_vl[4];
    a64::Vec m_orig_vh[4];
    a64::Vec m_vl[4];
    a64::Vec m_vh[6];
    int m_vec_idx;

    a64::Gp m_x;
    a64::Gp m_y;

    bool m_read_used[4];
    bool m_write_used[4];
    int m_read_bytes;
    int m_write_bytes;
    const SwsOp *m_dither_op;

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

    AsmJitContext(const SwsOpList *ops, int block_size)
      : m_block_size(block_size)
    {
        m_code.init(m_rt.environment(), m_rt.cpuFeatures());
        if (av_log_get_level() >= AV_LOG_DEBUG)
            m_code.setLogger(&m_logger);
        m_cc = new a64::Compiler(&m_code);
        a64::Compiler &cc = *m_cc;
        cc.addDiagnosticOptions(DiagnosticOptions::kRAAnnotate);
        m_func = cc.addFunc(FuncSignature::build<void, uint8_t *, uint8_t *, int, int>());
        /* HACK to set function name in asmjit */
        {
            LabelNode *func_label_node = static_cast<LabelNode *>(m_func);
            uint32_t func_label_id = func_label_node->labelId();
            LabelEntry *func_label_entry = m_code.labelEntry(func_label_id);
            func_label_entry->_type = LabelType::kGlobal;
            snprintf(m_func_name, sizeof(m_func_name), "asmjit_%s_to_%s_neon",
                     av_get_pix_fmt_name(ops->src.format),
                     av_get_pix_fmt_name(ops->dst.format));
            func_label_entry->_name.setData(&m_code._zone, m_func_name, strlen(m_func_name));
        }
        m_prologue = cc.firstNode()->next();
        m_tail = cc.cursor();
#ifdef EMIT_BRK
        cc.comment("make asmjit happy");
        to_prologue();
        cc.comment("breakpoint");
        cc.brk(0x0f00);
        from_prologue();
#endif
        m_exec       = cc.newGpz("exec");
        m_num_blocks = cc.newGpw("num_blocks");
        m_num_lines  = cc.newGpw("num_lines");
        m_func->setArg(0, m_exec);
        m_func->setArg(2, m_num_blocks);
        m_func->setArg(3, m_num_lines);

        m_x = cc.newGpw("x");
        m_y = cc.newGpw("y");
#if 1
        cc.virtRegByReg(m_x)->setHomeIdHint(REGID_X);
        cc.virtRegByReg(m_y)->setHomeIdHint(REGID_Y);
#endif

        for (int i = 0; i < 4; i++) {
            m_read_used[i] = false;
            m_write_used[i] = false;
        }
        m_read_bytes = 0;
        m_write_bytes = 0;

        m_dither_op = nullptr;

        m_vec_idx = 0;
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
        char cbuf[64];
        snprintf(cbuf, sizeof(cbuf), "vimm%d", (int) ret);
        a64::Vec vimm = m_cc->newVecQ(cbuf);
        m_vimm.push_back(vimm);
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
        char cbuf[64];
        to_prologue();
        cc.comment("prologue (immediates)");
        std::vector<a64::Gp> tmp(size);
        /* First load immediates larger than 0xff into temporary registers */
        for (size_t i = 0; i < size; i++) {
            int small_value = m_imm[i].second >> 16;
            uint8_t repeat_len = m_imm[i].second >> 8;
            if (!small_value && repeat_len != 1)
            {
                snprintf(cbuf, sizeof(cbuf), "imm_tmp%d", (int) i);
                tmp[i] = cc.newGpz(cbuf);
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
            size_t vidx = (ret >> 2);
            char cbuf[64];
            snprintf(cbuf, sizeof(cbuf), "vdata%d", (int) vidx);
            a64::Vec vdata = m_cc->newVecQ(cbuf);
#if 0
            m_cc->virtRegByReg(vdata)->setHomeIdHint(REGID_VDATA + vidx);
#endif
            m_vdata.push_back(vdata);
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

    Label emit_data(void *data, size_t size, const char *name)
    {
        a64::Compiler &cc = *m_cc;

        Label ldata = cc.newNamedLabel(name);
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
        Label ldata = emit_data(m_data.data(), m_data.size() * sizeof(uint32_t), "const_data");

        /* Read matrix data into vectors */
        a64::Compiler &cc = *m_cc;
        a64::Gp ptr = cc.newGpz("const_data_ptr");
#if 1
        cc.virtRegByReg(ptr)->setHomeIdHint(REGID_TMP_PTR);
#endif

        to_prologue();
        cc.comment("prologue (const data)");
        cc.adr(ptr, ldata);
        switch (m_vdata.size()) {
        case 1: cc.ld1(m_vdata[0].b16(),                                                       a64::ptr(ptr)); break;
        case 2: cc.ld1(m_vdata[0].b16(), m_vdata[1].b16(),                                     a64::ptr(ptr)); break;
        case 3: cc.ld1(m_vdata[0].b16(), m_vdata[1].b16(), m_vdata[2].b16(),                   a64::ptr(ptr)); break;
        case 4: cc.ld1(m_vdata[0].b16(), m_vdata[1].b16(), m_vdata[2].b16(), m_vdata[3].b16(), a64::ptr(ptr)); break;
        case 5:
            /* help out asmjit with smaller ld1 */
            cc.ld1(m_vdata[0].b16(), m_vdata[1].b16(),                   a64::ptr(ptr).post(32));
            cc.ld1(m_vdata[2].b16(), m_vdata[3].b16(), m_vdata[4].b16(), a64::ptr(ptr));
            break;
        default:
            __builtin_trap();
            break;
        }
        from_prologue();
    }

    void emit_loop()
    {
        a64::Compiler &cc = *m_cc;
        a64::Gp orig_x;
        a64::Gp x_end;
        a64::Gp y_end;
        Label hloop = cc.newNamedLabel("hloop");
        Label vloop = cc.newNamedLabel("vloop");
        bool xy_used = (m_dither_op != nullptr);
        char cbuf[64];

        to_prologue();
        if (xy_used) {
            cc.comment("prologue (x/y)");
            orig_x = cc.newGpw("orig_x");
            x_end = cc.newGpw("x_end");
            y_end = cc.newGpw("y_end");
            cc.ldr(m_y, a64::ptr(m_exec, offsetof(SwsOpExec, y)));
            cc.ldr(orig_x, a64::ptr(m_exec, offsetof(SwsOpExec, x)));
            cc.add(y_end, m_y, m_num_lines);
            cc.add(x_end, orig_x, m_num_blocks, a64::lsl(av_log2(m_block_size)));
        }
        cc.comment("prologue (padding)");
        int read_increment = m_read_bytes * m_block_size;
        int write_increment = m_write_bytes * m_block_size;
        int read_increment_log2 = exact_log2(read_increment);
        int write_increment_log2 = exact_log2(write_increment);
        a64::Gp read_linesize;
        a64::Gp write_linesize;
        if (read_increment == write_increment) {
            read_linesize = cc.newGpw("rw_linesize");
            if (read_increment_log2 == 0)
                cc.mov(read_linesize, read_increment);
            write_linesize = read_linesize;
        } else {
            read_linesize = cc.newGpw("read_linesize");
            write_linesize = cc.newGpw("write_linesize");
            if (read_increment_log2 == 0)
                cc.mov(read_linesize, read_increment);
            if (write_increment_log2 == 0)
                cc.mov(write_linesize, write_increment);
        }
        a64::Gp in_padding[4];
        a64::Gp out_padding[4];
        LOOP_ARRAY(i, m_read_used) {
            snprintf(cbuf, sizeof(cbuf), "in_padding%d", i);
            in_padding[i] = cc.newGpz(cbuf);
            cc.ldr(in_padding[i], a64::ptr(m_exec, offsetof(SwsOpExec, in_stride) + (i * sizeof(ptrdiff_t))));
        }
        if (read_increment_log2 == 0) {
            cc.mul(read_linesize, read_linesize, m_num_blocks);
        } else {
            cc.lsl(read_linesize, m_num_blocks, read_increment_log2);
        }
        if (read_increment != write_increment) {
            if (write_increment_log2 == 0) {
                cc.mul(write_linesize, write_linesize, m_num_blocks);
            } else {
                cc.lsl(write_linesize, m_num_blocks, write_increment_log2);
            }
        }
        LOOP_ARRAY(i, m_write_used) {
            snprintf(cbuf, sizeof(cbuf), "out_padding%d", i);
            out_padding[i] = cc.newGpz(cbuf);
            cc.ldr(out_padding[i], a64::ptr(m_exec, offsetof(SwsOpExec, out_stride) + (i * sizeof(ptrdiff_t))));
        }
        LOOP_ARRAY(i, m_read_used) {
            cc.sub(in_padding[i], in_padding[i], read_linesize.r64());
        }
        LOOP_ARRAY(i, m_write_used) {
            cc.sub(out_padding[i], out_padding[i], write_linesize.r64());
        }

        if (!xy_used) {
            cc.comment("line fusing optimization");
            a64::Gp tmp_padding = cc.newGpz("tmp_padding");
            a64::Gp tmp_num_blocks = cc.newGpw("tmp_num_blocks");
            a64::Gp tmp_num_lines = cc.newGpw("tmp_num_lines");
            cc.orr(tmp_padding, in_padding[0], out_padding[0]);
            LOOP_ARRAY(i, m_read_used) {
                if (i != 0)
                    cc.orr(tmp_padding, tmp_padding, in_padding[i]);
            }
            LOOP_ARRAY(i, m_write_used) {
                if (i != 0)
                    cc.orr(tmp_padding, tmp_padding, out_padding[i]);
            }
            cc.cmp(tmp_padding, 0);
            cc.mul(tmp_num_blocks, m_num_blocks, m_num_lines);
            cc.mov(tmp_num_lines, 1);
            cc.csel(m_num_blocks, tmp_num_blocks, m_num_blocks, a64::CondCode::kEQ);
            cc.csel(m_num_lines, tmp_num_lines, m_num_lines, a64::CondCode::kEQ);
        }

        cc.bind(vloop);
        if (xy_used) {
            cc.mov(m_x, orig_x);
        } else {
            cc.mov(m_x, m_num_blocks);
        }
        cc.bind(hloop);
        from_prologue();

        cc.comment("horizontal loop back");
        if (xy_used) {
            cc.add(m_x, m_x, m_block_size);
            cc.cmp(m_x, x_end);
            cc.b(a64::CondCode::kLO, hloop);
        } else {
            cc.subs(m_x, m_x, 1);
            cc.b(a64::CondCode::kGT, hloop);
        }

        cc.comment("padding");
        LOOP_ARRAY(i, m_read_used) {
            cc.add(m_in[i], m_in[i], in_padding[i]);
        }
        LOOP_ARRAY(i, m_write_used) {
            cc.add(m_out[i], m_out[i], out_padding[i]);
        }

        cc.comment("vertical loop back");
        if (xy_used) {
            cc.add(m_y, m_y, 1);
            cc.cmp(m_y, y_end);
            cc.b(a64::CondCode::kLO, vloop);
        } else {
            cc.subs(m_num_lines, m_num_lines, 1);
            cc.b(a64::CondCode::kGT, vloop);
        }
    }
};

/* Vector element type for given SwsOp */
struct VectorElementType {
    VectorElementType(int block_size)
        : m_block_size(block_size)
    {
    }

    a64::Vec operator()(const a64::Vec &vreg, const SwsOp &op) const
    {
        if (op.type == SWS_PIXEL_U8  && m_block_size == 8)
            return vreg.b8(); /* half vector */
        if (op.type == SWS_PIXEL_U16 && m_block_size == 8)
            return vreg.h8(); /* full vector */
        if (op.type == SWS_PIXEL_U32 && m_block_size == 8)
            return vreg.s4(); /* full vector (TRUNCATE) */
        if (op.type == SWS_PIXEL_F32 && m_block_size == 8)
            return vreg.s4(); /* full vector (TRUNCATE) */
        if (op.type == SWS_PIXEL_U8  && m_block_size == 16)
            return vreg.b16(); /* full vector */
        if (op.type == SWS_PIXEL_U16 && m_block_size == 16)
            return vreg.h8(); /* full vector (TRUNCATE) */
        if (op.type == SWS_PIXEL_U32 && m_block_size == 16)
            return vreg.s4(); /* full vector (TRUNCATE) */
        if (op.type == SWS_PIXEL_F32 && m_block_size == 16)
            return vreg.s4(); /* full vector (TRUNCATE) */
        printf("ERORROORORRORORO %d %d\n", op.type, m_block_size);
        return vreg.b16();
    }

    int size(const SwsOp &op)
    {
        int elsize = (op.type == SWS_PIXEL_U8)  ? 1
                   : (op.type == SWS_PIXEL_U16) ? 2
                   :                              4;
        int ret = m_block_size * elsize;
        return FFMIN(ret, 16);
    }

    int m_block_size;
};

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
    char cbuf[64];
    for (int i = 0; i < 4; i++) {
        if (mask & (1 << i)) {
            snprintf(cbuf, sizeof(cbuf), "vl%d_%d", i, ctx->m_vec_idx);
            vl[i] = cc.newVecQ(cbuf);
        }
        if (mask & (1 << (i + 4))) {
            snprintf(cbuf, sizeof(cbuf), "vh%d_%d", i, ctx->m_vec_idx);
            vh[i] = cc.newVecQ(cbuf);
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

typedef enum SwsOpTypeAarch64 {
    SWS_OP_AARCH64_INVALID = SWS_OP_TYPE_NB,
    SWS_OP_AARCH64_WIDEN_LSHIFT,
    SWS_OP_AARCH64_SATURATING_CONVERT,
    SWS_OP_AARCH64_READ_BYTES,
    SWS_OP_AARCH64_WRITE_BYTES,
    SWS_OP_AARCH64_SHUFFLE_BYTES,
} SwsOpTypeAarch64;

typedef struct SwsWidenLshiftOp {
    SwsConvertOp convert;
    unsigned lshift;
} SwsWidenLshiftOp;

static void asmjit_optimize(SwsOpList *ops)
{
retry:
    for (int n = 0; n < ops->num_ops;) {
        SwsOp dummy = { SWS_OP_INVALID };
        SwsOp *op = &ops->ops[n];
        SwsOp *prev = n ? &ops->ops[n - 1] : &dummy;
        SwsOp *next = n + 1 < ops->num_ops ? &ops->ops[n + 1] : &dummy;

        switch (op->op) {
        case SWS_OP_MIN:
            /* Saturating convert */
            if (next->op == SWS_OP_CONVERT && next->type == SWS_PIXEL_F32 && next->convert.to < SWS_PIXEL_U32) {
                bool to_u16 = (next->convert.to == SWS_PIXEL_U16);
                int u = to_u16 ? 65535 : 255;
                AVRational q = av_make_q(u, 1);
                bool enable = true;
                LOOP_OUT(i) {
                    enable &= (av_cmp_q(op->c.q4[i], q) == 0);
                }
                if (enable) {
                    op->op = SWS_OP_CONVERT;
                    op->type = SWS_PIXEL_F32;
                    op->convert.to = to_u16 ? SWS_PIXEL_U32 : SWS_PIXEL_U16;
                    op->convert.expand = false;

                    next->op = (SwsOpType) SWS_OP_AARCH64_SATURATING_CONVERT;
                    next->type = op->convert.to;
                    next->convert.to = to_u16 ? SWS_PIXEL_U16 : SWS_PIXEL_U8; /* unnecessary, setting to itself */

                    goto retry;
                }
            }
            break;

        case SWS_OP_MAX:
            /* Check whether a conversion will implicitly clamp negative values */
            if (op->type == SWS_PIXEL_F32) {
                for (int i = n; i < ops->num_ops; i++) {
                    if (ops->ops[i].op == SWS_OP_CONVERT && ops->ops[i].convert.to != SWS_PIXEL_F32) {
                        ff_sws_op_list_remove_at(ops, n, 1);
                        goto retry;
                    }
                }
            }
            break;

        case SWS_OP_CONVERT:
            /* Simplify widen+lshift by zip with zero or ushll */
            if (op->type == SWS_PIXEL_U8 && op->convert.to == SWS_PIXEL_U16 && !op->convert.expand &&
                next->op == SWS_OP_LSHIFT)
            {
                SwsWidenLshiftOp *priv = (SwsWidenLshiftOp *) &op->convert;
                priv->lshift = next->c.u;
                op->op = (SwsOpType) SWS_OP_AARCH64_WIDEN_LSHIFT;
                ff_sws_op_list_remove_at(ops, n + 1, 1);
                goto retry;
            }
            break;
        }

        /* No optimization triggered, move on to next operation */
        n++;
    }
}

static int asmjit_compile_op(AsmJitContext *ctx, const SwsOpList *ops, int n)
{
    int block_size = ctx->m_block_size;

    a64::Compiler &cc = *ctx->m_cc;
    a64::Gp &exec = ctx->m_exec;
    a64::Vec *orig_vl = ctx->m_orig_vl;
    a64::Vec *orig_vh = ctx->m_orig_vh;
    a64::Vec *vl = ctx->m_vl;
    a64::Vec *vh = ctx->m_vh;
    std::vector<a64::Vec> &vimm = ctx->m_vimm;

    const SwsOp &op = ops->ops[n];
    const SwsOp *next = &ops->ops[n + 1];

    VectorElementType vet(block_size);

    bool use_vh = ((op.type == SWS_PIXEL_U16) && block_size == 16)
               || ((op.type == SWS_PIXEL_U32) && block_size == 8)
               || ((op.type == SWS_PIXEL_F32) && block_size == 8);

    char cbuf[64];

    switch (op.op) {
    /* Input/output handling */
    case SWS_OP_AARCH64_READ_BYTES:
    case SWS_OP_READ:            /* gather raw pixels from planes */
        if (op.rw.frac)
            return AVERROR(ENOTSUP);
        if (op.op == SWS_OP_READ)
            ctx->m_read_bytes = ff_sws_pixel_type_size(op.type) * (op.rw.packed ? op.rw.elems : 1);
        cc.comment("read");
        if (!op.rw.packed) {
            /* Load input pointers in prologue */
            ctx->to_prologue();
            cc.comment("prologue (read)");
            LOOP_OUT(i) {
                snprintf(cbuf, sizeof(cbuf), "in%d", i);
                ctx->m_in[i] = cc.newGpz(cbuf);
#if 1
                cc.virtRegByReg(ctx->m_in[i])->setHomeIdHint(REGID_IN + i);
#endif
                cc.ldr(ctx->m_in[i], a64::ptr(exec, offsetof(SwsOpExec, in) + sizeof(uint8_t *) * i));
                ctx->m_read_used[i] = true;
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
            ctx->m_in[0] = cc.newGpz("in0");
#if 1
            cc.virtRegByReg(ctx->m_in[0])->setHomeIdHint(REGID_IN);
#endif
            cc.ldr(ctx->m_in[0], a64::ptr(exec, offsetof(SwsOpExec, in)));
            ctx->m_read_used[0] = true;
            ctx->from_prologue();
            /* Read vectors from input pointer */
            if (op.op == SWS_OP_AARCH64_READ_BYTES) {
                for (int i = 0; i < ctx->m_read_bytes; i += 16) {
                    snprintf(cbuf, sizeof(cbuf), "vin%d", i, ctx->m_vec_idx);
                    vl[i] = cc.newVecQ(cbuf);
                }
                switch (ctx->m_read_bytes) {
                case 16:
                    cc.ld1(vl[0].b16(),                                        a64::ptr(ctx->m_in[0]).post(ctx->m_read_bytes));
                    break;
                case 32:
                    cc.ld1(vl[0].b16(), vl[1].b16(),                           a64::ptr(ctx->m_in[0]).post(ctx->m_read_bytes));
                    break;
                case 48:
                    cc.ld1(vl[0].b16(), vl[1].b16(), vl[2].b16(),              a64::ptr(ctx->m_in[0]).post(ctx->m_read_bytes));
                    break;
                case 64:
                    cc.ld1(vl[0].b16(), vl[1].b16(), vl[2].b16(), vl[3].b16(), a64::ptr(ctx->m_in[0]).post(ctx->m_read_bytes));
                    break;
                }
            } else {
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
        }
        break;
    case SWS_OP_WRITE:           /* write raw pixels to planes */
        if (op.rw.frac)
            return AVERROR(ENOTSUP);
        if (op.op == SWS_OP_WRITE)
            ctx->m_write_bytes = ff_sws_pixel_type_size(op.type) * (op.rw.packed ? op.rw.elems : 1);
        cc.comment("write");
        if (!op.rw.packed) {
            /* Load output pointers in prologue */
            ctx->to_prologue();
            cc.comment("prologue (write)");
            LOOP_IN(i) {
                snprintf(cbuf, sizeof(cbuf), "out%d", i);
                ctx->m_out[i] = cc.newGpz(cbuf);
#if 1
                cc.virtRegByReg(ctx->m_out[i])->setHomeIdHint(REGID_OUT + i);
#endif
                cc.ldr(ctx->m_out[i], a64::ptr(exec, offsetof(SwsOpExec, out) + sizeof(uint8_t *) * i));
                ctx->m_write_used[i] = true;
            }
            ctx->from_prologue();
            /* Write vectors to output pointers */
            LOOP_IN(i) {
#if 0
                /* TODO reenable after the shuffle solver is done */
                cc.virtRegByReg    (vl[0])->setHomeIdHint(REGID_VSTX + (i * 2) + 0);
                if (use_vh)
                    cc.virtRegByReg(vh[0])->setHomeIdHint(REGID_VSTX + (i * 2) + 1);
#endif
                if (use_vh)
                    cc.st1(vet(vl[i], op), vet(vh[i], op), a64::ptr(ctx->m_out[i]).post(vet.size(op) * 2));
                else
                    cc.st1(vet(vl[i], op),                 a64::ptr(ctx->m_out[i]).post(vet.size(op) * 1));
            }
        } else {
            /* Load output pointer in prologue */
            ctx->to_prologue();
            cc.comment("prologue (write)");
            ctx->m_out[0] = cc.newGpz("out0");
#if 1
            cc.virtRegByReg(ctx->m_out[0])->setHomeIdHint(REGID_OUT);
#endif
            cc.ldr(ctx->m_out[0], a64::ptr(exec, offsetof(SwsOpExec, out)));
            ctx->m_write_used[0] = true;
            ctx->from_prologue();
            /* Write vectors to output pointer */
            if (op.op == SWS_OP_AARCH64_WRITE_BYTES) {
#if 1
                for (int i = 0; i < ctx->m_read_bytes; i += 16) {
                    cc.virtRegByReg(vh[i])->setHomeIdHint(REGID_VSTX + i);
                }
#endif
                switch (ctx->m_read_bytes) {
                case 16:
                    cc.st1(vh[0].b16(),                                        a64::ptr(ctx->m_out[0]).post(ctx->m_read_bytes));
                    break;
                case 32:
                    cc.st1(vh[0].b16(), vh[1].b16(),                           a64::ptr(ctx->m_out[0]).post(ctx->m_read_bytes));
                    break;
                case 48:
                    cc.st1(vh[0].b16(), vh[1].b16(), vh[2].b16(),              a64::ptr(ctx->m_out[0]).post(ctx->m_read_bytes));
                    break;
                case 64:
                    cc.st1(vh[0].b16(), vh[1].b16(), vh[2].b16(), vh[3].b16(), a64::ptr(ctx->m_out[0]).post(ctx->m_read_bytes));
                    break;
                case 96:
                    cc.st1(vh[0].b16(), vh[1].b16(), vh[2].b16(),              a64::ptr(ctx->m_out[0]).post(ctx->m_read_bytes));
                    cc.st1(vh[3].b16(), vh[4].b16(), vh[5].b16(),              a64::ptr(ctx->m_out[0]).post(ctx->m_read_bytes));
                    break;
                }
            } else {
#if 1
                for (int i = 0; i < op.rw.elems; i++) {
                    cc.virtRegByReg    (vl[i])->setHomeIdHint(REGID_VSTX + i);
                    if (use_vh)
                        cc.virtRegByReg(vh[i])->setHomeIdHint(REGID_VSTX + i + 4);
                }
#endif
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
                    cc.st4    (vet(vl[0], op), vet(vl[1], op), vet(vl[2], op), vet(vl[3], op), a64::ptr(ctx->m_out[0]).post(vet.size(op) * 4));
                    if (use_vh)
                        cc.st4(vet(vh[0], op), vet(vh[1], op), vet(vh[2], op), vet(vh[3], op), a64::ptr(ctx->m_out[0]).post(vet.size(op) * 4));
                    break;
                }
            }
        }
        break;
    case SWS_OP_SWAP_BYTES:      /* swap byte order (for differing endianness) */
        if        (op.type == SWS_PIXEL_U16) {
            cc.comment("swap_bytes (u16)");
            ctx->m_vec_idx++;
            LOOP_OUT(i) {
                refresh_vector(ctx, i, use_vh ? 0xff : 0x0f);
                cc.rev16    (vl[i].b16(), orig_vl[i].b16());
                if (use_vh)
                    cc.rev16(vh[i].b16(), orig_vh[i].b16());
            }
        } else if (op.type == SWS_PIXEL_U32 || op.type == SWS_PIXEL_F32) {
            cc.comment("swap_bytes (u32)");
            ctx->m_vec_idx++;
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
            bool update0 = false;
            LOOP_OUT(i) {
                if (offsets[i]) {
                    update0 = true;
                }
            }
            if (update0)
                ctx->m_vec_idx++;
            LOOP_OUT(i) {
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
            ctx->m_vec_idx++;
            LOOP_OUT(i) {
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
            bool update0 = false;
            bool update1 = false;
            LOOP_IN(i) {
                if (offsets[i])
                    update0 = true;
                if (i != 0)
                    update1 = true;
            }
            if (update0)
                ctx->m_vec_idx++;
            LOOP_IN(i) {
                if (offsets[i]) {
                    refresh_vector(ctx, i, use_vh ? 0xff : 0x0f);
                    cc.shl    (vet(vl[i], op), vet(orig_vl[i], op), offsets[i]);
                    if (use_vh)
                        cc.shl(vet(vh[i], op), vet(orig_vh[i], op), offsets[i]);
                }
            }
            if (update1)
                ctx->m_vec_idx++;
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
            bool update0 = false;
            for (int i = 0; i < 4; i++) {
                if (op.c.q4[i].den) {
                    if (next->op == SWS_OP_WRITE) {
                        update0 = true;
                    }
                }
            }
            if (update0)
                ctx->m_vec_idx++;
            for (int i = 0; i < 4; i++) {
                if (op.c.q4[i].den) {
                    size_t vidx = ctx->push_imm32_op(op, av_q2i(op.c.q4[i]));
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
            bool update0 = false;
            for (int i = 0; i < 4; i++) {
                if (op.c.q4[i].den) {
                    vpos[i] = ctx->push_q(op.c.q4[i]);
                    update0 = true;
                }
            }

            /* Do the salmon dance */
            cc.comment("clear (f32)");
            if (update0)
                ctx->m_vec_idx++;
            for (int i = 0; i < 4; i++) {
                if (op.c.q4[i].den) {
                    new_vector(ctx, i);
                    cc.dup(vl[i].s4(), ctx->vdata(vpos[i]));
                    cc.dup(vh[i].s4(), ctx->vdata(vpos[i]));
                }
            }
        }
        break;
    case SWS_OP_LSHIFT:          /* logical left shift of raw pixel values by (u8) */
        if (op.type == SWS_PIXEL_U8 || op.type == SWS_PIXEL_U16 || op.type == SWS_PIXEL_U32) {
            cc.comment("lshift");
            ctx->m_vec_idx++;
            LOOP_OUT(i) {
                refresh_vector(ctx, i, use_vh ? 0xff : 0x0f);
                cc.shl    (vet(vl[i], op), vet(orig_vl[i], op), op.c.u);
                if (use_vh)
                    cc.shl(vet(vh[i], op), vet(orig_vh[i], op), op.c.u);
            }
        } else {
            return AVERROR(ENOTSUP);
        }
        break;
    case SWS_OP_RSHIFT:          /* right shift of raw pixel values by (u8) */
        if (op.type == SWS_PIXEL_U8 || op.type == SWS_PIXEL_U16 || op.type == SWS_PIXEL_U32) {
            cc.comment("rshift");
            ctx->m_vec_idx++;
            LOOP_OUT(i) {
                refresh_vector(ctx, i, use_vh ? 0xff : 0x0f);
                cc.ushr    (vet(vl[i], op), vet(orig_vl[i], op), op.c.u);
                if (use_vh)
                    cc.ushr(vet(vh[i], op), vet(orig_vh[i], op), op.c.u);
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
                bool update0 = false;
                LOOP_OUT(i) {
                    if (i != op.swizzle.in[i]) {
                        update0 = true;
                    }
                }
                if (update0)
                    ctx->m_vec_idx++;
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
        {
            SwsPixelType from = op.type;
            SwsPixelType to = op.convert.to;
            int from_size = ff_sws_pixel_type_size(from);
            int to_size   = ff_sws_pixel_type_size(to);

            if (op.convert.expand) {
                snprintf(cbuf, sizeof(cbuf), "expand(%s -> %s, block_w %d)", ff_sws_pixel_type_name(from), ff_sws_pixel_type_name(to), block_size);
                cc.comment(cbuf);
                if (from_size == 1) {
                    ctx->m_vec_idx++;
                    LOOP_OUT(i) {
                        save_vector(ctx, i, 0x0f);
                        new_vector(ctx, i, (block_size == 16) ? 0xff : 0x0f);
                        cc.zip1    (vl[i].b16(), orig_vl[i].b16(), orig_vl[i].b16());
                        if (block_size == 16)
                            cc.zip2(vh[i].b16(), orig_vl[i].b16(), orig_vl[i].b16());
                    }
                }
                if (to_size == 4) {
                    ctx->m_vec_idx++;
                    LOOP_OUT(i) {
                        save_vector(ctx, i, 0x0f);
                        new_vector(ctx, i);
                        cc.zip1(vl[i].b16(), orig_vl[i].b16(), orig_vl[i].b16());
                        cc.zip2(vh[i].b16(), orig_vl[i].b16(), orig_vl[i].b16());
                    }
                }
            } else {
                snprintf(cbuf, sizeof(cbuf), "convert(%s -> %s, block_w %d)", ff_sws_pixel_type_name(from), ff_sws_pixel_type_name(to), block_size);
                cc.comment(cbuf);
                if (from == SWS_PIXEL_F32) {
                    ctx->m_vec_idx++;
                    LOOP_OUT(i) {
                        refresh_vector(ctx, i);
                        cc.fcvtzu(vl[i].s4(), orig_vl[i].s4());
                        cc.fcvtzu(vh[i].s4(), orig_vh[i].s4());
                    }
                }
                if (block_size == 8) {
                    if        (from_size == 1 && to_size > from_size) {
                        ctx->m_vec_idx++;
                        LOOP_OUT(i) {
                            refresh_vector(ctx, i, 0x0f);
                            cc.uxtl(vl[i].h8(), orig_vl[i].b8());
                        }
                        from_size = 2;
                    } else if (from_size == 4 && to_size < from_size) {
                        ctx->m_vec_idx++;
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
                        ctx->m_vec_idx++;
                        LOOP_OUT(i) {
                            save_vector(ctx, i, 0x0f);
                            new_vector(ctx, i);
                            cc.uxtl (vl[i].s4(), orig_vl[i].h4());
                            cc.uxtl2(vh[i].s4(), orig_vl[i].h8());
                        }
                        from_size = 4;
                    } else if (from_size == 2 && to_size == 1) {
                        ctx->m_vec_idx++;
                        LOOP_OUT(i) {
                            refresh_vector(ctx, i, 0x0f);
                            cc.xtn(vl[i].b8(), orig_vl[i].h8());
                        }
                        from_size = 1;
                    }
                } else /* if (block_size == 16) */ {
                    if        (from_size == 1 && to_size == 2) {
                        ctx->m_vec_idx++;
                        LOOP_OUT(i) {
                            save_vector(ctx, i, 0x0f);
                            new_vector(ctx, i);
                            cc.uxtl (vl[i].h8(), orig_vl[i].b8());
                            cc.uxtl2(vh[i].h8(), orig_vl[i].b16());
                        }
                    } else if (from_size == 2 && to_size == 1) {
                        ctx->m_vec_idx++;
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
                    ctx->m_vec_idx++;
                    LOOP_OUT(i) {
                        refresh_vector(ctx, i);
                        cc.ucvtf(vl[i].s4(), orig_vl[i].s4());
                        cc.ucvtf(vh[i].s4(), orig_vh[i].s4());
                    }
                }
            }
        }
        break;
    case SWS_OP_DITHER:          /* add dithering noise */
        if (op.dither.size_log2 == 0) {
            /* TODO dither(none) + convert(f32->u) use rounding convert instead */
            cc.comment("dither (none)");

            size_t vidx = ctx->push_immq(op.dither.matrix[0]);
            ctx->m_vec_idx++;
            LOOP_OUT(i) {
                refresh_vector(ctx, i);
                cc.fadd(vl[i].s4(), orig_vl[i].s4(), vet(vimm[vidx], op));
                cc.fadd(vh[i].s4(), orig_vh[i].s4(), vet(vimm[vidx], op));
            }
        } else {
            cc.comment("dither");

            /* Used by emit_loop to optimize away the use of x and y */
            ctx->m_dither_op = &op;

            static const int y_off[4] = { 0, 3, 2, 5 };
            int largest_y_off = 0;
            LOOP_OUT(i) {
                largest_y_off = FFMAX(largest_y_off, y_off[i]);
            }

            /* Write const data after function */
            int size = 1 << op.dither.size_log2;
            std::vector<float> fdata;
            fdata.resize((size + largest_y_off) * size);
            for (int i = 0; i < (size + largest_y_off) * size; i++) {
                fdata[i] = av_q2f(op.dither.matrix[i & ((size * size) - 1)]);
            }
            Label ldata = ctx->emit_data(fdata.data(), (size + largest_y_off) * size * sizeof(float), "dither_matrix");

            /* Pointer to dither_matrix */
            a64::Gp ptr = cc.newGpz("dither_matrix_ptr");
#if 1
            cc.virtRegByReg(ptr)->setHomeIdHint(REGID_TMP_PTR);
#endif

            BaseNode *last_use_of_ptr = nullptr;

            int last_y_off = -1;
            ctx->m_vec_idx++;
            LOOP_OUT(i) {
                // offset = ((((y + yoff[i]) & mask) << log2_size) + (x & mask)) * sizeof(float32);

                a64::Vec dither_vl = cc.newVecQ("vditherl");
                a64::Vec dither_vh = cc.newVecQ("vditherh");

                if (last_y_off < 0) {
                    /* On the first run, calculate pointer inside dither_matrix */

                    /* Load address of dither_matrix */
                    cc.adr(ptr, ldata);

                    /* y = ((y + y_off[i]) & ((1 << size_log2) - 1)) * (1 << size_log2) * sizeof(float32) */
                    a64::Gp y = cc.newGpw("tmp_y");
                    if (y_off[i] == 0) {
                        cc.ubfiz(y, ctx->m_y, op.dither.size_log2 + 2, op.dither.size_log2);
                    } else {
                        cc.add  (y, ctx->m_y, y_off[i]);
                        cc.ubfiz(y, y, op.dither.size_log2 + 2, op.dither.size_log2);
                    }

                    /* x = (x & ((1 << size_log2) - 1)) * sizeof(float32) */
                    a64::Gp x = cc.newGpw("tmp_x");
                    cc.ubfiz(x, ctx->m_x, 2, op.dither.size_log2);

                    /* ptr = dither_matrix_ptr + y + x */
                    cc.add(ptr, ptr, y.r64());
                    cc.add(ptr, ptr, x.r64());
                } else {
                    /* On subsequent runs, just increment the pointer.
                     * The matrix repeats itself at the end, so we don't risk overreading.
                     */
                    last_use_of_ptr = cc.setCursor(last_use_of_ptr);
                    int offset = (y_off[i] - last_y_off) * size * sizeof(float);
                    if (offset < 0)
                        cc.sub(ptr, ptr, -offset);
                    else
                        cc.add(ptr, ptr, offset);
                    cc.setCursor(last_use_of_ptr);
                }
                last_y_off = y_off[i];

                cc.ld1(dither_vl.s4(), dither_vh.s4(), a64::ptr(ptr));
                last_use_of_ptr = cc.cursor();

                refresh_vector(ctx, i);
                cc.fadd(vl[i].s4(), orig_vl[i].s4(), dither_vl.s4());
                cc.fadd(vh[i].s4(), orig_vh[i].s4(), dither_vh.s4());
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
                if (!used[i])
                    continue;
                bool is_identity = true;
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
            LOOP_ARRAY(i, used) {
                for (int j = 0; j < 5; j++) {
                    int sj = fdata_swizzle[j];
                    if (op.lin.m[i][sj].num) {
                        vpos[i][sj] = ctx->push_q(op.lin.m[i][sj]);
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
            ctx->m_vec_idx++;
            LOOP_ARRAY(i, used) {
                new_vector(ctx, i);
                for (int j = 0; j < 5; j++) {
                    int sj = fdata_swizzle[j];
                    int vidx = vpos[i][sj];
                    if (vidx != -1) {
                        if (j == 0) {
                            /* offset */
                            cc.dup(vl[i].s4(), ctx->vdata(vidx));
                            cc.dup(vh[i].s4(), ctx->vdata(vidx));
                        } else {
                            cc.fmul(vl[i].s4(), orig_vl[sj].s4(), ctx->vdata(vidx));
                            cc.fmul(vh[i].s4(), orig_vh[sj].s4(), ctx->vdata(vidx));
                        }
                        break;
                    }
                }
            }
#ifdef IN_ORDER_CORE
            /* Interleave all fmla instructions for better performance
             * on in-order cores.
             */
            for (int k = 0; k < 4; k++) {
                LOOP_ARRAY(i, used) {
                    int count = 0;
                    for (int j = 0; j < 5; j++) {
                        int sj = fdata_swizzle[j];
                        int vidx = vpos[i][sj];
                        if (vidx != -1 && count++ > k) {
                            cc.fmla(vl[i].s4(), orig_vl[sj].s4(), ctx->vdata(vidx));
                            cc.fmla(vh[i].s4(), orig_vh[sj].s4(), ctx->vdata(vidx));
                            break;
                        }
                    }
                }
            }
#else
            /* Group fmla by dst vector for better performance on
             * out-of-order cores (which have an fmla faspath).
             */
            LOOP_ARRAY(i, used) {
                int count = 0;
                for (int j = 0; j < 5; j++) {
                    int sj = fdata_swizzle[j];
                    int vidx = vpos[i][sj];
                    if (vidx != -1 && count++ != 0) {
                        cc.fmla(vl[i].s4(), orig_vl[sj].s4(), ctx->vdata(vidx));
                    }
                }
                count = 0;
                for (int j = 0; j < 5; j++) {
                    int sj = fdata_swizzle[j];
                    int vidx = vpos[i][sj];
                    if (vidx != -1 && count++ != 0) {
                        cc.fmla(vh[i].s4(), orig_vh[sj].s4(), ctx->vdata(vidx));
                    }
                }
            }
#endif
        }
        break;
    case SWS_OP_SCALE:           /* multiplication by scalar (q) */
        if (op.type == SWS_PIXEL_F32) {
            /* Add const data */
            size_t vidx = ctx->push_q(op.c.q);

            /* Do the salmon dance */
            cc.comment("scale (f32)");
            ctx->m_vec_idx++;
            LOOP_OUT(i) {
                refresh_vector(ctx, i);
                cc.fmul(vl[i].s4(), orig_vl[i].s4(), ctx->vdata(vidx));
                cc.fmul(vh[i].s4(), orig_vh[i].s4(), ctx->vdata(vidx));
            }
        } else if (op.type == SWS_PIXEL_U8 || op.type == SWS_PIXEL_U16 || op.type == SWS_PIXEL_U32) {
            /* Add immediate */
            size_t vidx = ctx->push_imm32_op(op, av_q2i(op.c.q));

            /* Do the salmon dance */
            cc.comment("scale (integer)");
            ctx->m_vec_idx++;
            LOOP_OUT(i) {
                refresh_vector(ctx, i, use_vh ? 0xff : 0x0f);
                cc.mul    (vet(vl[i], op), vet(orig_vl[i], op), vet(vimm[vidx], op));
                if (use_vh)
                    cc.mul(vet(vh[i], op), vet(orig_vh[i], op), vet(vimm[vidx], op));
            }
        }
        break;
    case SWS_OP_MIN:             /* numeric minimum (q4) */
        if (op.type == SWS_PIXEL_F32) {
            cc.comment("min (f32)");
            bool update0 = false;
            LOOP_OUT(i) {
                if (op.c.q4[i].den) {
                    update0 = true;
                }
            }
            if (update0)
                ctx->m_vec_idx++;
            LOOP_OUT(i) {
                if (op.c.q4[i].den) {
                    size_t vidx = ctx->push_immq(op.c.q4[i]);
                    refresh_vector(ctx, i, use_vh ? 0xff : 0x0f);
                    cc.fmin    (vl[i].s4(), orig_vl[i].s4(), vimm[vidx].s4());
                    if (use_vh)
                        cc.fmin(vh[i].s4(), orig_vh[i].s4(), vimm[vidx].s4());
                }
            }
        } else if (op.type == SWS_PIXEL_U8 || op.type == SWS_PIXEL_U16 || op.type == SWS_PIXEL_U32) {
            cc.comment("min (integer)");
            bool update0 = false;
            LOOP_OUT(i) {
                if (op.c.q4[i].den) {
                    update0 = true;
                }
            }
            if (update0)
                ctx->m_vec_idx++;
            LOOP_OUT(i) {
                if (op.c.q4[i].den) {
                    size_t vidx = ctx->push_imm32_op(op, av_q2i(op.c.q4[i]));
                    refresh_vector(ctx, i, use_vh ? 0xff : 0x0f);
                    cc.umin    (vet(vl[i], op), vet(orig_vl[i], op), vet(vimm[vidx], op));
                    if (use_vh)
                        cc.umin(vet(vh[i], op), vet(orig_vh[i], op), vet(vimm[vidx], op));
                }
            }
        }
        break;
    case SWS_OP_MAX:             /* numeric maximum (q4) */
        if (op.type == SWS_PIXEL_F32) {
            cc.comment("max");
            size_t vidx = ctx->push_imm32(0);
            bool update0 = false;
            LOOP_OUT(i) {
                if (op.c.q4[i].den) {
                    update0 = true;
                }
            }
            if (update0)
                ctx->m_vec_idx++;
            LOOP_OUT(i) {
                if (op.c.q4[i].den) {
                    refresh_vector(ctx, i, use_vh ? 0xff : 0x0f);
                    cc.fmax    (vl[i].s4(), orig_vl[i].s4(), vimm[vidx].s4());
                    if (use_vh)
                        cc.fmax(vh[i].s4(), orig_vh[i].s4(), vimm[vidx].s4());
                }
            }
        } else {
            return AVERROR(ENOTSUP);
        }
        break;

    case SWS_OP_AARCH64_WIDEN_LSHIFT:
        {
            SwsWidenLshiftOp *priv = (SwsWidenLshiftOp *) &op.convert;

            use_vh = (block_size == 16);

            snprintf(cbuf, sizeof(cbuf), "widen_lshift(%s -> %s, lshift %d)", ff_sws_pixel_type_name(op.type), ff_sws_pixel_type_name(priv->convert.to), priv->lshift);
            cc.comment(cbuf);

            if (priv->lshift == 8) {
                size_t vidx = ctx->push_imm8(0);
                ctx->m_vec_idx++;
                LOOP_OUT(i) {
                    save_vector(ctx, i, 0x0f);
                    new_vector(ctx, i, use_vh ? 0xff : 0x0f);
                    cc.zip1    (vl[i].b16(), vimm[vidx].b16(), orig_vl[i].b16());
                    if (use_vh)
                        cc.zip2(vh[i].b16(), vimm[vidx].b16(), orig_vl[i].b16());
                }
            } else /* if (priv->lshift < 8) */ {
                ctx->m_vec_idx++;
                LOOP_OUT(i) {
                    save_vector(ctx, i, 0x0f);
                    new_vector(ctx, i, use_vh ? 0xff : 0x0f);
                    cc.ushll     (vl[i].h8(), orig_vl[i].b8(),  priv->lshift);
                    if (use_vh)
                        cc.ushll2(vh[i].h8(), orig_vl[i].b16(), priv->lshift);
                }
            }
        }
        break;
    case SWS_OP_AARCH64_SATURATING_CONVERT:
        {
            snprintf(cbuf, sizeof(cbuf), "saturating_convert(%s -> %s)", ff_sws_pixel_type_name(op.type), ff_sws_pixel_type_name(op.convert.to));
            cc.comment(cbuf);
            if (op.convert.to == SWS_PIXEL_U16) {
                ctx->m_vec_idx++;
                LOOP_OUT(i) {
                    refresh_vector(ctx, i);
                    cc.uqxtn(vl[i].h4(), orig_vl[i].s4());
                    cc.uqxtn(vh[i].h4(), orig_vh[i].s4());
                }
                LOOP_OUT(i) {
                    cc.ins(vl[i].d(1), vh[i].d(0));
                }
            } else /* if (op.convert.to == SWS_PIXEL_U8) */ {
                ctx->m_vec_idx++;
                LOOP_OUT(i) {
                    refresh_vector(ctx, i, 0x0f);
                    cc.uqxtn(vl[i].b8(), orig_vl[i].h8());
                }
            }
        }
        break;

    default:
        return AVERROR(ENOTSUP);
    }

    return 0;
}

static av_cold void free_context(void *_ctx)
{
    AsmJitContext *ctx = static_cast<AsmJitContext *>(_ctx);
    if (ctx->m_cc)
        delete(ctx->m_cc);
    delete ctx;
}

static av_cold int asmjit_compile(SwsContext *swsctx, SwsOpList *ops, SwsCompiledOp *out)
{
    const unsigned cpu_flags = av_get_cpu_flags();
    if (!(cpu_flags & AV_CPU_FLAG_NEON))
        return AVERROR(ENOTSUP);

    /* Use at most two full vregs during the widest precision section */
    int block_size = (ff_sws_op_list_max_size(ops) == 4) ? 8 : 16;

    AsmJitContext *ctx = new AsmJitContext(ops, block_size);
    a64::Compiler &cc = *ctx->m_cc;
    Error err;

    asmjit_optimize(ops);

    for (int n = 0; n < ops->num_ops; n++) {
        if (asmjit_compile_op(ctx, ops, n) < 0)
            goto error;
    }

    ctx->emit_const();
    ctx->load_immediates();
    ctx->emit_loop();

    cc.ret();

    err = cc.endFunc();
    if (err) {
        std::cout << "Failed to end function: " << DebugUtils::errorAsString(err) << "\n";
        goto log_and_error;
    }
    err = cc.finalize();
    if (err) {
        std::cout << "Failed to finalize code: " << DebugUtils::errorAsString(err) << "\n";
        goto log_and_error;
    }

    ctx->m_rt.add(&out->func, &ctx->m_code);
    if (out->func == nullptr)
        goto error;

#ifdef WRITE_PERF_MAP
    {
        char path[64];
        snprintf(path, sizeof(path), "/tmp/perf-%d.map", getpid());

        FILE *fp = fopen(path, "a");
        if (fp) {
            uintptr_t address = (uintptr_t) out->func;
            size_t size = ctx->m_code.codeSize();
            const char *name = ctx->m_func_name;
            fprintf(fp, "%" PRIxPTR " %zx %s\n", address, size, name);
            fclose(fp);
        }
    }
#endif

    // At this point, logger already contains the output
    if (av_log_get_level() >= AV_LOG_DEBUG)
        std::cout << ctx->m_logger.data() << "\n";

    out->block_size = block_size;
    out->priv = ctx;
    out->free = free_context;

    return 0;

log_and_error:
    if (av_log_get_level() >= AV_LOG_DEBUG)
        std::cout << ctx->m_logger.data() << "\nXXX\n";
// __builtin_trap();

error:
    free_context(ctx);
    return AVERROR(ENOTSUP);
}

SwsOpBackend backend_asmjit = {
    .name    = "asmjit",
    .compile = asmjit_compile,
};
