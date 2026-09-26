// Shared aperture partition: driver allocations below64MiB, paging two-page window next.
#pragma once
#define PAGING_DRIVER_GTT_LIMIT 0x4000000ull
#define PAGING_WINDOW_BYTES 8192ull
#define PAGING_SYSTEM_ADDRESS (1ull << 63)
typedef struct { unsigned long long mc, table; } PAGING_WINDOW;
int PagingWindowInit(unsigned long long start, unsigned long long size,
                     unsigned long long table, unsigned long long tableBytes,
                     PAGING_WINDOW* out);


// Permanent OS mappings must not overlap driver GTT or temporary paging views.
// These are offsets/sizes within the hardware GART, not register addresses.
#define PAGING_APERTURE_OFFSET (PAGING_DRIVER_GTT_LIMIT + PAGING_WINDOW_BYTES)
#define PAGING_APERTURE_BYTES 0x10000000ull
typedef struct { unsigned long long mc, table, bytes; } PAGING_APERTURE;
int PagingApertureInit(unsigned long long start, unsigned long long size,
                       unsigned long long table, unsigned long long tableBytes,
                       PAGING_APERTURE* out);
int PagingApertureRange(const PAGING_APERTURE* aperture,
                        unsigned long long firstPage, unsigned long long pageCount,
                        unsigned long long* mc, unsigned long long* table);
