/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* <setjmp.h> of the stage C runtime: the crt_setjmp and crt_longjmp of crt.h. */
#ifndef CRT_SETJMP_H
#define CRT_SETJMP_H
#include "../crt.h"

typedef crt_jmp_buf jmp_buf[1];
#define setjmp(b) crt_setjmp(&(b)[0])
#define longjmp(b, v) crt_longjmp(&(b)[0], (v))

#endif
