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

/* General Purpose Registers */
#define REGID_TMP_PTR 1
/* in: 4, 5, 6, 7 */
#define REGID_IN      4
/* out: 10, 11, 12, 13 */
#define REGID_OUT    10
#define REGID_X      14
#define REGID_Y      15

/* Vector Registers */
/* vimm: 16, 17, 18, 19 */
#define REGID_VIMM   16
/* stx: 20, 21, 22, 23, 24, 25, 26, 27 */
#define REGID_VSTX   20
/* vdata: 28, 29, 30, 31 */
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
    a64::Vec m_src_vl[4];
    a64::Vec m_src_vh[4];
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
        cc.comment("inner loop");
        m_tail = cc.cursor();
#ifdef EMIT_BRK
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

    void new_step(void)
    {
        m_vec_idx++;
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
#if 1
        m_cc->virtRegByReg(vimm)->setHomeIdHint(REGID_VIMM + ret);
#endif
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
#if 1
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

        /* Align const data to 16 */
        while (m_data.size() & 3)
            m_data.push_back(0);
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
typedef struct VectorElementType {
    VectorElementType(const SwsOp &op, int block_size)
    {
        m_fmt_size = (op.type == SWS_PIXEL_U8)  ? 1
                   : (op.type == SWS_PIXEL_U16) ? 2
                   :                              4;
        int full_size = m_fmt_size * block_size;
        size = FFMIN(full_size, 16);
    }

    a64::Vec type(const a64::Vec &vreg) const
    {
        switch ((m_fmt_size << 8) | size) {
        case 0x0110: return vreg.b16();
        case 0x0108: return vreg.b8();
        case 0x0210: return vreg.h8();
        case 0x0410: return vreg.s4();
        }
        printf("ERORROORORRORORO %d %d\n", m_fmt_size, size);
        return vreg.b16();
    }

    uint8_t m_fmt_size;
    uint8_t size;
} VectorElementType;

static inline uint32_t mask_from_i(int i)
{
    return (1 << i) | (1 << (i + 4));
}

static inline void save_vectors_mask(AsmJitContext *ctx, VectorElementType *vet, uint32_t mask)
{
    a64::Vec *src_vl = ctx->m_src_vl;
    a64::Vec *src_vh = ctx->m_src_vh;
    a64::Vec *vl = ctx->m_vl;
    a64::Vec *vh = ctx->m_vh;
    for (int i = 0; i < 4; i++) {
        if (mask & (1 << i)) {
            src_vl[i] = vet->type(vl[i]);
        }
        if (mask & (1 << (i + 4))) {
            src_vh[i] = vet->type(vh[i]);
        }
    }
}

static inline a64::Vec new_vector_vet(AsmJitContext *ctx, VectorElementType *vet, const char *name)
{
    a64::Vec ret = ctx->m_cc->newVecQ(name);
    return vet->type(ret);
}

static inline void new_vectors_mask(AsmJitContext *ctx, VectorElementType *vet, uint32_t mask)
{
    a64::Vec *vl = ctx->m_vl;
    a64::Vec *vh = ctx->m_vh;
    char cbuf[64];
    for (int i = 0; i < 4; i++) {
        if (mask & (1 << i)) {
            snprintf(cbuf, sizeof(cbuf), "vl%d_%d", i, ctx->m_vec_idx);
            vl[i] = new_vector_vet(ctx, vet, cbuf);
        }
        if (mask & (1 << (i + 4))) {
            snprintf(cbuf, sizeof(cbuf), "vh%d_%d", i, ctx->m_vec_idx);
            vh[i] = new_vector_vet(ctx, vet, cbuf);
        }
    }
}

static inline void new_vector(AsmJitContext *ctx, VectorElementType *vet, int i, int mask = 0xff)
{
    mask &= mask_from_i(i);
    new_vectors_mask(ctx, vet, mask);
}

static inline void save_vector(AsmJitContext *ctx, VectorElementType *vet, int i, int mask = 0xff)
{
    mask &= mask_from_i(i);
    save_vectors_mask(ctx, vet, mask);
}

static inline void refresh_vector(AsmJitContext *ctx, VectorElementType *vet, int i, int mask = 0xff)
{
    mask &= mask_from_i(i);
    save_vectors_mask(ctx, vet, mask);
    new_vectors_mask(ctx, vet, mask);
}

typedef enum SwsOpTypeAarch64 {
    SWS_OP_AARCH64_INVALID = SWS_OP_TYPE_NB,
    SWS_OP_AARCH64_WIDEN_LSHIFT,
    SWS_OP_AARCH64_SATURATING_CONVERT,
    SWS_OP_AARCH64_SHUFFLE_BYTES,
} SwsOpTypeAarch64;

typedef struct SwsWidenLshiftOp {
    SwsConvertOp convert;
    unsigned lshift;
} SwsWidenLshiftOp;

typedef struct SwsShuffleOp {
    uint8_t data[128];
    int size;
    int read_bytes;
    int write_bytes;
} SwsShuffleOp;

static int asmjit_optimize(SwsOpList *ops, int block_size)
{
    /* First try the shuffle solver */
    uint8_t shuffle[128];
    int read_bytes;
    int write_bytes;
    int tmp_block_size = ff_sws_solve_shuffle(ops, shuffle, sizeof(shuffle), 16, 0x80, &read_bytes, &write_bytes);
    if (tmp_block_size >= 0) {
        /* Overwrite ops->ops[0] with the shuffle data */
        SwsShuffleOp *priv = (SwsShuffleOp *) &ops->ops[0].rw;
        ops->ops[0].op = (SwsOpType) SWS_OP_AARCH64_SHUFFLE_BYTES;
        memcpy(priv->data, shuffle, write_bytes);
        priv->size = write_bytes;
        priv->read_bytes = read_bytes;
        priv->write_bytes = write_bytes;
        ops->num_ops = 1;
        return tmp_block_size;
    }

    /* Continue with other optimizations */
retry:
    for (int n = 0; n < ops->num_ops;) {
        SwsOp dummy = { SWS_OP_INVALID };
        SwsOp *op = &ops->ops[n];
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

    return block_size;
}

static int asmjit_compile_op(AsmJitContext *ctx, const SwsOpList *ops, int n)
{
    int block_size = ctx->m_block_size;

    a64::Compiler &cc = *ctx->m_cc;
    a64::Gp &exec = ctx->m_exec;
    a64::Vec *src_vl = ctx->m_src_vl;
    a64::Vec *src_vh = ctx->m_src_vh;
    a64::Vec *vl = ctx->m_vl;
    a64::Vec *vh = ctx->m_vh;
    std::vector<a64::Vec> &vimm = ctx->m_vimm;

    const SwsOp &op = ops->ops[n];
    const SwsOp *next = &ops->ops[n + 1];

    VectorElementType vet(op, block_size);

    bool use_vh = ((op.type == SWS_PIXEL_U16) && block_size == 16)
               || ((op.type == SWS_PIXEL_U32) && block_size == 8)
               || ((op.type == SWS_PIXEL_F32) && block_size == 8);

    char cbuf[64];

    switch (op.op) {
    /* Input/output handling */
    case SWS_OP_READ:            /* gather raw pixels from planes */
        if (op.rw.frac)
            return AVERROR(ENOTSUP);
        ctx->m_read_bytes = ff_sws_pixel_type_size(op.type) * (op.rw.packed ? op.rw.elems : 1);
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
            cc.comment("read");
            LOOP_OUT(i) {
                new_vector(ctx, &vet, i, use_vh ? 0xff : 0x0f);
                if (use_vh)
                    cc.ld1(vl[i], vh[i], a64::ptr(ctx->m_in[i]).post(vet.size * 2));
                else
                    cc.ld1(vl[i],        a64::ptr(ctx->m_in[i]).post(vet.size * 1));
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
            cc.comment("read");
            for (int i = 0; i < op.rw.elems; i++) {
                new_vector(ctx, &vet, i, use_vh ? 0xff : 0x0f);
            }
            switch (op.rw.elems) {
            case 1:
                if (use_vh)
                    cc.ld1(vl[0], vh[0],               a64::ptr(ctx->m_in[0]).post(vet.size * 2));
                else
                    cc.ld1(vl[0],                      a64::ptr(ctx->m_in[0]).post(vet.size * 1));
                break;
            case 2:
                cc.ld2    (vl[0], vl[1],               a64::ptr(ctx->m_in[0]).post(vet.size * 2));
                if (use_vh)
                    cc.ld2(vh[0], vh[1],               a64::ptr(ctx->m_in[0]).post(vet.size * 2));
                break;
            case 3:
                cc.ld3    (vl[0], vl[1], vl[2],        a64::ptr(ctx->m_in[0]).post(vet.size * 3));
                if (use_vh)
                    cc.ld3(vh[0], vh[1], vh[2],        a64::ptr(ctx->m_in[0]).post(vet.size * 3));
                break;
            case 4:
                cc.ld4    (vl[0], vl[1], vl[2], vl[3], a64::ptr(ctx->m_in[0]).post(vet.size * 4));
                if (use_vh)
                    cc.ld4(vh[0], vh[1], vh[2], vh[3], a64::ptr(ctx->m_in[0]).post(vet.size * 4));
                break;
            }
        }
        break;
    case SWS_OP_WRITE:           /* write raw pixels to planes */
        if (op.rw.frac)
            return AVERROR(ENOTSUP);
        ctx->m_write_bytes = ff_sws_pixel_type_size(op.type) * (op.rw.packed ? op.rw.elems : 1);
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
            cc.comment("write");
            /* Write vectors to output pointers */
            LOOP_IN(i) {
#if 1
                cc.virtRegByReg    (vl[i])->setHomeIdHint(REGID_VSTX + (i * 2) + 0);
                if (use_vh)
                    cc.virtRegByReg(vh[i])->setHomeIdHint(REGID_VSTX + (i * 2) + 1);
#endif
                save_vector(ctx, &vet, i);
                if (use_vh)
                    cc.st1(src_vl[i], src_vh[i], a64::ptr(ctx->m_out[i]).post(vet.size * 2));
                else
                    cc.st1(src_vl[i],            a64::ptr(ctx->m_out[i]).post(vet.size * 1));
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
            cc.comment("write");
#if 1
            for (int i = 0; i < op.rw.elems; i++) {
                cc.virtRegByReg    (vl[i])->setHomeIdHint(REGID_VSTX + i);
                if (use_vh)
                    cc.virtRegByReg(vh[i])->setHomeIdHint(REGID_VSTX + i + 4);
            }
#endif
            for (int i = 0; i < op.rw.elems; i++) {
                save_vector(ctx, &vet, i, use_vh ? 0xff : 0x0f);
            }
            switch (op.rw.elems) {
            case 1:
                if (use_vh)
                    cc.st1(src_vl[0], src_vh[0],                       a64::ptr(ctx->m_out[0]).post(vet.size * 2));
                else
                    cc.st1(src_vl[0],                                  a64::ptr(ctx->m_out[0]).post(vet.size * 1));
                break;
            case 2:
                cc.st2    (src_vl[0], src_vl[1],                       a64::ptr(ctx->m_out[0]).post(vet.size * 2));
                if (use_vh)
                    cc.st2(src_vh[0], src_vh[1],                       a64::ptr(ctx->m_out[0]).post(vet.size * 2));
                break;
            case 3:
                cc.st3    (src_vl[0], src_vl[1], src_vl[2],            a64::ptr(ctx->m_out[0]).post(vet.size * 3));
                if (use_vh)
                    cc.st3(src_vh[0], src_vh[1], src_vh[2],            a64::ptr(ctx->m_out[0]).post(vet.size * 3));
                break;
            case 4:
                cc.st4    (src_vl[0], src_vl[1], src_vl[2], src_vl[3], a64::ptr(ctx->m_out[0]).post(vet.size * 4));
                if (use_vh)
                    cc.st4(src_vh[0], src_vh[1], src_vh[2], src_vh[3], a64::ptr(ctx->m_out[0]).post(vet.size * 4));
                break;
            }
        }
        break;
    case SWS_OP_SWAP_BYTES:      /* swap byte order (for differing endianness) */
        if        (op.type == SWS_PIXEL_U16) {
            cc.comment("swap_bytes (u16)");
            ctx->new_step();
            LOOP_OUT(i) {
                refresh_vector(ctx, &vet, i, use_vh ? 0xff : 0x0f);
                cc.rev16    (vl[i].b16(), src_vl[i].b16());
                if (use_vh)
                    cc.rev16(vh[i].b16(), src_vh[i].b16());
            }
        } else if (op.type == SWS_PIXEL_U32 || op.type == SWS_PIXEL_F32) {
            cc.comment("swap_bytes (u32)");
            ctx->new_step();
            LOOP_OUT(i) {
                refresh_vector(ctx, &vet, i, use_vh ? 0xff : 0x0f);
                cc.rev32    (vl[i].b16(), src_vl[i].b16());
                if (use_vh)
                    cc.rev32(vh[i].b16(), src_vh[i].b16());
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
            save_vector(ctx, &vet, 0);
            ctx->new_step();
            LOOP_OUT(i) {
                if (!offsets[i]) {
                    /* Move element with no offset */
                    vl[i] = src_vl[0];
                    if (use_vh)
                        vh[i] = src_vh[0];
                } else {
                    new_vector(ctx, &vet, i, use_vh ? 0xff : 0x0f);
                    cc.ushr    (vl[i], src_vl[0], offsets[i]);
                    if (use_vh)
                        cc.ushr(vh[i], src_vh[0], offsets[i]);
                }
            }
            ctx->new_step();
            LOOP_OUT(i) {
                uint32_t mask = (1u << op.pack.pattern[i]) - 1;
                size_t vidx = ctx->push_imm32_op(op, mask);
                refresh_vector(ctx, &vet, i, use_vh ? 0xff : 0x0f);
                cc.and_    (vl[i].b16(), src_vl[i].b16(), vimm[vidx].b16());
                if (use_vh)
                    cc.and_(vh[i].b16(), src_vh[i].b16(), vimm[vidx].b16());
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
            ctx->new_step();
            LOOP_IN(i) {
                if (offsets[i]) {
                    refresh_vector(ctx, &vet, i, use_vh ? 0xff : 0x0f);
                    cc.shl    (vl[i], src_vl[i], offsets[i]);
                    if (use_vh)
                        cc.shl(vh[i], src_vh[i], offsets[i]);
                }
            }
            ctx->new_step();
            LOOP_IN(i) {
                if (i != 0) {
                    refresh_vector(ctx, &vet, 0, use_vh ? 0xff : 0x0f);
                    cc.orr    (vl[0].b16(), src_vl[0].b16(), vl[i].b16());
                    if (use_vh)
                        cc.orr(vh[0].b16(), src_vh[0].b16(), vh[i].b16());
                }
            }
        }
        break;
    /* Pixel manipulation */
    case SWS_OP_CLEAR:           /* clear pixel values */
        /* Set vectors to constant value */
        if (op.type == SWS_PIXEL_U8 || op.type == SWS_PIXEL_U16 || op.type == SWS_PIXEL_U32) {
            cc.comment("clear (integer)");
            ctx->new_step();
            for (int i = 0; i < 4; i++) {
                if (op.c.q4[i].den) {
                    size_t vidx = ctx->push_imm32_op(op, av_q2i(op.c.q4[i]));
#if 1
                    if (next->op == SWS_OP_WRITE) {
                        /* TODO astmjit's register allocator sometimes fails, so we relieve some pressure */
                        new_vector(ctx, &vet, i, 0x0f);
                        cc.mov(vl[i], vet.type(vimm[vidx]));
                    } else {
                        vl[i] = vimm[vidx];
                    }
#else
                    vl[i] = vimm[vidx];
#endif
                    if (use_vh)
                        vh[i] = vimm[vidx];
                }
            }
        } else if (op.type == SWS_PIXEL_F32) {
            /* Add const data */
            size_t vpos[4];
            for (int i = 0; i < 4; i++) {
                if (op.c.q4[i].den)
                    vpos[i] = ctx->push_q(op.c.q4[i]);
            }

            /* Do the salmon dance */
            cc.comment("clear (f32)");
            ctx->new_step();
            for (int i = 0; i < 4; i++) {
                if (op.c.q4[i].den) {
                    new_vector(ctx, &vet, i);
                    cc.dup(vl[i].s4(), ctx->vdata(vpos[i]));
                    cc.dup(vh[i].s4(), ctx->vdata(vpos[i]));
                }
            }
        }
        break;
    case SWS_OP_LSHIFT:          /* logical left shift of raw pixel values by (u8) */
        if (op.type == SWS_PIXEL_U8 || op.type == SWS_PIXEL_U16 || op.type == SWS_PIXEL_U32) {
            cc.comment("lshift");
            ctx->new_step();
            LOOP_OUT(i) {
                refresh_vector(ctx, &vet, i, use_vh ? 0xff : 0x0f);
                cc.shl    (vl[i], src_vl[i], op.c.u);
                if (use_vh)
                    cc.shl(vh[i], src_vh[i], op.c.u);
            }
        } else {
            return AVERROR(ENOTSUP);
        }
        break;
    case SWS_OP_RSHIFT:          /* right shift of raw pixel values by (u8) */
        if (op.type == SWS_PIXEL_U8 || op.type == SWS_PIXEL_U16 || op.type == SWS_PIXEL_U32) {
            cc.comment("rshift");
            ctx->new_step();
            LOOP_OUT(i) {
                refresh_vector(ctx, &vet, i, use_vh ? 0xff : 0x0f);
                cc.ushr    (vl[i], src_vl[i], op.c.u);
                if (use_vh)
                    cc.ushr(vh[i], src_vh[i], op.c.u);
            }
        } else {
            return AVERROR(ENOTSUP);
        }
        break;
    case SWS_OP_SWIZZLE:         /* rearrange channel order, or duplicate channels */
        {
            bool reorder = true;
            if (next->op != SWS_OP_WRITE || next->rw.packed) {
                bool used[4] = { false, false, false, false };
                LOOP_OUT(i) {
                    if (used[op.swizzle.in[i]]) {
                        reorder = false;
                        break;
                    }
                    used[op.swizzle.in[i]] = true;
                }
            }

            LOOP_IN(i) {
                save_vector(ctx, &vet, i);
            }
            if (reorder) {
                cc.comment("swizzle (reorder)");
                LOOP_OUT(i) {
                    vl[i] = src_vl[op.swizzle.in[i]];
                    vh[i] = src_vh[op.swizzle.in[i]];
                }
            } else {
                cc.comment("swizzle (copy)");
                ctx->new_step();
                LOOP_OUT(i) {
                    if (i == op.swizzle.in[i]) {
                        vl[i] = src_vl[op.swizzle.in[i]];
                        if (use_vh)
                            vh[i] = src_vh[op.swizzle.in[i]];
                    } else {
                        new_vector(ctx, &vet, i, use_vh ? 0xff : 0x0f);
                        cc.mov    (vl[i].b16(), src_vl[op.swizzle.in[i]].b16());
                        if (use_vh)
                            cc.mov(vh[i].b16(), src_vh[op.swizzle.in[i]].b16());
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
                    ctx->new_step();
                    LOOP_OUT(i) {
                        save_vector(ctx, &vet, i, 0x0f);
                        new_vector(ctx, &vet, i, (block_size == 16) ? 0xff : 0x0f);
                        cc.zip1    (vl[i].b16(), src_vl[i].b16(), src_vl[i].b16());
                        if (block_size == 16)
                            cc.zip2(vh[i].b16(), src_vl[i].b16(), src_vl[i].b16());
                    }
                }
                if (to_size == 4) {
                    ctx->new_step();
                    LOOP_OUT(i) {
                        save_vector(ctx, &vet, i, 0x0f);
                        new_vector(ctx, &vet, i);
                        cc.zip1(vl[i].b16(), src_vl[i].b16(), src_vl[i].b16());
                        cc.zip2(vh[i].b16(), src_vl[i].b16(), src_vl[i].b16());
                    }
                }
            } else {
                snprintf(cbuf, sizeof(cbuf), "convert(%s -> %s, block_w %d)", ff_sws_pixel_type_name(from), ff_sws_pixel_type_name(to), block_size);
                cc.comment(cbuf);
                if (from == SWS_PIXEL_F32) {
                    ctx->new_step();
                    LOOP_OUT(i) {
                        refresh_vector(ctx, &vet, i);
                        cc.fcvtzu(vl[i].s4(), src_vl[i].s4());
                        cc.fcvtzu(vh[i].s4(), src_vh[i].s4());
                    }
                }
                if (block_size == 8) {
                    if        (from_size == 1 && to_size > from_size) {
                        ctx->new_step();
                        LOOP_OUT(i) {
                            refresh_vector(ctx, &vet, i, 0x0f);
                            cc.uxtl(vl[i].h8(), src_vl[i].b8());
                        }
                        from_size = 2;
                    } else if (from_size == 4 && to_size < from_size) {
                        ctx->new_step();
                        LOOP_OUT(i) {
                            refresh_vector(ctx, &vet, i);
                            cc.xtn(vl[i].h4(), src_vl[i].s4());
                            cc.xtn(vh[i].h4(), src_vh[i].s4());
                        }
                        LOOP_OUT(i) {
                            cc.ins(vl[i].d(1), vh[i].d(0));
                        }
                        from_size = 2;
                    }
                    if        (from_size == 2 && to_size == 4) {
                        ctx->new_step();
                        LOOP_OUT(i) {
                            save_vector(ctx, &vet, i, 0x0f);
                            new_vector(ctx, &vet, i);
                            cc.uxtl (vl[i].s4(), src_vl[i].h4());
                            cc.uxtl2(vh[i].s4(), src_vl[i].h8());
                        }
                        from_size = 4;
                    } else if (from_size == 2 && to_size == 1) {
                        ctx->new_step();
                        LOOP_OUT(i) {
                            refresh_vector(ctx, &vet, i, 0x0f);
                            cc.xtn(vl[i].b8(), src_vl[i].h8());
                        }
                        from_size = 1;
                    }
                } else /* if (block_size == 16) */ {
                    if        (from_size == 1 && to_size == 2) {
                        ctx->new_step();
                        LOOP_OUT(i) {
                            save_vector(ctx, &vet, i, 0x0f);
                            new_vector(ctx, &vet, i);
                            cc.uxtl (vl[i].h8(), src_vl[i].b8());
                            cc.uxtl2(vh[i].h8(), src_vl[i].b16());
                        }
                    } else if (from_size == 2 && to_size == 1) {
                        ctx->new_step();
                        LOOP_OUT(i) {
                            refresh_vector(ctx, &vet, i);
                            cc.xtn(vl[i].b8(), src_vl[i].h8());
                            cc.xtn(vh[i].b8(), src_vh[i].h8());
                        }
                        LOOP_OUT(i) {
                            cc.ins(vl[i].d(1), vh[i].d(0));
                        }
                    }
                }
                if (to == SWS_PIXEL_F32) {
                    ctx->new_step();
                    LOOP_OUT(i) {
                        refresh_vector(ctx, &vet, i);
                        cc.ucvtf(vl[i].s4(), src_vl[i].s4());
                        cc.ucvtf(vh[i].s4(), src_vh[i].s4());
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
            ctx->new_step();
            LOOP_OUT(i) {
                refresh_vector(ctx, &vet, i);
                cc.fadd(vl[i].s4(), src_vl[i].s4(), vimm[vidx].s4());
                cc.fadd(vh[i].s4(), src_vh[i].s4(), vimm[vidx].s4());
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
            ctx->new_step();
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

                refresh_vector(ctx, &vet, i);
                cc.fadd(vl[i].s4(), src_vl[i].s4(), dither_vl.s4());
                cc.fadd(vh[i].s4(), src_vh[i].s4(), dither_vh.s4());
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
                save_vector(ctx, &vet, i);
            }
            ctx->new_step();
            LOOP_ARRAY(i, used) {
                new_vector(ctx, &vet, i);
                for (int j = 0; j < 5; j++) {
                    int sj = fdata_swizzle[j];
                    int vidx = vpos[i][sj];
                    if (vidx != -1) {
                        if (j == 0) {
                            /* offset */
                            cc.dup(vl[i].s4(), ctx->vdata(vidx));
                            cc.dup(vh[i].s4(), ctx->vdata(vidx));
                        } else {
                            cc.fmul(vl[i].s4(), src_vl[sj].s4(), ctx->vdata(vidx));
                            cc.fmul(vh[i].s4(), src_vh[sj].s4(), ctx->vdata(vidx));
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
                            cc.fmla(vl[i].s4(), src_vl[sj].s4(), ctx->vdata(vidx));
                            cc.fmla(vh[i].s4(), src_vh[sj].s4(), ctx->vdata(vidx));
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
                        cc.fmla(vl[i].s4(), src_vl[sj].s4(), ctx->vdata(vidx));
                    }
                }
                count = 0;
                for (int j = 0; j < 5; j++) {
                    int sj = fdata_swizzle[j];
                    int vidx = vpos[i][sj];
                    if (vidx != -1 && count++ != 0) {
                        cc.fmla(vh[i].s4(), src_vh[sj].s4(), ctx->vdata(vidx));
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
            ctx->new_step();
            LOOP_OUT(i) {
                refresh_vector(ctx, &vet, i);
                cc.fmul(vl[i].s4(), src_vl[i].s4(), ctx->vdata(vidx));
                cc.fmul(vh[i].s4(), src_vh[i].s4(), ctx->vdata(vidx));
            }
        } else if (op.type == SWS_PIXEL_U8 || op.type == SWS_PIXEL_U16 || op.type == SWS_PIXEL_U32) {
            /* Add immediate */
            size_t vidx = ctx->push_imm32_op(op, av_q2i(op.c.q));

            /* Do the salmon dance */
            cc.comment("scale (integer)");
            ctx->new_step();
            LOOP_OUT(i) {
                refresh_vector(ctx, &vet, i, use_vh ? 0xff : 0x0f);
                cc.mul    (vl[i], src_vl[i], vet.type(vimm[vidx]));
                if (use_vh)
                    cc.mul(vh[i], src_vh[i], vet.type(vimm[vidx]));
            }
        }
        break;
    case SWS_OP_MIN:             /* numeric minimum (q4) */
        if (op.type == SWS_PIXEL_F32) {
            cc.comment("min (f32)");
            ctx->new_step();
            LOOP_OUT(i) {
                if (op.c.q4[i].den) {
                    size_t vidx = ctx->push_immq(op.c.q4[i]);
                    refresh_vector(ctx, &vet, i, use_vh ? 0xff : 0x0f);
                    cc.fmin    (vl[i].s4(), src_vl[i].s4(), vimm[vidx].s4());
                    if (use_vh)
                        cc.fmin(vh[i].s4(), src_vh[i].s4(), vimm[vidx].s4());
                }
            }
        } else if (op.type == SWS_PIXEL_U8 || op.type == SWS_PIXEL_U16 || op.type == SWS_PIXEL_U32) {
            cc.comment("min (integer)");
            ctx->new_step();
            LOOP_OUT(i) {
                if (op.c.q4[i].den) {
                    size_t vidx = ctx->push_imm32_op(op, av_q2i(op.c.q4[i]));
                    refresh_vector(ctx, &vet, i, use_vh ? 0xff : 0x0f);
                    cc.umin    (vl[i], src_vl[i], vet.type(vimm[vidx]));
                    if (use_vh)
                        cc.umin(vh[i], src_vh[i], vet.type(vimm[vidx]));
                }
            }
        }
        break;
    case SWS_OP_MAX:             /* numeric maximum (q4) */
        if (op.type == SWS_PIXEL_F32) {
            cc.comment("max");
            size_t vidx = ctx->push_imm32(0);
            ctx->new_step();
            LOOP_OUT(i) {
                if (op.c.q4[i].den) {
                    refresh_vector(ctx, &vet, i, use_vh ? 0xff : 0x0f);
                    cc.fmax    (vl[i].s4(), src_vl[i].s4(), vimm[vidx].s4());
                    if (use_vh)
                        cc.fmax(vh[i].s4(), src_vh[i].s4(), vimm[vidx].s4());
                }
            }
        } else {
            return AVERROR(ENOTSUP);
        }
        break;

    case SWS_OP_AARCH64_WIDEN_LSHIFT:
        {
            const SwsWidenLshiftOp *priv = (const SwsWidenLshiftOp *) &op.convert;

            use_vh = (block_size == 16);

            snprintf(cbuf, sizeof(cbuf), "widen_lshift(%s -> %s, lshift %d)", ff_sws_pixel_type_name(op.type), ff_sws_pixel_type_name(priv->convert.to), priv->lshift);
            cc.comment(cbuf);

            if (priv->lshift == 8) {
                size_t vidx = ctx->push_imm8(0);
                ctx->new_step();
                LOOP_OUT(i) {
                    save_vector(ctx, &vet, i, 0x0f);
                    new_vector(ctx, &vet, i, use_vh ? 0xff : 0x0f);
                    cc.zip1    (vl[i].b16(), vimm[vidx].b16(), src_vl[i].b16());
                    if (use_vh)
                        cc.zip2(vh[i].b16(), vimm[vidx].b16(), src_vl[i].b16());
                }
            } else /* if (priv->lshift < 8) */ {
                ctx->new_step();
                LOOP_OUT(i) {
                    save_vector(ctx, &vet, i, 0x0f);
                    new_vector(ctx, &vet, i, use_vh ? 0xff : 0x0f);
                    cc.ushll     (vl[i].h8(), src_vl[i].b8(),  priv->lshift);
                    if (use_vh)
                        cc.ushll2(vh[i].h8(), src_vl[i].b16(), priv->lshift);
                }
            }
        }
        break;

    case SWS_OP_AARCH64_SATURATING_CONVERT:
        {
            snprintf(cbuf, sizeof(cbuf), "saturating_convert(%s -> %s)", ff_sws_pixel_type_name(op.type), ff_sws_pixel_type_name(op.convert.to));
            cc.comment(cbuf);
            if (op.convert.to == SWS_PIXEL_U16) {
                ctx->new_step();
                LOOP_OUT(i) {
                    refresh_vector(ctx, &vet, i);
                    cc.uqxtn(vl[i].h4(), src_vl[i].s4());
                    cc.uqxtn(vh[i].h4(), src_vh[i].s4());
                }
                LOOP_OUT(i) {
                    cc.ins(vl[i].d(1), vh[i].d(0));
                }
            } else /* if (op.convert.to == SWS_PIXEL_U8) */ {
                ctx->new_step();
                LOOP_OUT(i) {
                    refresh_vector(ctx, &vet, i, 0x0f);
                    cc.uqxtn(vl[i].b8(), src_vl[i].h8());
                }
            }
        }
        break;

    case SWS_OP_AARCH64_SHUFFLE_BYTES:
        {
            /* Read */
            const SwsShuffleOp *priv = (SwsShuffleOp *) &op.rw;
            ctx->m_read_bytes = priv->read_bytes / block_size;
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
            cc.comment("read_bytes");
            for (int i = 0; i < priv->read_bytes; i += 16) {
                int j = (i >> 4);
                snprintf(cbuf, sizeof(cbuf), "vin%d", j);
                vl[j] = cc.newVecQ(cbuf);
            }
            switch (priv->read_bytes) {
            case 16: cc.ld1(vl[0].b16(),                                        a64::ptr(ctx->m_in[0]).post(16)); break;
            case 32: cc.ld1(vl[0].b16(), vl[1].b16(),                           a64::ptr(ctx->m_in[0]).post(32)); break;
            case 48: cc.ld1(vl[0].b16(), vl[1].b16(), vl[2].b16(),              a64::ptr(ctx->m_in[0]).post(48)); break;
            case 64: cc.ld1(vl[0].b16(), vl[1].b16(), vl[2].b16(), vl[3].b16(), a64::ptr(ctx->m_in[0]).post(64)); break;
            }

            /* Shuffle */
            const uint8_t *shuffle = priv->data;
            int shuffle_size = priv->size;
            int vector_size = 16;

            uint8_t tbl_data[256];
            int     tbl_data_size = 0;
            uint8_t tbl_insn[16];
            int     tbl_insn_count = 0;
            uint8_t orr_insn[16];
            int     orr_insn_count = 0;
            int     vtmp_count = 0;
            for (int i = 0; i < shuffle_size; i += vector_size) {
                /* Calculate input vectors mask */
                int vin_mask = 0;
                for (int j = 0; j < vector_size; j++) {
                    int val = shuffle[i + j];
                    if (val != 0x80) {
                        int vin = (val >> 4);
                        vin_mask |= (1 << vin);
                    }
                }
                /* Populate tbl_data, tbl_insn, and orr_insn */
                int tbl_count = 0;
                int vout = (i >> 4);
                for (int vin = 0; vin < 4; vin++) {
                    if (vin_mask & (1 << vin)) {
                        for (int j = 0; j < vector_size; j++) {
                            int val = shuffle[i + j];
                            if ((val >> 4) == vin) {
                                val &= 0x0f;
                            } else {
                                val = 0x80;
                            }
                            tbl_data[tbl_data_size++] = val;
                        }
                        if (tbl_count++ == 0) {
                            tbl_insn[tbl_insn_count++] = (vout << 4) | vin;
                        } else {
                            size_t vtmp = vtmp_count++;
                            tbl_insn[tbl_insn_count++] = 0x80 | (vtmp << 4) | vin;
                            orr_insn[orr_insn_count++] = (vout << 4) | vtmp;
                        }
                    }
                }
            }
            /* Create output vectors */
            int vout_count = shuffle_size / vector_size;
            for (int i = 0; i < vout_count; i++) {
                snprintf(cbuf, sizeof(cbuf), "vout%d", i);
                vh[i] = cc.newVecQ(cbuf);
            }
            /* Create tbl data vectors */
            std::vector<a64::Vec> vshuffle;
            for (int i = 0; i < tbl_insn_count; i++) {
                snprintf(cbuf, sizeof(cbuf), "vshuffle%d", i);
                a64::Vec vreg = cc.newVecQ(cbuf);
                vshuffle.push_back(vreg);
            }
            /* Create temporary vectors */
            std::vector<a64::Vec> vtmp;
            for (int i = 0; i < vtmp_count; i++) {
                snprintf(cbuf, sizeof(cbuf), "vtmp%d", i);
                a64::Vec vreg = cc.newVecQ(cbuf);
                vtmp.push_back(vreg);
            }
            /* Write tbl data after function */
            Label ldata = ctx->emit_data(tbl_data, tbl_data_size, "tbl_data_array");
            /* Read tbl data into vectors (prologue) */
            ctx->to_prologue();
            a64::Gp ptr = cc.newGpz("tbl_data_ptr");
            cc.comment("prologue (shuffle)");
            cc.adr(ptr, ldata);
            if (tbl_insn_count > 4) {
                cc.ld1(vshuffle[0].b16(), vshuffle[1].b16(), vshuffle[2].b16(), vshuffle[3].b16(), a64::ptr(ptr).post(64));
            } else {
                switch (tbl_insn_count) {
                case 1: cc.ld1(vshuffle[0].b16(),                                                          a64::ptr(ptr)); break;
                case 2: cc.ld1(vshuffle[0].b16(), vshuffle[1].b16(),                                       a64::ptr(ptr)); break;
                case 3: cc.ld1(vshuffle[0].b16(), vshuffle[1].b16(), vshuffle[2].b16(),                    a64::ptr(ptr)); break;
                case 4: cc.ld1(vshuffle[0].b16(), vshuffle[1].b16(), vshuffle[2].b16(), vshuffle[3].b16(), a64::ptr(ptr)); break;
                }
            }
            if (tbl_insn_count > 8) {
                cc.ld1(vshuffle[4].b16(), vshuffle[5].b16(), vshuffle[6].b16(), vshuffle[7].b16(), a64::ptr(ptr).post(64));
            } else {
                switch (tbl_insn_count) {
                case 5: cc.ld1(vshuffle[4].b16(),                                                          a64::ptr(ptr)); break;
                case 6: cc.ld1(vshuffle[4].b16(), vshuffle[5].b16(),                                       a64::ptr(ptr)); break;
                case 7: cc.ld1(vshuffle[4].b16(), vshuffle[5].b16(), vshuffle[6].b16(),                    a64::ptr(ptr)); break;
                case 8: cc.ld1(vshuffle[4].b16(), vshuffle[5].b16(), vshuffle[6].b16(), vshuffle[7].b16(), a64::ptr(ptr)); break;
                }
            }
            switch (tbl_insn_count) {
            case  9: cc.ld1(vshuffle[8].b16(),                                                            a64::ptr(ptr)); break;
            case 10: cc.ld1(vshuffle[8].b16(), vshuffle[9].b16(),                                         a64::ptr(ptr)); break;
            case 11: cc.ld1(vshuffle[8].b16(), vshuffle[9].b16(), vshuffle[10].b16(),                     a64::ptr(ptr)); break;
            case 12: cc.ld1(vshuffle[8].b16(), vshuffle[9].b16(), vshuffle[10].b16(), vshuffle[11].b16(), a64::ptr(ptr)); break;
            }
            ctx->from_prologue();
            /* Emit tbl instructions */
            cc.comment("shuffle");
            for (int i = 0; i < tbl_insn_count; i++) {
                int vsrc = (tbl_insn[i] & 0x07);
                int vdst = tbl_insn[i] >> 4;
                if (vdst & 0x08) {
                    vdst &= 7;
                    cc.tbl(vtmp[vdst].b16(), vl[vsrc].b16(), vshuffle[i].b16());
                } else {
                    cc.tbl(vh  [vdst].b16(), vl[vsrc].b16(), vshuffle[i].b16());
                }
            }
            /* Emit orr instructions */
            for (int i = 0; i < orr_insn_count; i++) {
                int vsrc = (orr_insn[i] & 0x07);
                int vdst = orr_insn[i] >> 4;
                cc.orr(vh[vdst].b16(), vh[vdst].b16(), vtmp[vsrc].b16());
            }

            /* Write */
            ctx->m_write_bytes = priv->write_bytes / block_size;
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
            cc.comment("write_bytes");
#if 1
            for (int i = 0; i < priv->write_bytes; i += 16) {
                int j = (i >> 4);
                cc.virtRegByReg(vh[j])->setHomeIdHint(REGID_VSTX + j);
            }
#endif
            switch (priv->write_bytes) {
            case 16: cc.st1(vh[0].b16(),                                        a64::ptr(ctx->m_out[0]).post(16)); break;
            case 32: cc.st1(vh[0].b16(), vh[1].b16(),                           a64::ptr(ctx->m_out[0]).post(32)); break;
            case 48: cc.st1(vh[0].b16(), vh[1].b16(), vh[2].b16(),              a64::ptr(ctx->m_out[0]).post(48)); break;
            case 64: cc.st1(vh[0].b16(), vh[1].b16(), vh[2].b16(), vh[3].b16(), a64::ptr(ctx->m_out[0]).post(64)); break;
            case 96: cc.st1(vh[0].b16(), vh[1].b16(), vh[2].b16(), vh[3].b16(), a64::ptr(ctx->m_out[0]).post(64));
                     cc.st1(vh[4].b16(), vh[5].b16(),                           a64::ptr(ctx->m_out[0]).post(32)); break;
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

    block_size = asmjit_optimize(ops, block_size);

    AsmJitContext *ctx = new AsmJitContext(ops, block_size);
    a64::Compiler &cc = *ctx->m_cc;
    Error err;

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
