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
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "../jit.h"
}

using namespace llvm;

/**
 * Assemble AArch64 GAS-syntax text into a read+exec memory buffer.
 *
 * On success, sets *out_text to executable memory of *out_size bytes
 * that must be released with ff_sws_jit_free(*out_text, *out_size) and returns 0.
 * Returns a negative AVERROR code on failure.
 */
extern "C"
int ff_sws_jit_assemble_llvm(const char *asm_src, void **out_text, size_t *out_size)
{
    static const char triple[] = "aarch64-unknown-linux-gnu";

    LLVMInitializeAArch64TargetInfo();
    LLVMInitializeAArch64Target();
    LLVMInitializeAArch64TargetMC();
    LLVMInitializeAArch64AsmParser();

    std::string err;
    const Target *target = TargetRegistry::lookupTarget(triple, err);
    if (!target) {
        av_log(NULL, AV_LOG_ERROR, "LLVM JIT: failed to find target %s: %s\n",
               triple, err.c_str());
        return AVERROR_EXTERNAL;
    }

    MCTargetOptions MCOpts;

    std::unique_ptr<MCRegisterInfo> MRI(target->createMCRegInfo(triple));
    if (!MRI) {
        av_log(NULL, AV_LOG_ERROR, "LLVM JIT: createMCRegInfo() failed\n");
        return AVERROR_EXTERNAL;
    }

    std::unique_ptr<MCAsmInfo> MAI(target->createMCAsmInfo(*MRI, triple, MCOpts));
    if (!MAI) {
        av_log(NULL, AV_LOG_ERROR, "LLVM JIT: createMCAsmInfo() failed\n");
        return AVERROR_EXTERNAL;
    }

    std::unique_ptr<MCInstrInfo> MCII(target->createMCInstrInfo());
    if (!MCII) {
        av_log(NULL, AV_LOG_ERROR, "LLVM JIT: createMCInstrInfo() failed\n");
        return AVERROR_EXTERNAL;
    }

    std::unique_ptr<MCSubtargetInfo> STI(target->createMCSubtargetInfo(triple, "", ""));
    if (!STI) {
        av_log(NULL, AV_LOG_ERROR, "LLVM JIT: createMCSubtargetInfo() failed\n");
        return AVERROR_EXTERNAL;
    }

    SmallVector<char, 4096> ObjBuf;
    raw_svector_ostream OS(ObjBuf);

    MCContext ctx(Triple(triple), MAI.get(), MRI.get(), STI.get());

    std::unique_ptr<MCObjectFileInfo> MOFI(target->createMCObjectFileInfo(ctx, false));
    if (!MOFI) {
        av_log(NULL, AV_LOG_ERROR, "LLVM JIT: createMCObjectFileInfo() failed\n");
        return AVERROR_EXTERNAL;
    }
    ctx.setObjectFileInfo(MOFI.get());

    std::unique_ptr<MCCodeEmitter> CE(target->createMCCodeEmitter(*MCII, ctx));
    if (!CE) {
        av_log(NULL, AV_LOG_ERROR, "LLVM JIT: createMCCodeEmitter() failed\n");
        return AVERROR_EXTERNAL;
    }

    std::unique_ptr<MCAsmBackend> MAB(target->createMCAsmBackend(*STI, *MRI, MCOpts));
    if (!MAB) {
        av_log(NULL, AV_LOG_ERROR, "LLVM JIT: createMCAsmBackend() failed\n");
        return AVERROR_EXTERNAL;
    }

    std::unique_ptr<MCObjectWriter> OW(MAB->createObjectWriter(OS));
    if (!OW) {
        av_log(NULL, AV_LOG_ERROR, "LLVM JIT: createObjectWriter() failed\n");
        return AVERROR_EXTERNAL;
    }

    std::unique_ptr<MCStreamer> streamer(target->createMCObjectStreamer(Triple(triple), ctx, std::move(MAB), std::move(OW), std::move(CE), *STI));
    if (!streamer) {
        av_log(NULL, AV_LOG_ERROR, "LLVM JIT: createMCObjectStreamer() failed\n");
        return AVERROR_EXTERNAL;
    }

    SourceMgr SrcMgr;
    SrcMgr.AddNewSourceBuffer(MemoryBuffer::getMemBuffer(asm_src, "<asm>"), SMLoc());

    std::unique_ptr<MCAsmParser> Parser(createMCAsmParser(SrcMgr, ctx, *streamer, *MAI));
    if (!Parser) {
        av_log(NULL, AV_LOG_ERROR, "LLVM JIT: createMCAsmParser() failed\n");
        return AVERROR_EXTERNAL;
    }

    MCTargetAsmParser *TAP = target->createMCAsmParser(*STI, *Parser, *MCII, MCOpts);
    if (!TAP) {
        av_log(NULL, AV_LOG_ERROR, "LLVM JIT: target createMCAsmParser() failed\n");
        return AVERROR_EXTERNAL;
    }
    Parser->setTargetParser(*TAP);

    if (Parser->Run(false)) {
        av_log(NULL, AV_LOG_ERROR, "LLVM JIT: assembly failed\n");
        return AVERROR(EINVAL);
    }

    MemoryBufferRef ObjMBR(StringRef(ObjBuf.data(), ObjBuf.size()), "obj");
    Expected<std::unique_ptr<object::ObjectFile>> ObjOrErr = object::ObjectFile::createObjectFile(ObjMBR);
    if (!ObjOrErr) {
        consumeError(ObjOrErr.takeError());
        av_log(NULL, AV_LOG_ERROR, "LLVM JIT: failed to parse assembled object\n");
        return AVERROR_INVALIDDATA;
    }

    for (const object::SectionRef &S: (*ObjOrErr)->sections()) {
        Expected<StringRef> name = S.getName();
        if (!name) {
            consumeError(Name.takeError());
            continue;
        }
        if (*name != ".text")
            continue;

        Expected<StringRef> Contents = S.getContents();
        if (!Contents) {
            consumeError(Contents.takeError());
            av_log(NULL, AV_LOG_ERROR, "LLVM JIT: failed to read .text section\n");
            return AVERROR_INVALIDDATA;
        }
        if (Contents->empty()) {
            av_log(NULL, AV_LOG_ERROR, "LLVM JIT: .text section is empty\n");
            return AVERROR_INVALIDDATA;
        }

        int ret = ff_sws_jit_make_exec(Contents->data(), Contents->size(), out_text);
        if (ret < 0)
            return ret;

        *out_size = Contents->size();

        return 0;
    }

    av_log(NULL, AV_LOG_ERROR, "LLVM JIT: no .text section in assembled output\n");

    return AVERROR_INVALIDDATA;
}
