// Durable identity-bound backup; caller serializes the platform transaction.
#pragma once
#include "board_memory_service.h"
void BoardMemoryStoreIo(BC250_DEVICE* device, struct board_memory_store* out);
