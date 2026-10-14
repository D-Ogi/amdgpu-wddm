/* One numeric namespace for the adapter's B250 private escapes, including the
 * HIP journal header shared with user mode. Duplicate case labels are a build
 * error in both flavours. The inline function has no runtime work. */
#ifndef BC250_COMMAND_NAMESPACE_H
#define BC250_COMMAND_NAMESPACE_H
#include "bc250kmd_escape.h"
#include "wave_snapshot.h"
#include "../shim/include/bc250_hip_journal.h"

static __inline void Bc250EscapeCommandsUnique(void)
{
    switch (0u) {
    case BC250_ESCAPE_GET_INFO:
    case BC250_ESCAPE_READ_REG:
    case BC250_ESCAPE_WRITE_REG:
    case BC250_ESCAPE_GET_MEMORY:
    case BC250_ESCAPE_VRAM_READ:
    case BC250_ESCAPE_VRAM_WRITE:
    case BC250_ESCAPE_RUN_GART:
    case BC250_ESCAPE_RUN_PSP:
    case BC250_ESCAPE_RUN_GFX:
    case BC250_ESCAPE_RUN_IH:
    case BC250_ESCAPE_RUN_FENCE:
    case BC250_ESCAPE_GET_LOG:
    case BC250_ESCAPE_LOG_SUMMARY:
    case BC250_ESCAPE_RUN_DCN:
    case BC250_ESCAPE_RUN_DCNFLIP:
    case BC250_ESCAPE_RUN_SDMACOPY:
    case BC250_ESCAPE_RUN_FBDUMP:
    case BC250_ESCAPE_RUN_SDMAIB:
    case BC250_ESCAPE_RUN_CLOCK:
    case BC250_ESCAPE_OBSERVE_DCN:
    case BC250_ESCAPE_RUN_START_HEALTH:
    case BC250_ESCAPE_RUN_CU_MODE:
    case BC250_ESCAPE_RUN_DPM:
    case BC250_ESCAPE_GET_PAGING_JOURNAL:
    case BC250_ESCAPE_RUN_INTEROP:
    case BC250_ESCAPE_RUN_DPM_TUNE:
    case BC250_ESCAPE_RUN_HWMON:
    case BC250_ESCAPE_RUN_DPM_CURVE:
    case BC250_ESCAPE_RUN_CPU:
    case BC250_ESCAPE_RUN_FAN:
    case BC250_ESCAPE_RUN_DPAUDIO:
    case BC250_ESCAPE_RUN_BOARD_MEMORY:
    case BC250_ESCAPE_RUN_BOARD_CAPS:
    case BC250_ESCAPE_GET_WAVES:
    case BC250_HIP_JOURNAL_COMMAND:
    case BC250_ESCAPE_CONNECTOR_REPLUG:
    default:
        break;
    }
}
#endif
