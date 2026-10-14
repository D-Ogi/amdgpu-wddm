/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The WDK declarations of the GDI command buffer, for gdi_walk.c and nothing else.
 *
 * gdi_walk.c is the one unit of the driver that does not include bc250kmd.h, because the host test
 * of the walk compiles it as it is, with no copy of it and no second version of the walk. So this
 * file brings the same prelude that bc250kmd.h brings before the display interface: dispmprt.h
 * itself includes acpiioct.h, which needs the kernel types, and windef.h gives RECT.
 *
 * The host test (driver/kmd/test/run_gdi_walk.ps1) defines BC250_GDI_WALK_DDI_H on the command
 * line and force-includes its own slice of the WDK GDI types, so the body below is skipped and the
 * driver build is not touched at all.
 */
#ifndef BC250_GDI_WALK_DDI_H
#define BC250_GDI_WALK_DDI_H

#include <ntifs.h>
#include <windef.h>
#include <dispmprt.h>

#endif /* BC250_GDI_WALK_DDI_H */
