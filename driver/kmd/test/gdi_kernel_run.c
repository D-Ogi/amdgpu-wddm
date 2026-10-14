/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include "gdi_kernel_run.h"
#ifdef _MSC_VER
/* The six kernels share one argument list on purpose, so each of them leaves part of it
 * unread; that is the point of GDI_ARGS and not an oversight. The device compiler does not
 * warn about it, and the driver never compiles this file. */
#pragma warning(disable: 4100)
#endif
#include "gdi_kernel_host.h"
#include "../gdi_kernels.cl"

/*
 * The kernels, as host C. Nothing else is in this translation unit, and it includes no system
 * header and no driver header: see gdi_kernel_run.h for why it stands alone.
 */

unsigned int g_Bc250GdiWorkgroupX, g_Bc250GdiWorkgroupY;
unsigned int g_Bc250GdiWorkitemX, g_Bc250GdiWorkitemY;

/* The scalars, expanded in the order of the kernel's parameter list. Two of them are signed, which
 * is the only place this file knows anything about what an argument means. */
#define RUN(fn) fn(Dst, Src, Alpha, Gamma, s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8],   \
                   s[9], (int)s[10], (int)s[11], s[12], s[13], s[14], s[15], s[16], s[17])

int Bc250GdiRunKernel(unsigned int Kernel, unsigned char* Dst, const unsigned char* Src,
                      const unsigned char* Alpha, const unsigned char* Gamma,
                      const unsigned int* Scalars)
{
    const unsigned int* s = Scalars;
    if (!Dst || !s) return 0;
    /* The order is enum bc250_gdi_kernel's, which the test checks by name. */
    switch (Kernel) {
    case 1: RUN(gdi_bitblt_rop); return 1;
    case 2: RUN(gdi_colorfill_rop); return 1;
    case 3: RUN(gdi_alphablend); return 1;
    case 4: RUN(gdi_stretchblt); return 1;
    case 5: RUN(gdi_transparentblt); return 1;
    case 6: RUN(gdi_cleartypeblend); return 1;
    default: return 0;
    }
}

const char* Bc250GdiRunKernelName(unsigned int Kernel)
{
    switch (Kernel) {
    case 1: return "gdi_bitblt_rop";
    case 2: return "gdi_colorfill_rop";
    case 3: return "gdi_alphablend";
    case 4: return "gdi_stretchblt";
    case 5: return "gdi_transparentblt";
    case 6: return "gdi_cleartypeblend";
    default: return 0;
    }
}
