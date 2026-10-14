#pragma once
#include <ntddk.h>
// The fixed, SYSTEM-owned file is an installer handoff, not a runtime override.
// Absence is distinct from inaccessible, replaced, malformed or truncated data.
NTSTATUS DriverStateReadHandoff(PVOID *buffer,ULONG *bytes);
void DriverStateFreeHandoff(PVOID buffer);
