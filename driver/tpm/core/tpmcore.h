/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The TPM 2.0 core of the servicer: libtpms (TPM 2.0 reference code with OpenSSL), its persistent state in one
 * directory, and the platform duties that firmware does on a real machine (_TPM_Init and TPM2_Startup(CLEAR) once
 * per start of the core).
 *
 * libtpms keeps one process-wide TPM, so this module has no handle: one core per process.
 */
#ifndef BC250_TPMCORE_H
#define BC250_TPMCORE_H
#include <stddef.h>

/* Starts the core with its state in state_dir (created if absent; the caller sets its ACL). max_buffer is the
 * largest command and response the transport carries (the CRB data buffer); libtpms reports it to the requester
 * through TPM2_GetCapability, so that the requester never sends more. Returns 0 or a TPM_RC / libtpms code. */
unsigned tpmcore_start(const wchar_t *state_dir, unsigned max_buffer);

/* Starts the core as the continuation of the pre-OS TPM of the stage (driver/tpm/handoff.h): the TPM that measured
 * this boot, with its PCRs, objects and sessions. Both states go to libtpms before _TPM_Init, and no TPM2_Startup is
 * sent (the stage sent it). The blobs are libtpms blobs of the same revision as this core (PROVENANCE.md). libtpms
 * stores its permanent state in state_dir from then on. Returns 0 or a TPM_RC / libtpms code; a damaged volatile
 * blob gives TPM_RC_HASH (0x083). */
unsigned tpmcore_resume(const wchar_t *state_dir, unsigned max_buffer, const unsigned char *permanent,
			unsigned permanent_bytes, const unsigned char *volatile_state, unsigned volatile_bytes);

/* Runs one command. Writes at most capacity bytes of answer and returns the count, 0 on a core failure (the
 * caller then answers TPM_RC_FAILURE itself). */
size_t tpmcore_process(const unsigned char *command, size_t command_bytes, unsigned char *response, size_t capacity);

/* Called after each store of the permanent state of libtpms (its NV image, "permall") with the stored blob, on the
 * thread of the command that changed it. NULL: none. The pre-OS TPM keeps its state files current with it. */
void tpmcore_on_permanent(void (*fn)(const unsigned char *blob, unsigned bytes));

/* Stores the volatile state and stops the core (service stop, system shutdown). */
void tpmcore_stop(void);

#endif
