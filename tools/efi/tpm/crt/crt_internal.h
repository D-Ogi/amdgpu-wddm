/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The parts of the stage C runtime that only its own files use. */
#ifndef CRT_INTERNAL_H
#define CRT_INTERNAL_H
#include <stdarg.h>
#include <stddef.h>

int crt_alloc_init(void *arena, size_t bytes);
void errno_set_nomem(void);
/* Text to the log sink, as it is (no prefix, no line end added). */
void crt_log_write(const char *text, size_t bytes);
/* Formats into a local buffer and writes the result to the log sink. */
int crt_log_vformat(const char *format, va_list args);

#endif
