// Cached firmware actually loaded for the current device session. No I/O in queries.
#pragma once
#include "../contract/bc250_umd_firmware.h"
// PASSIVE_LEVEL; takes GartLock only for the cached PSP snapshot. Not ready
// before a successful complete load or after unload/stop/quarantine.
NTSTATUS PspReadFirmware(BC250_DEVICE* Device, struct bc250_umd_firmware* Firmware);
