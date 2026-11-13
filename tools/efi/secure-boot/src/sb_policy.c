/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The rule for the inputs of sb.mode=enforce (sb_policy.h). */
#include <stddef.h>
#include "sb_policy.h"

const char *sb_enforce_defect(const struct sb_inputs *inputs)
{
	/* The files are the operator's statement. Both must be there: a dbx.esl that is absent can be a copy that
	 * failed, and a file with an empty list says "nothing is forbidden" on purpose. */
	if (inputs->db_file == SB_SOURCE_ABSENT) return "db.esl is not beside the stage";
	if (inputs->db_file == SB_SOURCE_BROKEN) return "db.esl did not read or is not a well-formed signature database";
	if (inputs->dbx_file == SB_SOURCE_ABSENT) return "dbx.esl is not beside the stage";
	if (inputs->dbx_file == SB_SOURCE_BROKEN) return "dbx.esl did not read or is not a well-formed signature database";
	/* A firmware without the variables is the BC-250. A variable that is there must read. */
	if (inputs->db_firmware == SB_SOURCE_BROKEN) return "the db variable of the firmware did not read";
	if (inputs->dbx_firmware == SB_SOURCE_BROKEN) return "the dbx variable of the firmware did not read";
	/* An entry that the stage cannot read can forbid the image. */
	if (inputs->dbx_unread != 0) return "dbx has entries of a type that this stage does not read";
	return NULL;
}
