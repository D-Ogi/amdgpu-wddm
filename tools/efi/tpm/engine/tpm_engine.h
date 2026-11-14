/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The TPM engine of the pre-OS TPM: libtpms with OpenSSL, for the UEFI stage (docs/design/tpm-preos.md). The stage
 * module (tools/efi/tpm/src/tpm_module.c) sees only this interface. The host tests of the module link a model of it
 * (tools/efi/tpm/test/engine_model.c).
 *
 * One engine per image, not re-entrant. All memory comes from the arena of the platform; the engine calls no
 * memory service of the firmware, so tpm_engine_process and tpm_engine_get_state are safe in a callback of
 * ExitBootServices.
 */
#ifndef TPM_ENGINE_H
#define TPM_ENGINE_H
#include <stddef.h>

struct tpm_engine_platform {
	void *arena;			/* memory for libtpms and OpenSSL, owned by the engine until tpm_engine_stop */
	size_t arena_bytes;
	unsigned long long (*monotonic_ms)(void *ctx);
	int (*entropy)(void *ctx, unsigned char *out, size_t n);	/* 1 when out holds n random bytes */
	void *ctx;
};

/* 1 on success. Called once, before tpm_engine_start. */
int tpm_engine_init(const struct tpm_engine_platform *p);

/* Starts the TPM. permanent NULL: a new TPM is manufactured. volatile_state NULL: no saved volatile state, the
 * caller sends TPM2_Startup. max_buffer is the largest command and response. Returns 0 on success, else a libtpms
 * TPM_RESULT. */
unsigned tpm_engine_start(const unsigned char *permanent, size_t permanent_bytes, const unsigned char *volatile_state,
			  size_t volatile_bytes, unsigned max_buffer);

/* One command. Returns the size of the response, 0 when the engine does not run or the response is larger than
 * cap. */
size_t tpm_engine_process(const unsigned char *cmd, size_t n, unsigned char *rsp, size_t cap);

#define TPM_ENGINE_PERMANENT	1
#define TPM_ENGINE_VOLATILE	2

/* The state blob of that kind, in the format of TPMLIB_GetState. Returns its size, 0 on failure or when cap is too
 * small. */
size_t tpm_engine_get_state(int kind, unsigned char *out, size_t cap);

/* 1 when libtpms stored its permanent state since tpm_engine_start. */
int tpm_engine_permanent_dirty(void);

/* Stops the TPM (TPMLIB_Terminate). The arena is free afterwards. */
void tpm_engine_stop(void);

/* The largest use of the arena so far, in bytes. */
size_t tpm_engine_arena_peak(void);

#endif
