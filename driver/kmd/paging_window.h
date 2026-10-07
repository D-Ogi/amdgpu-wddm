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
// The OS aperture size is a start-time value since 0.7.216.8 (ApertureSegmentMegabytes, wddm.c), in whole MiB.
//   MIN     256 MiB, the only size before 0.7.216.8. The setting's bisect value: it gives that driver's
//           aperture byte for byte.
//   DEFAULT 384 MiB, what an absent setting gets.
//   MAX     447 MiB, the largest whole MiB that fits the 512 MiB GART bc250_gmc.c builds (gmc_v10_0_mc_init's
//           default) after the private GTT and the paging window: 512 MiB - 64 MiB - 8 KiB = 447.99 MiB. Its
//           PTE slice ends at 131088 + 447*256*8 = 1046544 bytes, inside the 1 MiB table gart.c gives the GART.
//           A larger aperture needs a larger GART first, and that needs the VRAM reservation table re-cut
//           (gart.c refuses a table past BC250_GART_SCRATCH_OFFSET).
// PagingApertureInit still checks the real GART: these bounds only say which sizes a setting may ask for.
#define PAGING_APERTURE_MIB (1ull << 20)
#define PAGING_APERTURE_MIN_BYTES (256ull * PAGING_APERTURE_MIB)
#define PAGING_APERTURE_DEFAULT_BYTES (384ull * PAGING_APERTURE_MIB)
#define PAGING_APERTURE_MAX_BYTES (447ull * PAGING_APERTURE_MIB)
typedef struct { unsigned long long mc, table, bytes; } PAGING_APERTURE;
// 1 when Bytes is a size an aperture may have: whole MiB inside [MIN, MAX].
int PagingApertureBytesValid(unsigned long long bytes);
// The setting in MiB to a size. A value outside [MIN, MAX] in MiB is clamped to the nearer bound and *clamped
// says so (it may be NULL); 0 is not "off", it is below MIN and becomes MIN.
unsigned long long PagingApertureBytesForSetting(unsigned long megabytes, int* clamped);
int PagingApertureInit(unsigned long long start, unsigned long long size,
                       unsigned long long table, unsigned long long tableBytes,
                       unsigned long long bytes, PAGING_APERTURE* out);
int PagingApertureRange(const PAGING_APERTURE* aperture,
                        unsigned long long firstPage, unsigned long long pageCount,
                        unsigned long long* mc, unsigned long long* table);
