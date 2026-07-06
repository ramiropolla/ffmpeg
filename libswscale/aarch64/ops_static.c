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

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

/**
 * This file is compiled as a standalone build-time tool and must not depend
 * on internal FFmpeg libraries. The necessary utils are redefined below using
 * standard C equivalents.
 */

#define AVUTIL_AVASSERT_H
#define AVUTIL_LOG_H
#define AVUTIL_MACROS_H
#define AVUTIL_MEM_H
#define av_assert0(cond) assert(cond)
#define av_malloc(s)     malloc(s)
#define av_mallocz(s)    calloc(1, s)
#define av_realloc(p, s) realloc(p, s)
#define av_strdup(s)     strdup(s)
#define av_free(p)       free(p)
#define FFMAX(a,b) ((a) > (b) ? (a) : (b))
#define FFMIN(a,b) ((a) > (b) ? (b) : (a))
#define MKTAG(a,b,c,d) ((a) | ((b) << 8) | ((c) << 16) | ((unsigned)(d) << 24))

static void av_freep(void *ptr)
{
    void **pptr = (void **) ptr;
    if (pptr) {
        ptr = *pptr;
        if (ptr)
            free(ptr);
        *pptr = NULL;
    }
}

static void *av_memdup(const void *p, size_t size)
{
    void *ptr = NULL;
    if (p) {
        ptr = av_malloc(size);
        if (ptr)
            memcpy(ptr, p, size);
    }
    return ptr;
}

#include "libavutil/dynarray.h"

static void *av_dynarray2_add(void **tab_ptr, int *nb_ptr, size_t elem_size,
                              const uint8_t *elem_data)
{
    uint8_t *tab_elem_data = NULL;

    FF_DYNARRAY_ADD(INT_MAX, elem_size, *tab_ptr, *nb_ptr, {
        tab_elem_data = (uint8_t *)*tab_ptr + (*nb_ptr) * elem_size;
        if (elem_data)
            memcpy(tab_elem_data, elem_data, elem_size);
    }, {
        av_freep(tab_ptr);
        *nb_ptr = 0;
    });
    return tab_elem_data;
}

#include "libavutil/bprint.c"

/*********************************************************************/
#include "rasm.c"
#include "rasm_print.c"
#include "ops_impl.h"

/**
 * Implementation parameters for all exported functions. This list is
 * compiled by performing a dummy run of all conversions in sws_ops and
 * collecting all functions that need to be generated. This is achieved
 * by running:
 *   make fate-sws-ops-entries-aarch64 GEN=1
 */
typedef struct SwsAArch64OpEntry {
    const char *name;
    SwsAArch64OpImplParams params;
} SwsAArch64OpEntry;

static const SwsAArch64OpEntry ops_entries[] = {
#define ENTRY(fname, ...) { .name = #fname, .params = __VA_ARGS__ },
#include "ops_entries.c"
#undef ENTRY
    { NULL }
};

/*********************************************************************/
#include "ops_asmgen.c"

