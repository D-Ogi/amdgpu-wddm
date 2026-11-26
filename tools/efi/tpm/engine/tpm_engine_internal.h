/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The parts of the TPM engine that only its own files use. */
#ifndef BC250_TPM_ENGINE_INTERNAL_H
#define BC250_TPM_ENGINE_INTERNAL_H
#include <stddef.h>

/* The entropy source of the platform (tpm_engine_platform.entropy). 1 = out has n bytes of full entropy. */
int tpm_engine_entropy(unsigned char *out, size_t n);

#endif
