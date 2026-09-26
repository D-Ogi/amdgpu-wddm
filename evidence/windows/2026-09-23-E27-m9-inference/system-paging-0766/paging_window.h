// Shared aperture partition: driver allocations below64MiB, paging two-page window next.
#pragma once
#define PAGING_DRIVER_GTT_LIMIT 0x4000000ull
#define PAGING_WINDOW_BYTES 8192ull
#define PAGING_SYSTEM_ADDRESS (1ull << 63)
typedef struct { unsigned long long mc, table; } PAGING_WINDOW;
int PagingWindowInit(unsigned long long start, unsigned long long size,
                     unsigned long long table, unsigned long long tableBytes,
                     PAGING_WINDOW* out);
