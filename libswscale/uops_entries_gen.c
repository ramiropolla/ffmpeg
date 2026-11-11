/**
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

/**
 * Generate the list of kernels each backend must implement, by running the
 * backend's translation policy over every op list swscale can produce. See
 * uops_entries.h for the format of the generated lists.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

#include "libavutil/bprint.h"
#include "libavutil/error.h"
#include "libavutil/macros.h"
#include "libavutil/mem.h"
#include "libavutil/pixfmt.h"
#include "libavutil/thread.h"
#include "libavutil/tree.h"
#include "ops.h"
#include "ops_dispatch.h"
#include "op_list_gen_template.c"
#include "swscale.h"
#include "uops.h"
#include "uops_backend.h"
#include "uops_list.h"
#include "aarch64/uops_aarch64.h"
#include "x86/uops_x86.h"

static const char uop_names[SWS_UOP_TYPE_NB][32] = {
#define UOP_NAME(OP, ABBR) [OP] = #OP,
    UOPS_LIST(UOP_NAME)
};

static const char pixel_types[SWS_PIXEL_TYPE_NB][16] = {
    [SWS_PIXEL_NONE] = "SWS_PIXEL_NONE",
    [SWS_PIXEL_U8]   = "SWS_PIXEL_U8",
    [SWS_PIXEL_U16]  = "SWS_PIXEL_U16",
    [SWS_PIXEL_U32]  = "SWS_PIXEL_U32",
    [SWS_PIXEL_F32]  = "SWS_PIXEL_F32",
};

/* Bare suffixes of the enum names, for token pasting by the consumers */
#define UOP_SUFFIX(uop)   (uop_names[uop] + sizeof("SWS_UOP_") - 1)
#define TYPE_SUFFIX(type) (pixel_types[type] + sizeof("SWS_PIXEL_") - 1)

/* Print uop params either as C struct designators, or as plain values */
static void print_params(AVBPrint *bp, const SwsUOp *uop, bool designated)
{
#define FMT(FIELD, VALUE, ...)     av_bprintf(bp, designated ? ", ." FIELD " = " VALUE      : ", " VALUE, __VA_ARGS__)
#define FMT_SUB(FIELD, VALUE, ...) av_bprintf(bp, designated ? ", ." FIELD " = {" VALUE "}" : ", " VALUE, __VA_ARGS__)

    const SwsUOpParams *par = &uop->par;
    switch (uop->uop) {
    case SWS_UOP_READ_PLANAR_FH:
    case SWS_UOP_READ_PLANAR_FV:
    case SWS_UOP_READ_PLANAR_FV_FMA:
        FMT("par.filter.type", "%s", pixel_types[par->filter.type]);
        break;
    case SWS_UOP_RW_SHUFFLE:
        FMT("par.shuffle.clear_value", "0x%x", par->shuffle.clear_value);
        FMT("par.shuffle.read_size",   "%u",   par->shuffle.read_size);
        FMT("par.shuffle.write_size",  "%u",   par->shuffle.write_size);
        break;
    case SWS_UOP_LSHIFT:
    case SWS_UOP_RSHIFT:
        FMT("par.shift.amount", "%u", par->shift.amount);
        break;
    case SWS_UOP_PERMUTE:
    case SWS_UOP_COPY:
        FMT("par.move.num_moves", "%d", par->move.num_moves);
        FMT_SUB("par.move.dst", "%d, %d, %d, %d, %d, %d",
                par->move.dst[0], par->move.dst[1], par->move.dst[2],
                par->move.dst[3], par->move.dst[4], par->move.dst[5]);
        FMT_SUB("par.move.src", "%d, %d, %d, %d, %d, %d",
                par->move.src[0], par->move.src[1], par->move.src[2],
                par->move.src[3], par->move.src[4], par->move.src[5]);
        break;
    case SWS_UOP_PACK:
    case SWS_UOP_UNPACK:
        FMT_SUB("par.pack.pattern", "%d, %d, %d, %d",
                par->pack.pattern[0], par->pack.pattern[1],
                par->pack.pattern[2], par->pack.pattern[3]);
        break;
    case SWS_UOP_CLEAR:
        FMT("par.clear.one",  "0x%05x", par->clear.one);
        FMT("par.clear.zero", "0x%05x", par->clear.zero);
        break;
    case SWS_UOP_LINEAR:
    case SWS_UOP_LINEAR_FMA:
        FMT("par.lin.one",  "0x%05x", par->lin.one);
        FMT("par.lin.zero", "0x%05x", par->lin.zero);
        if (uop->uop == SWS_UOP_LINEAR_FMA)
            FMT("par.lin.exact", "0x%05x", par->lin.exact);
        break;
    case SWS_UOP_DITHER:
        FMT_SUB("par.dither.y_offset", "%u, %u, %u, %u",
                par->dither.y_offset[0], par->dither.y_offset[1],
                par->dither.y_offset[2], par->dither.y_offset[3]);
        FMT("par.dither.size_log2", "%u", par->dither.size_log2);
        break;
    case SWS_UOP_LUT_3D:
        FMT("par.lut3d.dynamic", "%d", par->lut3d.dynamic);
        break;
    }

#undef FMT
#undef FMT_SUB
}

