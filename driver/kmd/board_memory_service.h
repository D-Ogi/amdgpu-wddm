// Portable transaction orchestration. Storage callbacks acknowledge flush plus read-back.
#pragma once
#include "bc250_uma.h"
struct board_memory_state {
    unsigned ready, blocked, backup_valid;
    int result;
    unsigned char block[28], backup[28];
};
struct board_memory_store {
    void* context;
    // 1 found, 0 absent, -1 invalid/unavailable. Never replace an existing backup.
    int (*load)(void*, unsigned char backup[28], unsigned* pending);
    int (*save)(void*, const unsigned char backup[28], unsigned pending);
};
void BoardMemoryServiceStart(struct board_memory_state*, const struct bc250_uma_io*, const struct board_memory_store*);
int BoardMemoryServiceChange(struct board_memory_state*, const struct bc250_uma_io*, const struct board_memory_store*,
                             const unsigned char expected[28], unsigned target, int restore);
