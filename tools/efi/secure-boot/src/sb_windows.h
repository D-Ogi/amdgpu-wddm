/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The Secure Boot state that the operating system reads, presented by the stage (docs/design/efi-resident-stage.md,
 * "The Secure Boot module", sb.windows). The board has no Secure Boot in its firmware. This part gives the operating
 * system, at runtime, the four databases of the kit and the two global variables SecureBoot and SetupMode, through a
 * filter in front of the firmware's GetVariable. Nothing is written to NVRAM: no SetVariable call exists in the
 * stage. Only a start with sb.mode=enforce and sb.windows=1 installs the filter, and every other variable goes to the
 * firmware's own function unchanged.
 *
 * The filter and the address-change notify run after the stage returns, when the image of the stage is freed.
 * sb_windows.c copies them into memory of the type EfiRuntimeServicesCode and keeps their state in memory of the
 * type EfiRuntimeServicesData.
 */
#ifndef SB_WINDOWS_H
#define SB_WINDOWS_H
#include <stddef.h>
#include "stage.h"

struct sb_windows_inputs {
	const unsigned char *pk;
	size_t pk_bytes;
	const unsigned char *kek;
	size_t kek_bytes;
	const unsigned char *db;
	size_t db_bytes;
	const unsigned char *dbx;
	size_t dbx_bytes;
};

/* Installs the filter and the address-change notify. The database bytes are copied into runtime memory, so the
 * caller keeps ownership of its own buffers. 1 when the firmware took the filter (and the read-back says so), 0 when
 * it did not; the caller logs the reason in either case. */
int sb_windows_install(struct stage *stage, const struct sb_windows_inputs *inputs);

/* Puts the firmware's own GetVariable back, closes the event and frees the runtime memory. Safe to call after a
 * failed install and after the filter answered. */
void sb_windows_unwind(struct stage *stage);

/* One line for the console and the log: what the install answered. */
const char *sb_windows_result_text(int result);

#endif
