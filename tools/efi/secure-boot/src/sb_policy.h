/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The rule for the inputs of sb.mode=enforce. Pure code, so the host tests run it.
 *
 * The enforce mode fails closed on its inputs: when the rule finds a defect, the module refuses the next image and
 * prints the reason. It does not fall back to the audit mode, because an operator who asked for a refusal must not
 * get a start that looks checked.
 */
#ifndef SB_POLICY_H
#define SB_POLICY_H

/* What one source of a database gave. */
enum sb_source {
	SB_SOURCE_BROKEN = -1,	/* it is there and did not read, or is not a well-formed signature database */
	SB_SOURCE_ABSENT = 0,
	SB_SOURCE_GOOD = 1
};

struct sb_inputs {
	enum sb_source db_file, dbx_file;		/* db.esl and dbx.esl beside the stage */
	enum sb_source db_firmware, dbx_firmware;	/* the db and dbx variables of the firmware, read only */
	unsigned dbx_unread;	/* the entries of dbx, from both sources, whose type the stage does not read */
};

/* NULL when the enforce mode can run with these inputs, else the reason as one console line. */
const char *sb_enforce_defect(const struct sb_inputs *inputs);

#endif