/* A kernel, as listed in the generated output */
typedef struct Entry {
    SwsUOp uop;
    int list; /* index into Target.lists */
    int vectors; /* x86: number of registers per component, 0 if unused */
} Entry;

/* Group entries by list and uop type, then order them like SwsUOps */
static int entry_cmp(const void *pa, const void *pb)
{
    const Entry *a = pa;
    const Entry *b = pb;
    if (a->list != b->list)
        return a->list - b->list;
    if (a->uop.uop != b->uop.uop)
        return (int) a->uop.uop - b->uop.uop;
    return ff_sws_uop_cmp(&a->uop, &b->uop);
}

struct EnumPriv;

typedef struct Target {
    const char *name;
    const char *guard; /* include guard of the generated C header */
    const char *lists[SWS_X86_ISA_NB]; /* names of the kernel lists */
    bool nasm; /* also generate NASM macros */

    /* Register the kernels needed to run `ops` */
    int (*collect)(SwsContext *ctx, const SwsOpList *ops);
    void (*name_entry)(const Entry *entry, char buf[SWS_UOP_NAME_MAX]);
} Target;

struct EnumPriv {
    const Target *target;
    struct AVTreeNode *root;
    AVMutex lock;
};

static int register_entry(SwsContext *ctx, const Entry *entry)
{
    struct EnumPriv *s = ctx->opaque;
    Entry *key = av_memdup(entry, sizeof(*entry));
    if (!key)
        return AVERROR(ENOMEM);
    memset(&key->uop.data, 0, sizeof(key->uop.data));

    struct AVTreeNode *node = av_tree_node_alloc();
    if (!node) {
        av_free(key);
        return AVERROR(ENOMEM);
    }

    ff_mutex_lock(&s->lock);
    av_tree_insert(&s->root, key, entry_cmp, &node);
    ff_mutex_unlock(&s->lock);
    if (node) {
        av_free(node);
        av_free(key);
    }
    return 0;
}

static void name_generic(const Entry *entry, char buf[SWS_UOP_NAME_MAX])
{
    ff_sws_uop_name(&entry->uop, buf);
}

/*********************************************************************/
/* C backend */

static int collect_c(SwsContext *ctx, const SwsOpList *ops)
{
    SwsUOpList *uops = ff_sws_uop_list_alloc();
    if (!uops)
        return AVERROR(ENOMEM);

    int ret = ff_sws_ops_translate(ctx, ops, ff_sws_uop_flags_c(), uops);
    for (int i = 0; ret >= 0 && i < uops->num_ops; i++) {
        Entry entry = { .uop = uops->ops[i] };
        entry.uop.block_size = SWS_BLOCK_SIZE;
        ret = register_entry(ctx, &entry);
    }

    ff_sws_uop_list_free(&uops);
    return ret;
}

/*********************************************************************/
/* x86 backend */

