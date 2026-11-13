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

/* Runs one command. Writes at most capacity bytes of answer and returns the count, 0 on a core failure (the
 * caller then answers TPM_RC_FAILURE itself). */
size_t tpmcore_process(const unsigned char *command, size_t command_bytes, unsigned char *response, size_t capacity);

/* Stores the volatile state and stops the core (service stop, system shutdown). */
void tpmcore_stop(void);

#endif