/*********************************************************************/
static void asmgen_op_cps(SwsAArch64Context *s, const SwsAArch64OpEntry *entry)
{
    const SwsAArch64OpImplParams *p = &entry->params;
    RasmContext *r = s->rctx;

    bool is_read = false;
    bool is_write = false;
    switch (p->uop) {
    case SWS_UOP_READ_BIT:
    case SWS_UOP_READ_NIBBLE:
    case SWS_UOP_READ_PACKED:
    case SWS_UOP_READ_PLANAR:
        is_read = true;
        break;
    case SWS_UOP_WRITE_BIT:
    case SWS_UOP_WRITE_NIBBLE:
    case SWS_UOP_WRITE_PACKED:
    case SWS_UOP_WRITE_PLANAR:
        is_write = true;
        break;
    default:
        break;
    }

    rasm_func_begin(r, entry->name, true, !is_read);

    /**
     * Set up vector register dimensions and reshape all vectors
     * accordingly.
     */
    size_t el_size = ff_sws_pixel_type_size(p->type);
    size_t total_size = p->block_size * el_size;

    s->vec_size = FFMIN(total_size, 16);
    s->use_vh = (s->vec_size != total_size);

    s->el_size = el_size;
    s->el_count = s->vec_size / el_size;
    reshape_io_vectors(&s->regs, s->el_count, el_size);
    reshape_temp_vectors(&s->regs, s->el_count, el_size);
    reshape_const_vectors(&s->regs, s->el_count, el_size);

    /* Common start for continuation-passing style (CPS) functions. */
    s->impl_priv = a64op_off(s->impl, offsetof_impl_priv);
    asmgen_set_load_cont_node(s);

    /* Set up constants. */
    switch (p->uop) {
    case SWS_UOP_READ_BIT:     asmgen_setup_read_bit(s, p, &s->regs);     break;
    case SWS_UOP_READ_NIBBLE:  asmgen_setup_read_nibble(s, p, &s->regs);  break;
    case SWS_UOP_WRITE_BIT:    asmgen_setup_write_bit(s, p, &s->regs);    break;
    case SWS_UOP_UNPACK:       asmgen_setup_unpack(s, p, &s->regs);       break;
    case SWS_UOP_CLEAR:        asmgen_setup_clear(s, p, &s->regs);        break;
    case SWS_UOP_MIN:          asmgen_setup_min(s, p, &s->regs);          break;
    case SWS_UOP_MAX:          asmgen_setup_max(s, p, &s->regs);          break;
    case SWS_UOP_SCALE:        asmgen_setup_scale(s, p, &s->regs);        break;
    case SWS_UOP_LINEAR:       asmgen_setup_linear(s, p, &s->regs);       break;
    case SWS_UOP_LINEAR_FMA:   asmgen_setup_linear(s, p, &s->regs);       break;
    case SWS_UOP_DITHER:       asmgen_setup_dither(s, p, &s->regs);       break;
    default:
        break;
    }

    /* Emit uop kernel. */
    switch (p->uop) {
    case SWS_UOP_READ_BIT:     asmgen_op_read_bit(s, p, &s->regs);     break;
    case SWS_UOP_READ_NIBBLE:  asmgen_op_read_nibble(s, p, &s->regs);  break;
    case SWS_UOP_READ_PACKED:  asmgen_op_read_packed(s, p, &s->regs);  break;
    case SWS_UOP_READ_PLANAR:  asmgen_op_read_planar(s, p, &s->regs);  break;
    case SWS_UOP_WRITE_BIT:    asmgen_op_write_bit(s, p, &s->regs);    break;
    case SWS_UOP_WRITE_NIBBLE: asmgen_op_write_nibble(s, p, &s->regs); break;
    case SWS_UOP_WRITE_PACKED: asmgen_op_write_packed(s, p, &s->regs); break;
    case SWS_UOP_WRITE_PLANAR: asmgen_op_write_planar(s, p, &s->regs); break;
    case SWS_UOP_SWAP_BYTES:   asmgen_op_swap_bytes(s, p, &s->regs);   break;
    case SWS_UOP_MOVE:         asmgen_op_move(s, p, &s->regs);         break;
    case SWS_UOP_UNPACK:       asmgen_op_unpack(s, p, &s->regs);       break;
    case SWS_UOP_PACK:         asmgen_op_pack(s, p, &s->regs);         break;
    case SWS_UOP_LSHIFT:       asmgen_op_lshift(s, p, &s->regs);       break;
    case SWS_UOP_RSHIFT:       asmgen_op_rshift(s, p, &s->regs);       break;
    case SWS_UOP_CLEAR:        asmgen_op_clear(s, p, &s->regs);        break;
    case SWS_UOP_TO_U8:        asmgen_op_convert(s, p, &s->regs);      break;
    case SWS_UOP_TO_U16:       asmgen_op_convert(s, p, &s->regs);      break;
    case SWS_UOP_TO_U32:       asmgen_op_convert(s, p, &s->regs);      break;
    case SWS_UOP_TO_F32:       asmgen_op_convert(s, p, &s->regs);      break;
    case SWS_UOP_EXPAND_PAIR:  asmgen_op_expand(s, p, &s->regs);       break;
    case SWS_UOP_EXPAND_QUAD:  asmgen_op_expand(s, p, &s->regs);       break;
    case SWS_UOP_MIN:          asmgen_op_min(s, p, &s->regs);          break;
    case SWS_UOP_MAX:          asmgen_op_max(s, p, &s->regs);          break;
    case SWS_UOP_SCALE:        asmgen_op_scale(s, p, &s->regs);        break;
    case SWS_UOP_LINEAR:       asmgen_op_linear(s, p, &s->regs);       break;
    case SWS_UOP_LINEAR_FMA:   asmgen_op_linear(s, p, &s->regs);       break;
    case SWS_UOP_DITHER:       asmgen_op_dither(s, p, &s->regs);       break;
    /* TODO implement SWS_UOP_SHUFFLE */
    default:
        break;
    }

    if (is_write) {
        /* Write functions return directly. */
        i_ret(r);
    } else {
        /* Load continuation address and increment impl pointer. */
        RasmNode *node = rasm_set_current_node(r, s->load_cont_node);
        RasmOp impl_post = a64op_post(s->impl, sizeof_impl);
        i_ldr(r, s->cont, impl_post);                   CMT("SwsFuncPtr cont = (impl++)->cont;");
        rasm_set_current_node(r, node);
        /* Common end for remaining CPS functions. */
        i_br (r, s->cont);                              CMT("jump to cont");
    }
}

