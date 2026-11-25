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
// Bit 31 of the stored ApertureSegmentMegabytes value asks for 64 KiB pages in host memory (vidmm.c, wddm.c
// WddmGpuMmuCaps). It is a bit of this setting, and not a setting of its own, because a new name changes the
// state catalog, and a KMD starts only on the catalog it compiles (state_kernel.c): an experiment build must stay
// a plain image swap on an installed release. The setting already describes the host-memory segment, takes any
// 32-bit value, and no size reaches bit 31 (447 is the largest). Stored 0x80000180 = 384 MiB with 64 KiB pages.
#define PAGING_APERTURE_SETTING_PAGES_64K 0x80000000ul
// The stored value as its two parts. Returns the size part in MiB for PagingApertureBytesForSetting, and
// *pages64k is 1 when bit 31 is set (it may be NULL). A value with the bit and no size, 0x80000000, keeps the
// default size, so that the bit alone does not also move the aperture to its minimum.
unsigned long PagingApertureSettingMegabytes(unsigned long stored, int* pages64k);
// 1 when the byte range [offset, offset + bytes) of a fill or a transfer lies inside an allocation of size bytes
// and is not empty. Size is the size VidMm was given for the allocation (DXGK_ALLOCATIONINFO::Size): VidMm fills
// and transfers in that size, which is the size the user-mode driver asked for rounded up to pages, and to
// 64 KiB when the allocation was sized for 64 KiB host pages.
int PagingRangeInAllocation(unsigned long long size, unsigned long long offset, unsigned long long bytes);
int PagingApertureInit(unsigned long long start, unsigned long long size,
                       unsigned long long table, unsigned long long tableBytes,
                       unsigned long long bytes, PAGING_APERTURE* out);
int PagingApertureRange(const PAGING_APERTURE* aperture,
                        unsigned long long firstPage, unsigned long long pageCount,
                        unsigned long long* mc, unsigned long long* table);
