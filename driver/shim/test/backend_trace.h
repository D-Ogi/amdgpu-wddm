/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* Host replay backend for the shim: see backend_trace.c. */
#ifndef BC250_BACKEND_TRACE_H
#define BC250_BACKEND_TRACE_H

#include "amdgpu.h"

struct bc250_reg_write {
	u32 byte_offset;
	u32 value;
};

/* Load one `NAME 0xOFFSET VALUE` sweep log into the register state. First occurrence of an offset
 * wins, across calls as well, so load the logs in the order they were taken. Returns the number of
 * offsets this call contributed, or -1 if the file could not be read. */
int backend_load_sweep(const char *path);

/* The recorded writes, oldest first. */
const struct bc250_reg_write *backend_writes(void);
unsigned int backend_write_count(void);
void backend_reset_writes(void);

/* Reads that hit an offset no sweep had a value for. Should stay 0: a non-zero count means the
 * replay was answering with an invented value. */
unsigned int backend_unknown_reads(void);
u32 backend_first_unknown_read(void);

/* Drop every written value, so the next run reads the firmware state again. */
void backend_reset_state(void);

/* 0 silences the imports' dev_err/dev_info; 1 prints them. */
void backend_set_verbose(int on);

#endif /* BC250_BACKEND_TRACE_H */
