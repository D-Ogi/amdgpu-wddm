/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * What core-unlock does at one start, as a pure function of four observations (docs/design/efi-core-unlock.md,
 * "The decision table"). No firmware call, no time, no state: test/host_test.c runs it with every combination.
 */
#ifndef CU_DECIDE_H
#define CU_DECIDE_H

/* The loop guard: a marker file next to the application on the EFI System Partition. */
enum cu_marker {
	CU_MARKER_NONE = 0,	/* no file */
	CU_MARKER_ATTEMPT = 1,	/* written before the reset of an attempt */
	CU_MARKER_FAILED = 2,	/* an attempt ended with 12 logical processors */
	CU_MARKER_UNKNOWN = 3,	/* a file with other content: treated as a failed attempt */
	CU_MARKER_ERROR = 4	/* the file system did not answer: nothing is done */
};

struct cu_inputs {
	int board_ok;		/* the PCI function and SMBIOS identify a BC-250 (BoardIdentitySelect) */
	unsigned processors;	/* logical processors this start, 0 when not known */
	enum cu_marker marker;
	int key_pressed;	/* the operator asked to skip */
};

enum cu_action {
	CU_ACT_BOOT = 0,	/* start Windows and change nothing */
	CU_ACT_CLEAR = 1,	/* the cores run: remove the marker, then start Windows */
	CU_ACT_MARK_FAILED = 2,	/* the attempt did not give the cores: mark it failed, then start Windows */
	CU_ACT_UNLOCK = 3	/* write the marker, send the message, reset */
};

/* Why: one value for each row of the decision table, for the console line. */
enum cu_reason {
	CU_WHY_NOT_BOARD = 0,
	CU_WHY_COUNT_UNKNOWN = 1,	/* neither 12 nor 16 logical processors */
	CU_WHY_FULL = 2,		/* 16, no marker */
	CU_WHY_FULL_AFTER = 3,		/* 16, with a marker */
	CU_WHY_MARKER_ERROR = 4,
	CU_WHY_ATTEMPT_FAILED = 5,	/* 12 after an attempt */
	CU_WHY_EARLIER_FAILED = 6,	/* 12, and an attempt failed before */
	CU_WHY_KEY = 7,
	CU_WHY_STOCK = 8,		/* 12, no marker: the unlock */
	CU_WHY_COUNT
};

struct cu_decision { enum cu_action action; enum cu_reason reason; };

struct cu_decision cu_decide(const struct cu_inputs *in);

/* The marker file's content for a state, and the state of a content. Plain ASCII, one line. */
const char *cu_marker_text(enum cu_marker state);
enum cu_marker cu_marker_parse(const unsigned char *data, unsigned long bytes);

#endif
