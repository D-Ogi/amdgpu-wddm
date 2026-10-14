// Durable identity-bound backup; caller serializes the platform transaction.
#pragma once
#include "board_memory_service.h"
#include "board_memory_format.h"
void BoardMemoryStoreIo(BC250_DEVICE* device, struct board_memory_store* out);
// Called only by the SYSTEM migration initializer after validating its provenance.
// Fresh is explicit proof of no legacy backup, not an inaccessible/missing export.
// Existing durable files are checked, never replaced. This performs no board I/O.
NTSTATUS BoardMemoryStoreSeed(BC250_DEVICE* device, const BOARD_STORE_RECORD* legacy, BOOLEAN fresh);
NTSTATUS DriverStateSeedBoard(BC250_DEVICE* device);
