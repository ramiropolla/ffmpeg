#include "libswscale/ffasm/aarch64.h"

#include <stdio.h>

int main(int argc, char *argv[])
{
    AArch64Context *actx = aarch64_alloc();

    AArch64Op x0 = aarch64_x0();
    AArch64Op x1 = aarch64_x1();
    AArch64Op x2 = aarch64_x2();

    /* GPR instructions */
    aarch64_add(actx, x0, x1, x2);
    aarch64_mov(actx, x0, x1);

    /* vector with arrangement specifier */
    aarch64_fadd(actx,  aarch64_vec4s(0), aarch64_vec4s(1), aarch64_vec4s(2));
    aarch64_ushr(actx,  aarch64_vec8h(3), aarch64_vec8h(4), aarch64_gpx(5));
    aarch64_rev16(actx, aarch64_vec16b(0), aarch64_vec16b(1));

    /* scalar arrangement specifier */
    aarch64_addv(actx, aarch64_s0(), aarch64_vec4s(1));
    aarch64_ucvtf(actx, aarch64_d0(), aarch64_d1());

    /* no arrangement specifier */
    aarch64_mov(actx, aarch64_v2(), aarch64_v3());

    AVBPrint bp;
    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);
    aarch64_print(actx, &bp);
    printf("%s", bp.str);
    av_bprint_finalize(&bp, NULL);

    aarch64_free(&actx);

    return 0;
}
