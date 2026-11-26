/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The TPM engine of the stage: libtpms (TPM 2.0) and OpenSSL, built for the stage target, behind one small C
 * interface (design: docs/design/tpm-preos.md, "The parts"). The same objects run in the UEFI binding
 * (tpm/src/tpm_module.c) and in the host test (tpm/engine/test).
 *
 * One TPM for each image: libtpms keeps its TPM in global state. No threads: the caller makes one call at a time.
 * All memory comes from the arena of tpm_engine_init; the engine never asks the firmware for memory.
 *
 * The state blobs are those of libtpms (TPMLIB_GetState and TPMLIB_SetState): the permanent state ("permall",
 * the NV image of the TPM) and the volatile state (PCRs, loaded objects, sessions; with a SHA-1 checksum). The
 * Windows core (driver/tpm/core) runs the same libtpms revision and takes the same blobs.
 */
#ifndef BC250_TPM_ENGINE_H
#define BC250_TPM_ENGINE_H
#include <stddef.h>

/* The return codes of tpm_engine_start that the engine makes itself. They are the libtpms values (tpm_error.h);
 * every other code comes from libtpms or is a TPM_RC of the TPM. */
#define TPM_ENGINE_OK 0x00u
#define TPM_ENGINE_BAD_PARAMETER 0x03u	/* a blob without its size, a volatile blob without a permanent blob */
#define TPM_ENGINE_FAIL 0x09u		/* not initialized, running already, out of arena, or the engine failed */
#define TPM_ENGINE_SIZE 0x17u		/* max_buffer is outside what libtpms supports */

#define TPM_ENGINE_PERMANENT 1
#define TPM_ENGINE_VOLATILE 2

struct tpm_engine_platform {		/* given by the binding (UEFI or host) */
	void *arena;			/* all malloc and free of libtpms and OpenSSL come from here; never from firmware */
	size_t arena_bytes;
	unsigned long long (*monotonic_ms)(void *ctx);	/* the TPM clock source; must never go back */
	/* Fills out with n bytes of full entropy (RDSEED, EFI_RNG_PROTOCOL raw, BCryptGenRandom). 1 = ok. It seeds the
	 * DRBG of OpenSSL, and the TPM takes its random numbers from that DRBG. */
	int (*entropy)(void *ctx, unsigned char *out, size_t n);
	void *ctx;			/* given to both functions */
};

/* Once for each image: starts the C runtime on the arena and OpenSSL, and reads 32 bytes from the seeded DRBG as a
 * check of the entropy source. Returns 1, or 0 when a field is missing, the arena is too small, the entropy source
 * fails, or the engine is initialized already. The arena belongs to the engine from now on: OpenSSL keeps its
 * global state in it until tpm_engine_wipe. */
int tpm_engine_init(const struct tpm_engine_platform *p);

/* Starts the TPM. The engine keeps its own copies of the blobs; the caller may free them after the call.
 *   permanent: NULL/0 = manufacture a new TPM (new seeds); else the permanent state to continue.
 *   volatile_state: NULL/0 = a fresh start: the caller then sends TPM2_Startup. Non-NULL = resume: the TPM
 *     continues with these PCRs, objects and sessions, and needs no TPM2_Startup. It needs a permanent blob.
 *   max_buffer: the largest command and response of the transport (0 = the libtpms maximum, 4096). libtpms reports
 *     it in TPM2_GetCapability.
 * Returns 0, or a TPM_ENGINE_* code, a libtpms code or a TPM_RC (TPM_RC_HASH 0x083 is a damaged volatile blob).
 * After a failure the TPM does not run and start may be called again. */
unsigned tpm_engine_start(const unsigned char *permanent, size_t permanent_bytes,
			  const unsigned char *volatile_state, size_t volatile_bytes, unsigned max_buffer);

/* Runs one TPM command. Writes the response to rsp and returns its size, or 0 on a failure of the engine (the TPM
 * does not run, the response does not fit in cap). A TPM error is a response, not 0. */
size_t tpm_engine_process(const unsigned char *cmd, size_t n, unsigned char *rsp, size_t cap);

/* Copies the TPM_ENGINE_PERMANENT or TPM_ENGINE_VOLATILE state into out and returns its size, or 0 when it does
 * not fit or on a failure. out NULL with cap 0 returns the size without a copy. The TPM must run. It allocates only
 * from the arena (it runs in an ExitBootServices callback); the permanent state needs about 260 KiB of arena for a
 * short time. A successful copy of the permanent state clears tpm_engine_permanent_dirty. */
size_t tpm_engine_get_state(int kind, unsigned char *out, size_t cap);

/* 1 when libtpms stored NV state that differs from the last permanent state the caller has (the blob of start, or
 * of the last tpm_engine_get_state(TPM_ENGINE_PERMANENT)). A manufacture sets it. */
int tpm_engine_permanent_dirty(void);

/* Stops the TPM (TPMLIB_Terminate) without TPM2_Shutdown: for a handoff the TPM continues elsewhere. Send
 * TPM2_Shutdown first for an orderly stop. The engine can start again in the same arena. */
void tpm_engine_stop(void);

/* The largest extent of the arena that was in use since tpm_engine_init: the arena size that this load needs. */
size_t tpm_engine_arena_peak(void);

/* Stops the TPM if it runs, writes zeros over the whole arena (seeds, keys, the OpenSSL state) and disables the
 * engine for the rest of the image. For the last step before the arena goes to the next owner. */
void tpm_engine_wipe(void);

/* AES-256-GCM with the OpenSSL of the engine, in the contract of tpm_statefile_gcm (driver/tpm/statefile.h): the
 * cipher of the state files on the EFI system partition. Returns 0 before tpm_engine_init and after
 * tpm_engine_wipe, because the OpenSSL state lives in the arena. */
int tpm_engine_gcm(int seal, const unsigned char key[32], const unsigned char nonce[12], const unsigned char *aad,
		   size_t aad_bytes, const unsigned char *in, size_t n, unsigned char *out, unsigned char tag[16]);

/* An optional sink for the log lines of libtpms and the C runtime (one call for each piece of text). NULL: none. */
void tpm_engine_set_log(void (*sink)(void *ctx, const char *text, size_t bytes), void *ctx);

#endif
