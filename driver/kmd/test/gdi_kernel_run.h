/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef BC250_GDI_KERNEL_RUN_H
#define BC250_GDI_KERNEL_RUN_H
/*
 * One work item of one GDI kernel, run on this processor. The implementation is
 * gdi_kernel_run.c, which is nothing but gdi_kernels.cl compiled as host C (gdi_kernel_host.h):
 * the test calls the kernels' own source text.
 *
 * It is a translation unit of its own because gdi_pixel.h has two arms and a header guard, so one
 * unit cannot hold both the kernels' arm and the driver's. That separation is useful on its own:
 * nothing of the kernels is visible to the test but these three functions.
 */

/* The work item to run, which the hardware would put in SGPRs and VGPRs. */
extern unsigned int g_Bc250GdiWorkgroupX, g_Bc250GdiWorkgroupY;
extern unsigned int g_Bc250GdiWorkitemX, g_Bc250GdiWorkitemY;

/* The scalar arguments, in the order of the kernel's parameter list, which is the order of the
 * argument block (BC250_GDI_ARG_* of gdi_kernels.generated.h) from the first scalar onward. The
 * caller passes &block[BC250_GDI_ARG_DSTPITCH]. */
#define BC250_GDI_RUN_SCALARS 18

/* Run the work item in the four globals above. Kernel is a value of enum bc250_gdi_kernel. Returns
 * 0 for an index that is not one of the six. */
int Bc250GdiRunKernel(unsigned int Kernel, unsigned char* Dst, const unsigned char* Src,
                      const unsigned char* Alpha, const unsigned char* Gamma,
                      const unsigned int* Scalars);

/* The entry point name of that index, for the test to hold against the generated name list: the
 * table in gdi_kernel_run.c and the enum must agree, and this is how the test sees that they do. */
const char* Bc250GdiRunKernelName(unsigned int Kernel);

#endif