static int collect_x86_level(SwsContext *ctx, const SwsUOpList *uops,
                             SwsX86ISA level)
{
    if (uops->num_ops == 1 && uops->ops[0].uop == SWS_UOP_RW_SHUFFLE) {
        const Entry entry = {
            .uop  = uops->ops[0],
            .list = ff_sws_uop_shuffle_isa_x86(level, &uops->ops[0]),
        };
        return register_entry(ctx, &entry);
    }

    /* The whole chain fails if any of its uops is unsupported */
    Entry *entries = av_calloc(uops->num_ops, sizeof(*entries));
    if (!entries)
        return AVERROR(ENOMEM);

    const int block_size = ff_sws_uop_block_size_x86(level, uops);
    int ret = 0;
    for (int i = 0; i < uops->num_ops; i++) {
        Entry *entry = &entries[i];
        entry->uop = uops->ops[i];
        entry->uop.block_size = block_size;
        ret = ff_sws_uop_simplify_x86(level, &entry->uop);
        if (ret < 0)
            goto done;
        entry->list = ret;
        entry->vectors = ff_sws_uop_vectors_x86(entry->list, &entry->uop);
    }

    for (int i = 0; i < uops->num_ops; i++) {
        ret = register_entry(ctx, &entries[i]);
        if (ret < 0)
            goto done;
    }

done:
    av_free(entries);
    return ret == AVERROR(ENOTSUP) ? 0 : ret;
}

