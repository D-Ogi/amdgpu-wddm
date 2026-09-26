/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * driver/contract/uapi-shim/drm.h - ours, NOT the kernel's drm.h.
 *
 * driver/contract/third_party/amdgpu_drm.h is the Linux UAPI header imported byte for byte, and
 * its first line of code is `#include "drm.h"`. Of that header it uses exactly two things:
 *
 *   - the fixed-width typedefs __u8 __u16 __u32 __u64 __s32 __s64;
 *   - the ioctl-number macros DRM_COMMAND_BASE, DRM_IO, DRM_IOW, DRM_IOWR, which appear only in
 *     the DRM_IOCTL_AMDGPU_* defines at amdgpu_drm.h:62-90.
 *
 * Nothing else. This file supplies those and nothing else, so that the UAPI header compiles under
 * MSVC in both user mode and /kernel without being edited. Importing the kernel's real drm.h would
 * drag in <sys/ioccom.h>, <linux/types.h> and the BSD portability arm, none of which exist here.
 *
 * The DRM_IO* macros expand to a token that is never a valid ioctl number, because this driver
 * never issues a DRM ioctl: on Windows the same structures travel through a WDDM private-data
 * blob (driver/contract/bc250_umd_private.h), not through /dev/dri. If a translation unit ever
 * *uses* one of the DRM_IOCTL_AMDGPU_* macros it will fail to compile, which is the intent.
 *
 * C's quote-include rules put this file on the search path without shadowing anything: the
 * compiler looks in third_party/ first (no drm.h there), then falls back to -I uapi-shim.
 */
#ifndef BC250_CONTRACT_UAPI_SHIM_DRM_H
#define BC250_CONTRACT_UAPI_SHIM_DRM_H

#if defined(BC250_CONTRACT_KERNEL) || defined(BC250_SHIM_KERNEL) || defined(_KERNEL_MODE)
/* The WDK's km CRT has no <stdint.h>; take the names from the compiler's own types, the same way
 * driver/shim/include/amdgpu.h does. */
typedef unsigned char      __u8;
typedef unsigned short     __u16;
typedef unsigned int       __u32;
typedef unsigned __int64   __u64;
typedef signed char        __s8;
typedef short              __s16;
typedef int                __s32;
typedef __int64            __s64;
#else
#include <stdint.h>
typedef uint8_t  __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int8_t   __s8;
typedef int16_t  __s16;
typedef int32_t  __s32;
typedef int64_t  __s64;
#endif

/* Poison. See the file comment: these exist so amdgpu_drm.h:62-90 parses, not so they work. */
#define DRM_COMMAND_BASE                0x40
#define DRM_IO(nr)                      BC250_CONTRACT_NO_DRM_IOCTLS_HERE
#define DRM_IOR(nr, type)               BC250_CONTRACT_NO_DRM_IOCTLS_HERE
#define DRM_IOW(nr, type)               BC250_CONTRACT_NO_DRM_IOCTLS_HERE
#define DRM_IOWR(nr, type)              BC250_CONTRACT_NO_DRM_IOCTLS_HERE

#endif /* BC250_CONTRACT_UAPI_SHIM_DRM_H */
