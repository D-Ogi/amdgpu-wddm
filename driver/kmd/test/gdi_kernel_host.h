/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef BC250_GDI_KERNEL_HOST_H
#define BC250_GDI_KERNEL_HOST_H
/*
 * Compile driver/kmd/gdi_kernels.cl as host C, so that the host test runs the kernels' own source
 * text and not a second copy of it. The kernels are plain integer code over plain pointers; what
 * OpenCL C adds to them is four builtins, five keywords and three type names, and that is all this
 * header supplies.
 *
 * What this does NOT prove is named in the test and in docs/design/gdi-hw-accel.md: it runs the C,
 * not the gfx1013 machine code the build gate extracts, so it proves the addressing, the grid, the
 * argument packing and the arithmetic, and says nothing about instruction selection. The code
 * object is checked by tools/gpu/gdi_kernels.py and will be run on the lab arm.
 *
 * Include it AFTER the system headers (it defines a macro called __attribute__) and before
 * gdi_kernels.cl.
 */

/* The OpenCL C keywords the kernels use. `kernel` becomes an ordinary external function, which is
 * what lets the test call it. */
#define kernel
#define __global
#define __attribute__(ignored)

typedef unsigned char uchar;
typedef unsigned int uint;
typedef unsigned long long ulong;

/* The work-item and work-group identity, which the hardware puts in registers and the test puts
 * here. One work item runs at a time: every kernel of gdi_kernels.cl writes its own destination
 * pixel and reads nothing another work item writes, so running the grid in order is a faithful
 * serialisation of it (the admission refuses a command whose source and destination are one
 * allocation, which is the case that would not be).
 */
extern unsigned int g_Bc250GdiWorkgroupX, g_Bc250GdiWorkgroupY;
extern unsigned int g_Bc250GdiWorkitemX, g_Bc250GdiWorkitemY;

#define __builtin_amdgcn_workgroup_id_x() (g_Bc250GdiWorkgroupX)
#define __builtin_amdgcn_workgroup_id_y() (g_Bc250GdiWorkgroupY)
#define __builtin_amdgcn_workitem_id_x() (g_Bc250GdiWorkitemX)
#define __builtin_amdgcn_workitem_id_y() (g_Bc250GdiWorkitemY)

/* The arm of gdi_pixel.h the kernels need: global pointers and the OpenCL integer types, which the
 * typedefs above make mean the right thing here. */
#define BC250_GDI_CL 1

#endif
