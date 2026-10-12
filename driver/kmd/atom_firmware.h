/* Caller: unpublished StartDevice or the display owner at PASSIVE_LEVEL.
 * Stop/D3 must drain all users before release. This API performs no MMIO or PCI writes. */
#pragma once
#include "../shim/bc250_atom_firmware.h"
struct _BC250_DEVICE;
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AtomFirmwareCapture(struct _BC250_DEVICE *Device, BC250_ATOM_FIRMWARE *Firmware,
                            BC250_ATOM_STATUS *Reason);
_IRQL_requires_(PASSIVE_LEVEL)
void AtomFirmwareRelease(BC250_ATOM_FIRMWARE *Firmware);
