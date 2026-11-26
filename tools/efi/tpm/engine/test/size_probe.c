/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The size probe of build.ps1: a UEFI application that calls each function of tpm_engine.h, linked as the stage is
 * linked. The difference to the same application without the calls (SIZE_PROBE_EMPTY) is the size that the engine
 * adds to the stage image. It also shows that the archive links into a UEFI image with the efl_libc.o of the stage.
 * The probe is never run.
 */
#include <stddef.h>
#include "tpm_engine.h"

static unsigned long long probe_clock(void *ctx)
{
	(void)ctx;
	return 0;
}

static int probe_entropy(void *ctx, unsigned char *out, size_t n)
{
	(void)ctx;
	(void)out;
	(void)n;
	return 0;
}

unsigned long long efi_main(void *image, void *system_table)
{
#ifndef SIZE_PROBE_EMPTY
	static unsigned char arena[4096], rsp[64];
	struct tpm_engine_platform p = {arena, sizeof(arena), probe_clock, probe_entropy, NULL};
	unsigned r;
	(void)image;
	(void)system_table;
	if (!tpm_engine_init(&p)) return 1;
	tpm_engine_set_log(NULL, NULL);
	r = tpm_engine_start(NULL, 0, NULL, 0, 0);
	r += (unsigned)tpm_engine_process(rsp, sizeof(rsp), rsp, sizeof(rsp));
	r += (unsigned)tpm_engine_get_state(TPM_ENGINE_PERMANENT, NULL, 0);
	r += (unsigned)tpm_engine_permanent_dirty() + (unsigned)tpm_engine_arena_peak();
	tpm_engine_stop();
	tpm_engine_wipe();
	return r;
#else
	(void)image;
	(void)system_table;
	(void)probe_clock;
	(void)probe_entropy;
	return 0;
#endif
}
