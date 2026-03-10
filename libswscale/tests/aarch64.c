#include "libswscale/ffasm/aarch64.h"

#include <stdio.h>

int main(int argc, char *argv[])
{
    AArch64Context *actx = aarch64_alloc();

    AArch64Op x0 = a64op_gpx(0);
    AArch64Op x1 = a64op_gpx(1);
    AArch64Op x2 = a64op_gpx(2);
    AArch64Op v2 = a64op_vec(2);
    AArch64Op v3 = a64op_vec(3);

    aarch64_add_comment(actx, "apfelstrudel");

    /* GPR instructions */
    a64insn_add(actx, x0, x1, x2); aarch64_annotate(actx, "something");
    a64insn_mov(actx, x0, x1);

    /* vector with arrangement specifier */
    a64insn_fadd(actx,  a64op_vec4s(0), a64op_vec4s(1), a64op_vec4s(2));
    a64insn_ushr(actx,  a64op_vec8h(3), a64op_vec8h(4), a64op_gpx(5));
    a64insn_rev16(actx, a64op_vec16b(0), a64op_vec16b(1));

    /* scalar arrangement specifier */
    a64insn_addv(actx, a64op_vecs(0), a64op_vec4s(1));
    a64insn_ucvtf(actx, a64op_vecd(0), a64op_vecd(1));

    /* no arrangement specifier */
    a64insn_mov(actx, v2, v3);

    AVBPrint bp;
    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);
    aarch64_print(actx, &bp);
    printf("%s", bp.str);
    av_bprint_finalize(&bp, NULL);

    aarch64_free(&actx);

    return 0;
}
