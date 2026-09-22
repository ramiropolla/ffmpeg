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

#include <string.h>

#include <llvm-c/Core.h>
#include <llvm-c/Object.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

#include "libavutil/bprint.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/time.h"
#include "../jit.h"
#include "ops_jit_llvm.h"

/*********************************************************************/
typedef struct DiagnosticHandlerContext {
    AVBPrint *errstr;
    int error;
} DiagnosticHandlerContext;

static void asm_diag_handler(struct LLVMOpaqueDiagnosticInfo *info, void *opaque)
{
    DiagnosticHandlerContext *dh_ctx = (DiagnosticHandlerContext *) opaque;

    if (LLVMGetDiagInfoSeverity(info) == LLVMDSError)
        dh_ctx->error = 1;

    char *desc = LLVMGetDiagInfoDescription(info);
    av_bprintf(dh_ctx->errstr, "%s\n", desc);
    LLVMDisposeMessage(desc);
}

/*********************************************************************/
int ff_sws_jit_assemble_llvm(void *logctx, const char *src,
                             uint8_t **out_text, size_t *out_size,
                             AVBPrint *errstr)
{
    static const char triple_name[] = "aarch64-unknown-linux-gnu";

    struct LLVMOpaqueTargetMachine *machine = NULL;
    struct LLVMOpaqueContext *llvm_ctx = NULL;
    struct LLVMOpaqueModule *module = NULL;
    struct LLVMOpaqueMemoryBuffer *mem_buf = NULL;
    struct LLVMOpaqueBinary *binary = NULL;
    struct LLVMOpaqueSectionIterator *iter = NULL;
    char *err = NULL;
    int ret;

    int64_t starttime = av_gettime_relative();

    LLVMInitializeAArch64TargetInfo();
    LLVMInitializeAArch64Target();
    LLVMInitializeAArch64TargetMC();
    LLVMInitializeAArch64AsmPrinter();
    LLVMInitializeAArch64AsmParser();

    struct LLVMTarget *target;
    if (LLVMGetTargetFromTriple(triple_name, &target, &err)) {
        av_log(logctx, AV_LOG_WARNING, "LLVM JIT: failed to find target %s: %s\n",
               triple_name, err ? err : "unknown error");
        LLVMDisposeMessage(err);
        return AVERROR_EXTERNAL;
    }

    machine = LLVMCreateTargetMachine(target, triple_name, "", "",
                                      LLVMCodeGenLevelDefault, LLVMRelocDefault,
                                      LLVMCodeModelDefault);
    if (!machine) {
        av_log(logctx, AV_LOG_WARNING, "LLVM JIT: LLVMCreateTargetMachine() failed\n");
        return AVERROR_EXTERNAL;
    }

    llvm_ctx = LLVMContextCreate();

    DiagnosticHandlerContext dh_ctx = { .errstr = errstr };
    LLVMContextSetDiagnosticHandler(llvm_ctx, asm_diag_handler, &dh_ctx);

    module = LLVMModuleCreateWithNameInContext("sws_jit", llvm_ctx);
    LLVMSetTarget(module, triple_name);
    LLVMSetModuleInlineAsm2(module, src, strlen(src));

    if (LLVMTargetMachineEmitToMemoryBuffer(machine, module, LLVMObjectFile,
                                            &err, &mem_buf) || dh_ctx.error) {
        if (err)
            av_log(logctx, AV_LOG_WARNING, "LLVM JIT: assembly failed: %s\n", err);
        else
            av_log(logctx, AV_LOG_WARNING, "LLVM JIT: assembly failed\n");
        LLVMDisposeMessage(err);
        ret = AVERROR(EINVAL);
        goto end;
    }

    binary = LLVMCreateBinary(mem_buf, NULL, &err);
    if (!binary) {
        av_log(logctx, AV_LOG_WARNING, "LLVM JIT: failed to parse assembled object: %s\n",
               err ? err : "unknown error");
        LLVMDisposeMessage(err);
        ret = AVERROR_INVALIDDATA;
        goto end;
    }

    for (iter = LLVMObjectFileCopySectionIterator(binary);
         iter && !LLVMObjectFileIsSectionIteratorAtEnd(binary, iter);
         LLVMMoveToNextSection(iter)) {
        const char *name = LLVMGetSectionName(iter);
        if (!name || strcmp(name, ".text"))
            continue;

        uint64_t size = LLVMGetSectionSize(iter);
        if (!size) {
            av_log(logctx, AV_LOG_WARNING, "LLVM JIT: .text section is empty\n");
            ret = AVERROR_INVALIDDATA;
            goto end;
        }

        av_log(logctx, AV_LOG_VERBOSE, "LLVM JIT: assembler took %" PRId64 " us\n",
               av_gettime_relative() - starttime);

        *out_size = size;
        *out_text = (uint8_t *) ff_sws_jit_alloc(*out_size);
        if (!*out_text) {
            ret = AVERROR(ENOMEM);
            goto end;
        }

        const char *data = LLVMGetSectionContents(iter);
        memcpy(*out_text, data, *out_size);
        ret = ff_sws_jit_protect(*out_text, *out_size);
        goto end;
    }

    av_log(logctx, AV_LOG_WARNING, "LLVM JIT: no .text section in assembled output\n");
    ret = AVERROR_INVALIDDATA;

end:
    if (iter)
        LLVMDisposeSectionIterator(iter);
    if (binary)
        LLVMDisposeBinary(binary);
    if (mem_buf)
        LLVMDisposeMemoryBuffer(mem_buf);
    if (module)
        LLVMDisposeModule(module);
    if (llvm_ctx)
        LLVMContextDispose(llvm_ctx);
    if (machine)
        LLVMDisposeTargetMachine(machine);

    return ret;
}
