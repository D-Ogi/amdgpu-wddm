/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The controls and counters of the model engine (engine_model.c), for the host tests. */
#ifndef ENGINE_MODEL_H
#define ENGINE_MODEL_H

struct engine_model_state {
	int fail_init;		/* tpm_engine_init fails */
	int new_sha256_only;	/* a new TPM has the SHA-256 bank only: no PCR_Allocate */
	unsigned inits, starts, stops, commands, extends, wipes, gcm_calls;
	int from_state;		/* the last start had a permanent state */
};

extern struct engine_model_state engine_model;

/* Stops the model and clears the controls and counters. */
void engine_model_reset(void);
int engine_model_running(void);

/* Makes tpm_engine_gcm work before a tpm_engine_init, for a test that writes state files. */
void engine_model_init_cipher(void);

#endif
