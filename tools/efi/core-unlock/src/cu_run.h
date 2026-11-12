/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * One start of core-unlock: the observations, the decision (cu_decide.h) and the steps of the action, over a table
 * of operations. src/main.c gives the UEFI operations, test/host_test.c gives recorded ones, so the order of the
 * steps is tested on the host: the marker before the message, the message before the reset, and no reset after any
 * failure. Every path returns, and the caller then starts Windows.
 */
#ifndef CU_RUN_H
#define CU_RUN_H
#include "cu_decide.h"

struct cu_ops {
	void *context;
	int (*board)(void *context);				/* 1 when the board is a BC-250 */
	unsigned (*processors)(void *context);			/* logical processors, 0 when not known */
	enum cu_marker (*marker_read)(void *context);
	int (*marker_write)(void *context, enum cu_marker state);	/* writes, then reads back; 0 is success */
	int (*marker_delete)(void *context);			/* 0 is success, also when there is no file */
	int (*key_window)(void *context);			/* 1 when the operator pressed a key in the window */
	int (*unlock)(void *context);				/* 0 when the firmware accepted the message */
	int (*reset)(void *context);				/* returns only when the reset did not happen */
	void (*say)(void *context, const char *line);
};

enum cu_outcome {
	CU_OUT_NOTHING = 0,	/* nothing was sent and nothing was changed */
	CU_OUT_CLEARED = 1,	/* the marker of a successful attempt was removed */
	CU_OUT_MARKED_FAILED = 2,
	CU_OUT_SENT = 3,	/* the message was accepted and no reset was asked for */
	CU_OUT_FAILED = 4	/* a step of the unlock failed: Windows starts with the cores it has */
};

/* resets: 0 for CU_RESET_NONE, which sends the message, writes no marker and does not reset. */
enum cu_outcome cu_run(const struct cu_ops *ops, int resets);

#endif