/*********************************************************************/

/* Generate all functions described by ops_entries.c */
static int asmgen(void)
{
    RasmContext *rctx = rasm_alloc();
    if (!rctx)
        return AVERROR(ENOMEM);

    SwsAArch64Context s = { .rctx = rctx };
    AVBPrint bp;
    int ret;

    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);

    /**
     * The entry point of the SwsOpFunc is the `process` function. The
     * first kernel function is called from `process`, and subsequent
     * kernel functions are chained by directly branching to the next
     * operation, using a continuation-passing style design. The last
     * operation must be a write operation, which returns from the call
     * to the `process` function.
     *
     * The GPRs used by the entire call-chain are listed below.
     *
     * Function arguments are passed in r0-r5. After the parameters
     * from `exec` have been read, r0 is reused to branch to the
     * continuation functions. After the original parameters from
     * `impl` have been computed, r1 is reused as the `impl` pointer
     * for each operation.
     *
     * Loop iterators are r6 for `bx` and r3 for `y`, reused from
     * `y_start`, which doesn't need to be preserved.
     *
     * The intra-procedure-call temporary registers (r16 and r17) are
     * used as scratch registers. They may be used by call veneers and
     * PLT code inserted by the linker, so we cannot expect them to
     * persist across branches between functions.
     *
     * The Platform Register (r18) is not used.
     *
     * The read/write data pointers and padding values first use up the
     * remaining free caller-saved registers, and only then are the
     * caller-saved registers (r19-r28) used.
     *
     * The Link Register (r30) is used when calling the first kernel,
     * so it must be saved.
     */

    /* SwsOpFunc arguments. */
    s.exec      = a64op_gpx(0); // const SwsOpExec *exec
    s.impl      = a64op_gpx(1); // const void *priv
    s.bx_start  = a64op_gpw(2); // int bx_start
    s.y_start   = a64op_gpw(3); // int y_start
    s.bx_end    = a64op_gpw(4); // int bx_end
    s.y_end     = a64op_gpw(5); // int y_end

    /* Loop iterator variables. */
    s.bx        = a64op_gpw(6);
    s.y         = s.y_start;    /* Reused from SwsOpFunc argument. */

    /* Scratch registers. */
    s.tmp0      = a64op_gpx(16); /* IP0 */
    s.tmp1      = a64op_gpx(17); /* IP1 */

    /* CPS-related variables. */
    s.op0_func  = a64op_gpx(7);
    s.op1_impl  = a64op_gpx(8);
    s.cont      = s.exec;       /* Reused from SwsOpFunc argument. */

    /* Read/Write data pointers and padding. */
    s.in      [0] = a64op_gpx(9);
    s.out     [0] = a64op_gpx(10);
    s.in_bump [0] = a64op_gpx(11);
    s.out_bump[0] = a64op_gpx(12);
    s.in      [1] = a64op_gpx(13);
    s.out     [1] = a64op_gpx(14);
    s.in_bump [1] = a64op_gpx(15);
    s.out_bump[1] = a64op_gpx(19);
    s.in      [2] = a64op_gpx(20);
    s.out     [2] = a64op_gpx(21);
    s.in_bump [2] = a64op_gpx(22);
    s.out_bump[2] = a64op_gpx(23);
    s.in      [3] = a64op_gpx(24);
    s.out     [3] = a64op_gpx(25);
    s.in_bump [3] = a64op_gpx(26);
    s.out_bump[3] = a64op_gpx(27);

    /* Generate all process functions using rasm. */
    asmgen_process(&s, SWS_COMP_MASK(1, 0, 0, 0));
    asmgen_process(&s, SWS_COMP_MASK(1, 1, 0, 0));
    asmgen_process(&s, SWS_COMP_MASK(1, 1, 1, 0));
    asmgen_process(&s, SWS_COMP_MASK(1, 1, 1, 1));

    /* Generate all functions from ops_entries.c using rasm. */
    const SwsAArch64OpEntry *entries = ops_entries;
    while (entries->name) {
        asmgen_op_cps(&s, entries++);
        if (rctx->error) {
            ret = rctx->error;
            goto error;
        }
    }

    /* Print all rasm functions to stdout. */
    printf("#include \"libavutil/aarch64/asm.S\"\n");
    printf("\n");
    ret = rasm_print(s.rctx, &bp);
    if (ret < 0)
        goto error;
    fputs(bp.str, stdout);

error:
    av_bprint_finalize(&bp, NULL);
    rasm_free(&s.rctx);
    return ret;
}

/*********************************************************************/
int main(int argc, char *argv[])
{
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    return asmgen();
}