static int collect_x86(SwsContext *ctx, const SwsOpList *ops)
{
    for (int fma = 0; fma <= 1; fma++) {
        SwsUOpList *uops = ff_sws_uop_list_alloc();
        if (!uops)
            return AVERROR(ENOMEM);

        int ret = ff_sws_ops_translate(ctx, ops, ff_sws_uop_flags_x86(fma), uops);
        for (int level = 0; ret >= 0 && level < SWS_X86_ISA_NB; level++)
            ret = collect_x86_level(ctx, uops, level);
        ff_sws_uop_list_free(&uops);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static void name_x86(const Entry *entry, char buf[SWS_UOP_NAME_MAX])
{
    ff_sws_uop_name_x86(&entry->uop, entry->list, buf);
}

/*********************************************************************/
/* aarch64 backend */

static int collect_aarch64(SwsContext *ctx, const SwsOpList *ops)
{
    SwsUOpList *uops = ff_sws_uop_list_alloc();
    if (!uops)
        return AVERROR(ENOMEM);

    Entry *entries = NULL;
    const SwsUOpFlags flags = ff_sws_uop_flags_aarch64(ctx->flags & SWS_BITEXACT);
    int ret = ff_sws_ops_translate(ctx, ops, flags, uops);
    if (ret < 0)
        goto done;

    /* The whole chain fails if any of its uops is unsupported */
    entries = av_calloc(uops->num_ops, sizeof(*entries));
    if (!entries) {
        ret = AVERROR(ENOMEM);
        goto done;
    }

    for (int i = 0; i < uops->num_ops; i++) {
        entries[i].uop = uops->ops[i];
        entries[i].uop.block_size = ff_sws_uop_block_size_aarch64(uops);
        ret = ff_sws_uop_simplify_aarch64(&entries[i].uop);
        if (ret < 0)
            goto done;
    }

    for (int i = 0; i < uops->num_ops; i++) {
        ret = register_entry(ctx, &entries[i]);
        if (ret < 0)
            goto done;
    }

done:
    av_free(entries);
    ff_sws_uop_list_free(&uops);
    return ret == AVERROR(ENOTSUP) ? 0 : ret;
}

static void name_aarch64(const Entry *entry, char buf[SWS_UOP_NAME_MAX])
{
    ff_sws_uop_name_aarch64(&entry->uop, buf);
}

/*********************************************************************/

static const Target targets[] = {
    {
        .name       = "c",
        .guard      = "SWSCALE_UOPS_ENTRIES_C_H",
        .lists      = { "C" },
        .collect    = collect_c,
        .name_entry = name_generic,
    }, {
        .name       = "x86",
        .guard      = "SWSCALE_X86_UOPS_ENTRIES_H",
        .lists      = {
            [SWS_X86_SSE4]      = "X86_SSE4",
            [SWS_X86_AVX2]      = "X86_AVX2",
            [SWS_X86_AVX512]    = "X86_AVX512",
            [SWS_X86_AVX512ICL] = "X86_AVX512ICL",
        },
        .nasm       = true,
        .collect    = collect_x86,
        .name_entry = name_x86,
    }, {
        .name       = "aarch64",
        .guard      = "SWSCALE_AARCH64_UOPS_ENTRIES_H",
        .lists      = { "AARCH64" },
        .collect    = collect_aarch64,
        .name_entry = name_aarch64,
    },
};

static const Target *find_target(const char *name)
{
    for (int i = 0; i < FF_ARRAY_ELEMS(targets); i++) {
        if (!strcmp(targets[i].name, name))
            return &targets[i];
    }
    return NULL;
}

static int collect_uops(SwsContext *ctx, const SwsOpList *ops,
                        SwsCompiledOp *out)
{
    const struct EnumPriv *s = ctx->opaque;
    int ret = s->target->collect(ctx, ops);
    if (ret < 0)
        return ret;

    *out = (SwsCompiledOp) {0}; /* dummy value, will be immediately freed */
    return 0;
}

/* Dummy backend that just registers all needed kernels */
static const SwsOpBackend backend_collect = {
    .name    = "uops_entries_gen",
    .compile = collect_uops,
};

static int register_all_uops(SwsContext *ctx, void *graph, SwsOpList *ops)
{
    /* ff_sws_compile_pass() takes over ownership of `ops` */
    SwsOpList *copy = ff_sws_op_list_duplicate(ops);
    if (!copy)
        return AVERROR(ENOMEM);

    const int flags = SWS_OP_FLAG_DRY_RUN | SWS_OP_FLAG_SPLIT_MEMCPY;
    return ff_sws_compile_pass(graph, &backend_collect, &copy, flags, NULL, NULL);
}

static const SwsFlags flags_list[] = {
    0,
    SWS_ACCURATE_RND,   /* may insert extra 1x1 dither ops (for accurate rounding) */
    SWS_BITEXACT,       /* prevents some FMA optimizations */
    SWS_ACCURATE_RND | SWS_BITEXACT,
};

static int free_entry(void *opaque, void *key)
{
    av_free(key);
    return 0;
}

static void free_entries(struct AVTreeNode **root)
{
    av_tree_enumerate(*root, NULL, NULL, free_entry);
    av_tree_destroy(*root);
    *root = NULL;
}

/**
 * Collect all unique kernels needed by `target` into a newly allocated tree.
 *
 * This function can be quite slow as it iterates over every possible
 * combination of pixel formats and flags.
 *
 * Returns 0 or a negative error code. On success, the tree is returned via
 * `root`, and must be freed with free_entries() by the caller.
 */
static int collect_entries(struct AVTreeNode **root, const Target *target)
{
    struct EnumPriv s = { .target = target };
    SwsLut3D *lut3d = NULL;
    int ret = ff_mutex_init(&s.lock, NULL);
    if (ret)
        return AVERROR(ENOSYS);

    /* Allocate dummy graph and context for ff_sws_compile_pass() */
    SwsGraph *graph = ff_sws_graph_alloc();
    SwsContext *ctx = sws_alloc_context();
    if (!graph || !ctx) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    /* Use this to plumb the enum state through all the layers of abstraction */
    graph->ctx = ctx;
    ctx->opaque = &s;
    ctx->scaler = SWS_SCALE_BILINEAR; /* cheaper to generate filter kernels */
    ctx->threads = 0; /* use slice threading to speed up tree building */

    /* Allocate dummy 3DLUT to force generation of SWS_UOP_LUT_3D */
    lut3d = ff_sws_lut3d_alloc();
    if (!lut3d) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ret = ff_sws_enum_op_lists(ctx, graph, lut3d, AV_PIX_FMT_NONE,
                               AV_PIX_FMT_NONE, register_all_uops);
    if (ret < 0)
        goto fail;

    lut3d->dynamic = true;
    ret = ff_sws_enum_op_lists(ctx, graph, lut3d, AV_PIX_FMT_NONE,
                               AV_PIX_FMT_NONE, register_all_uops);
    if (ret < 0)
        goto fail;

    /* Register all unique kernels over every relevant combination of flags */
    for (int i = 0; i < FF_ARRAY_ELEMS(flags_list); i++) {
        ctx->flags = flags_list[i];
        ret = ff_sws_enum_op_lists(ctx, graph, NULL, AV_PIX_FMT_NONE,
                                   AV_PIX_FMT_NONE, register_all_uops);
        if (ret < 0)
            goto fail;
    }

    *root = s.root;
    s.root = NULL;

fail:
    av_refstruct_unref(&lut3d);
    free_entries(&s.root);
    ff_mutex_destroy(&s.lock);
    ff_sws_graph_free(&graph);
    sws_free_context(&ctx);
    return ret;
}

/*********************************************************************/
/* Output */

typedef struct PrintPriv {
    const Target *target;
    AVBPrint *bp;
    int list; /* only print entries of this list */
    const Entry *prev; /* previously printed entry */
} PrintPriv;

static int print_entry_c(void *opaque, void *key)
{
    PrintPriv *p = opaque;
    const Entry *entry = key;
    const SwsUOp *uop = &entry->uop;
    AVBPrint *bp = p->bp;
    if (entry->list != p->list)
        return 0;

    char name[SWS_UOP_NAME_MAX];
    p->target->name_entry(entry, name);
    av_bprintf(bp, " \\\n    MACRO(__VA_ARGS__, %s, %s, %s, %d, (0x%x",
               name, TYPE_SUFFIX(uop->type), UOP_SUFFIX(uop->uop),
               uop->block_size, uop->mask);
    print_params(bp, uop, false);
    av_bprintf(bp, "), (.type = %s, .uop = %s, .mask = 0x%x, .block_size = %d",
               pixel_types[uop->type], uop_names[uop->uop], uop->mask,
               uop->block_size);
    print_params(bp, uop, true);
    av_bprintf(bp, "))");
    return 0;
}

/**
 * Generate one C macro per kernel list, iterating over all its entries.
 *
 * Returns 0 or a negative error code. On success, an allocated string is
 * returned via `out_str`, and must be av_free()'d by the caller.
 */
static int print_c(struct AVTreeNode *root, const Target *target,
                   char **out_str)
{
    AVBPrint bprint;
    AVBPrint *const bp = &bprint;
    av_bprint_init(bp, 0, AV_BPRINT_SIZE_UNLIMITED);

    av_bprintf(bp,
"/**\n"
" * This file is automatically generated. Do not edit manually.\n"
" * To regenerate, run: make fate-sws-uops-entries-%s GEN=1\n"
" */\n"
"\n"
"#ifndef %s\n"
"#define %s\n"
"\n"
"/* See libswscale/uops_entries.h for the format of these lists */\n",
               target->name, target->guard, target->guard);

    for (int i = 0; i < FF_ARRAY_ELEMS(target->lists); i++) {
        if (!target->lists[i])
            continue;
        PrintPriv p = { .target = target, .bp = bp, .list = i };
        av_bprintf(bp, "\n#define SWS_ENTRIES_%s(MACRO, ...)", target->lists[i]);
        av_tree_enumerate(root, &p, NULL, print_entry_c);
        av_bprintf(bp, "\n");
    }

    av_bprintf(bp, "\n#endif /* %s */", target->guard);
    return av_bprint_finalize(bp, out_str);
}

static int print_entry_nasm(void *opaque, void *key)
{
    PrintPriv *p = opaque;
    const Entry *entry = key;
    const SwsUOp *uop = &entry->uop;
    AVBPrint *bp = p->bp;
    if (entry->list != p->list)
        return 0;

    const char *list = p->target->lists[p->list];
    if (!p->prev || p->prev->uop.type != uop->type || p->prev->uop.uop != uop->uop) {
        if (p->prev)
            av_bprintf(bp, "\n%%endmacro\n");
        av_bprintf(bp, "\n%%macro DECL_ENTRIES_%s_%s_%s 1", list,
                   TYPE_SUFFIX(uop->type), UOP_SUFFIX(uop->uop));
    }
    p->prev = entry;

    char name[SWS_UOP_NAME_MAX];
    p->target->name_entry(entry, name);
    av_bprintf(bp, "\n    DECL_ENTRY %%1, %d, %s, %s, %s, 0x%x", entry->vectors,
               name, pixel_types[uop->type], uop_names[uop->uop], uop->mask);
    print_params(bp, uop, false);
    return 0;
}

/**
 * Generate one NASM macro DECL_ENTRIES_<LIST>_<TYPE>_<UOP> per group of
 * kernels with the same list, pixel type and uop type. Same return semantics
 * as print_c().
 */
static int print_nasm(struct AVTreeNode *root, const Target *target,
                      char **out_str)
{
    AVBPrint bprint;
    AVBPrint *const bp = &bprint;
    av_bprint_init(bp, 0, AV_BPRINT_SIZE_UNLIMITED);

    av_bprintf(bp,
";******************************************************************************\n"
";* This file is automatically generated. Do not edit manually.\n"
";* To regenerate, run: make fate-sws-uops-entries-%s GEN=1\n"
";*\n"
";* Invoking DECL_ENTRIES_<LIST>_<TYPE>_<UOP> MACRO will instantiate, for\n"
";* every kernel of that group:\n"
";*   DECL_ENTRY MACRO, VECTORS, NAME, TYPE, UOP, MASK, [PARAMS]\n"
";* with parameters in struct order. Empty groups are not defined.\n"
";******************************************************************************\n",
               target->name);

    for (int i = 0; i < FF_ARRAY_ELEMS(target->lists); i++) {
        if (!target->lists[i])
            continue;
        PrintPriv p = { .target = target, .bp = bp, .list = i };
        av_tree_enumerate(root, &p, NULL, print_entry_nasm);
        if (p.prev)
            av_bprintf(bp, "\n%%endmacro\n");
    }

    return av_bprint_finalize(bp, out_str);
}

static int write_file(const char *path, const char *str)
{
    FILE *out = fopen(path, "wb");
    if (!out) {
        int ret = AVERROR(errno);
        fprintf(stderr, "Failed to open %s: %s\n", path, av_err2str(ret));
        return ret;
    }

    int ret = 0;
    if (fputs(str, out) < 0 || fputc('\n', out) < 0)
        ret = AVERROR(EIO);
    if (fclose(out) != 0 && ret >= 0)
        ret = AVERROR(errno);
    return ret;
}

static int usage(void)
{
    fprintf(stderr, "Usage: uops_entries_gen TARGET [-nasm FILE]\n"
                    "Writes the C kernel lists of TARGET to stdout, and the NASM\n"
                    "kernel lists to FILE for targets which have them.\n"
                    "TARGET is one of: c, x86, aarch64\n");
    return 1;
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    if (argc != 2 && argc != 4)
        return usage();

    const Target *target = find_target(argv[1]);
    if (!target)
        return usage();

    const char *nasm_path = NULL;
    if (argc == 4) {
        if (strcmp(argv[2], "-nasm") || !target->nasm)
            return usage();
        nasm_path = argv[3];
    }

    struct AVTreeNode *root = NULL;
    int ret = collect_entries(&root, target);
    if (ret < 0)
        return ret;

    char *str = NULL;
    ret = print_c(root, target, &str);
    if (ret >= 0)
        puts(str);
    av_free(str);

    if (ret >= 0 && nasm_path) {
        ret = print_nasm(root, target, &str);
        if (ret >= 0)
            ret = write_file(nasm_path, str);
        av_free(str);
    }

    free_entries(&root);
    return ret;
}
