/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The interface between the host binding (host_binding.c) and the engine test (engine_test.c). */
#ifndef BC250_TPM_ENGINE_TEST_HOST_H
#define BC250_TPM_ENGINE_TEST_HOST_H
#include <stddef.h>
#include "tpm_engine.h"

/* One line of test output on stdout. */
void host_print(const char *format, ...) __attribute__((format(printf, 1, 2)));
/* The performance counter in microseconds. */
unsigned long long host_us(void);
/* The platform of the engine: an arena of VirtualAlloc, the performance counter, BCryptGenRandom. */
const struct tpm_engine_platform *host_platform(void);
/* Fills the unused part of the test stack with a pattern; host_stack_used then gives the deepest use since. */
void host_stack_paint(void);
size_t host_stack_used(void);

/* The test. mutation is NULL, or the name of a check to break on purpose (engine_test.c). Returns 0 when every
 * check passed. */
int engine_test_run(const char *mutation);

#endif
