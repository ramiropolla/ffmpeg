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

/* Prevent collision with LLVM's usage of PIC as a parameter name. */
#undef PIC

#include <llvm/MC/MCAsmBackend.h>
#include <llvm/MC/MCAsmInfo.h>
#include <llvm/MC/MCCodeEmitter.h>
#include <llvm/MC/MCContext.h>
#include <llvm/MC/MCInstrInfo.h>
#include <llvm/MC/MCObjectFileInfo.h>
#include <llvm/MC/MCObjectWriter.h>
#include <llvm/MC/MCParser/MCAsmParser.h>
#include <llvm/MC/MCParser/MCTargetAsmParser.h>
#include <llvm/MC/MCRegisterInfo.h>
#include <llvm/MC/MCStreamer.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/MC/MCTargetOptions.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Triple.h>

extern "C" {
#include "libavutil/bprint.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "../jit.h"
#include "ops_jit_llvm.h"
}

using namespace llvm;

/*********************************************************************/
static void asm_diag_handler(const SMDiagnostic &diag, void *context)
{
    AVBPrint *errstr = (AVBPrint *) context;
    int n_line = diag.getLineNo();
    int n_col = diag.getColumnNo();
    const char *kind = "unknown";
    std::string message = std::string(diag.getMessage());
    std::string line = std::string(diag.getLineContents());

    switch (diag.getKind()) {
    case SourceMgr::DK_Error:   kind = "error";   break;
    case SourceMgr::DK_Warning: kind = "warning"; break;
    case SourceMgr::DK_Remark:  kind = "remark";  break;
    case SourceMgr::DK_Note:    kind = "note";    break;
    }

    av_bprintf(errstr, "%d:%d: %s: %s\n", n_line, n_col + 1, kind, message.c_str());
    if (!line.empty()) {
        av_bprintf(errstr, "%s\n", line.c_str());
        av_bprintf(errstr, "%*s^\n", n_col, " ");
    }
}

