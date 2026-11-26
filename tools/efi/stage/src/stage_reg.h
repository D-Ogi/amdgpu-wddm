/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The register BAR (BAR5) of the GPU function through the firmware: the binding of stage_bar.h. A module that reads
 * or writes a register identifies the BAR first. The identification reads only: the configuration header that the
 * core kept, the resource descriptors of the firmware, and one register of known value.
 */
#ifndef STAGE_REG_H
#define STAGE_REG_H
#include "stage.h"

/* 1 when both proofs hold. The console lines have the prefix of module. The result stays in the stage
 * (reg_identified, reg_choice, reg_bridge) until the next identification. */
int stage_reg_identify(struct stage *stage, const char *module);

/* One dword of BAR5 at a byte offset inside BC250_BAR5_LENGTH, through the identified path. 0 on success, -1 when
 * the BAR is not identified, the offset is out of range or the firmware refuses the access. */
int stage_reg_read32(struct stage *stage, unsigned offset, unsigned *value);
int stage_reg_write32(struct stage *stage, unsigned offset, unsigned value);

#endif