/*********************************************************************/
extern "C"
int ff_sws_jit_assemble_llvm(void *logctx, const char *src,
                             uint8_t **out_text, size_t *out_size,
                             AVBPrint *errstr)
{
    static const char triple_name[] = "aarch64-unknown-linux-gnu";

    LLVMInitializeAArch64TargetInfo();
    LLVMInitializeAArch64Target();
    LLVMInitializeAArch64TargetMC();
    LLVMInitializeAArch64AsmParser();

    std::string err;
    const Target *target = TargetRegistry::lookupTarget(triple_name, err);
    if (!target) {
        av_log(logctx, AV_LOG_WARNING, "LLVM JIT: failed to find target %s: %s\n",
               triple_name, err.c_str());
        return AVERROR_EXTERNAL;
    }

    std::unique_ptr<MCRegisterInfo> mri(target->createMCRegInfo(triple_name));
    if (!mri) {
        av_log(logctx, AV_LOG_WARNING, "LLVM JIT: createMCRegInfo() failed\n");
        return AVERROR_EXTERNAL;
    }

    MCTargetOptions options;
    std::unique_ptr<MCAsmInfo> mai(target->createMCAsmInfo(*mri, triple_name, options));
    if (!mai) {
        av_log(logctx, AV_LOG_WARNING, "LLVM JIT: createMCAsmInfo() failed\n");
        return AVERROR_EXTERNAL;
    }

    std::unique_ptr<MCInstrInfo> mcii(target->createMCInstrInfo());
    if (!mcii) {
        av_log(logctx, AV_LOG_WARNING, "LLVM JIT: createMCInstrInfo() failed\n");
        return AVERROR_EXTERNAL;
    }

    std::unique_ptr<MCSubtargetInfo> sti(target->createMCSubtargetInfo(triple_name, "", ""));
    if (!sti) {
        av_log(logctx, AV_LOG_WARNING, "LLVM JIT: createMCSubtargetInfo() failed\n");
        return AVERROR_EXTERNAL;
    }

    MCContext ctx(Triple(triple_name), mai.get(), mri.get(), sti.get());

    std::unique_ptr<MCObjectFileInfo> mofi(target->createMCObjectFileInfo(ctx, false));
    if (!mofi) {
        av_log(logctx, AV_LOG_WARNING, "LLVM JIT: createMCObjectFileInfo() failed\n");
        return AVERROR_EXTERNAL;
    }
    ctx.setObjectFileInfo(mofi.get());

    std::unique_ptr<MCCodeEmitter> ce(target->createMCCodeEmitter(*mcii, ctx));
    if (!ce) {
        av_log(logctx, AV_LOG_WARNING, "LLVM JIT: createMCCodeEmitter() failed\n");
        return AVERROR_EXTERNAL;
    }

    std::unique_ptr<MCAsmBackend> mab(target->createMCAsmBackend(*sti, *mri, options));
    if (!mab) {
        av_log(logctx, AV_LOG_WARNING, "LLVM JIT: createMCAsmBackend() failed\n");
        return AVERROR_EXTERNAL;
    }

    SmallVector<char, 4096> obj_buf;
    raw_svector_ostream ostream(obj_buf);
    std::unique_ptr<MCObjectWriter> ow(mab->createObjectWriter(ostream));
    if (!ow) {
        av_log(logctx, AV_LOG_WARNING, "LLVM JIT: createObjectWriter() failed\n");
        return AVERROR_EXTERNAL;
    }

    std::unique_ptr<MCStreamer> streamer(target->createMCObjectStreamer(Triple(triple_name), ctx, std::move(mab), std::move(ow), std::move(ce), *sti));
    if (!streamer) {
        av_log(logctx, AV_LOG_WARNING, "LLVM JIT: createMCObjectStreamer() failed\n");
        return AVERROR_EXTERNAL;
    }

    SourceMgr src_mgr;
    src_mgr.setDiagHandler(asm_diag_handler, errstr);
    src_mgr.AddNewSourceBuffer(MemoryBuffer::getMemBuffer(src, "<asm>"), SMLoc());

    std::unique_ptr<MCAsmParser> parser(createMCAsmParser(src_mgr, ctx, *streamer, *mai));
    if (!parser) {
        av_log(logctx, AV_LOG_WARNING, "LLVM JIT: createMCAsmParser() failed\n");
        return AVERROR_EXTERNAL;
    }

    MCTargetAsmParser *tap = target->createMCAsmParser(*sti, *parser, *mcii, options);
    if (!tap) {
        av_log(logctx, AV_LOG_WARNING, "LLVM JIT: target createMCAsmParser() failed\n");
        return AVERROR_EXTERNAL;
    }
    parser->setTargetParser(*tap);

    if (parser->Run(false)) {
        av_log(logctx, AV_LOG_WARNING, "LLVM JIT: assembly failed\n");
        return AVERROR(EINVAL);
    }

    MemoryBufferRef obj_membuf(StringRef(obj_buf.data(), obj_buf.size()), "obj");
    Expected<std::unique_ptr<object::ObjectFile>> obj = object::ObjectFile::createObjectFile(obj_membuf);
    if (!obj) {
        consumeError(obj.takeError());
        av_log(logctx, AV_LOG_WARNING, "LLVM JIT: failed to parse assembled object\n");
        return AVERROR_INVALIDDATA;
    }

    for (const object::SectionRef &section: (*obj)->sections()) {
        Expected<StringRef> name = section.getName();
        if (!name) {
            consumeError(name.takeError());
            continue;
        }
        if (*name != ".text")
            continue;

        Expected<StringRef> contents = section.getContents();
        if (!contents) {
            consumeError(contents.takeError());
            av_log(logctx, AV_LOG_WARNING, "LLVM JIT: failed to read .text section\n");
            return AVERROR_INVALIDDATA;
        }
        if (contents->empty()) {
            av_log(logctx, AV_LOG_WARNING, "LLVM JIT: .text section is empty\n");
            return AVERROR_INVALIDDATA;
        }

        *out_size = contents->size();
        *out_text = (uint8_t *) ff_sws_jit_alloc(*out_size);
        if (!*out_text)
            return AVERROR(ENOMEM);

        memcpy(*out_text, contents->data(), *out_size);
        ff_sws_jit_protect(*out_text, *out_size);

        return 0;
    }

    av_log(logctx, AV_LOG_WARNING, "LLVM JIT: no .text section in assembled output\n");

    return AVERROR_INVALIDDATA;
}
