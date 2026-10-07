// Host test for the start-time OS aperture size (memory manager stage 1a, 0.7.216.8): the setting's clamp,
// PagingApertureInit and PagingApertureRange at every admitted size inside the 512 MiB GART that bc250_gmc.c
// builds, and the logical aperture state (paging_aperture_state.c) at a size other than 256 MiB.
// The 256 MiB results are the ones the fixed-size version produced (experiments/E27-m9-inference
// paging-route-test-suffix.c case_aperture_partition), so the bisect value is checked byte for byte.
#include <stdio.h>
#include <stdlib.h>
#include "../paging_aperture_state.h"

static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); return 1; } } while (0)

#define MIB (1ull << 20)
#define GART_BYTES (512ull << 20)        /* bc250_gmc.c: adev->gmc.gart_size = 512ULL << 20 */
#define TABLE_BYTES (1ull << 20)         /* bc250_gmc.c: gart_size / 4096 * 8; gart.c: first MB of the window */

static int sized(unsigned long long bytes)
{
    const unsigned long long start = 0x300000000ull, root = 0x400000000ull, limit = 0x1000000000000ull;
    const unsigned long long pages = bytes / 4096;
    PAGING_APERTURE a, bad;
    unsigned long long mc, table;
    CHECK(PagingApertureInit(start, GART_BYTES, root, TABLE_BYTES, bytes, &a));
    // The aperture starts where it always did; only its end moves with the size.
    CHECK(a.mc == start + 67117056ull && a.table == root + 131088ull && a.bytes == bytes);
    CHECK(a.mc + a.bytes <= start + GART_BYTES && a.table + pages * 8 <= root + TABLE_BYTES);
    CHECK(PagingApertureRange(&a, 0, pages, &mc, &table) && mc == a.mc && table == a.table);
    CHECK(PagingApertureRange(&a, pages - 1, 1, &mc, &table) &&
          mc == a.mc + (pages - 1) * 4096 && table == a.table + (pages - 1) * 8);
    CHECK(!PagingApertureRange(&a, pages - 1, 2, &mc, &table) && !mc && !table);
    CHECK(!PagingApertureRange(&a, pages, 1, &mc, &table) && !mc && !table);
    CHECK(!PagingApertureRange(&a, 0, 0, &mc, &table) && !mc && !table);
    // A GART one page short of this size, or a table one entry short, advertises nothing at all.
    CHECK(!PagingApertureInit(start, 67117056ull + bytes - 4096, root, TABLE_BYTES, bytes, &bad) &&
          !bad.mc && !bad.table && !bad.bytes);
    CHECK(PagingApertureInit(start, 67117056ull + bytes, root, TABLE_BYTES, bytes, &bad));
    CHECK(!PagingApertureInit(start, GART_BYTES, root, 131088ull + pages * 8 - 8, bytes, &bad) && !bad.bytes);
    CHECK(PagingApertureInit(start, GART_BYTES, root, 131088ull + pages * 8, bytes, &bad));
    // 48-bit exclusive ends, as the fixed version checked them.
    CHECK(PagingApertureInit(limit - 67117056ull - bytes, 67117056ull + bytes, limit - 131088ull - pages * 8,
                             131088ull + pages * 8, bytes, &bad) &&
          bad.mc + bad.bytes == limit && bad.table + pages * 8 == limit);
    CHECK(!PagingApertureInit(limit - 67117056ull - bytes + 4096, 67117056ull + bytes, root, TABLE_BYTES, bytes, &bad) &&
          !bad.bytes);
    CHECK(!PagingApertureInit(start + 1, GART_BYTES, root, TABLE_BYTES, bytes, &bad) && !bad.bytes);
    CHECK(!PagingApertureInit(start, GART_BYTES, root + 1, TABLE_BYTES, bytes, &bad) && !bad.bytes);
    // An extent whose size was changed after Init is refused, never resized.
    bad = a; bad.bytes = bytes + 4096;
    CHECK(!PagingApertureRange(&bad, 0, 1, &mc, &table) && !mc && !table);
    return 0;
}

static int state_at(unsigned long long bytes)
{
    PAGING_APERTURE a;
    PAGING_APERTURE_STATE s = {0};
    const unsigned pages = (unsigned)(bytes / 4096);
    unsigned long long* storage = (unsigned long long*)calloc(pages, sizeof(unsigned long long));
    unsigned long long batch[PAGING_APERTURE_BATCH_PAGES], physical;
    unsigned i;
    CHECK(storage != NULL);
    CHECK(PagingApertureInit(0x300000000ull, GART_BYTES, 0x400000000ull, TABLE_BYTES, bytes, &a));
    CHECK(!PagingApertureStateInit(&s, &a, storage, pages - 1));     // storage for 256 MiB cannot hold more
    CHECK(PagingApertureStateInit(&s, &a, storage, pages) && s.count == pages);
    for (i = 0; i < PAGING_APERTURE_BATCH_PAGES; i++) batch[i] = (unsigned long long)(i * 7 + 1) * 4096;
    // The last batch of the larger aperture maps and resolves; one page past it is refused.
    CHECK(PagingApertureStateMap(&s, pages - PAGING_APERTURE_BATCH_PAGES, PAGING_APERTURE_BATCH_PAGES, batch, ~4095ull));
    for (i = 0; i < PAGING_APERTURE_BATCH_PAGES; i++)
        CHECK(PagingApertureStateResolve(&s, a.mc + (unsigned long long)(pages - PAGING_APERTURE_BATCH_PAGES + i) * 4096,
                                         4096, &physical) && physical == batch[i]);
    CHECK(!PagingApertureStateMap(&s, pages - PAGING_APERTURE_BATCH_PAGES + 1, PAGING_APERTURE_BATCH_PAGES, batch, ~4095ull));
    CHECK(!PagingApertureStateResolve(&s, a.mc + a.bytes, 4, &physical) && !physical);
    CHECK(PagingApertureStateUnmap(&s, pages - 1, 1));
    CHECK(!PagingApertureStateResolve(&s, a.mc + a.bytes - 4096, 4, &physical));
    free(storage);
    return 0;
}

int main(void)
{
    int clamped = -1;
    PAGING_APERTURE a;
    // volatile: the constants are what is under test here, and C4127 would otherwise refuse the checks.
    volatile unsigned long long defaultBytes = PAGING_APERTURE_DEFAULT_BYTES, maxBytes = PAGING_APERTURE_MAX_BYTES;
    // The setting: MiB in, bytes out, clamped to [256, 447] and saying so.
    CHECK(PagingApertureBytesForSetting(384, &clamped) == 0x18000000ull && clamped == 0);
    CHECK(defaultBytes == 0x18000000ull);
    CHECK(PagingApertureBytesForSetting(256, &clamped) == 0x10000000ull && clamped == 0);
    CHECK(PagingApertureBytesForSetting(447, &clamped) == 447 * MIB && clamped == 0);
    CHECK(PagingApertureBytesForSetting(448, &clamped) == 447 * MIB && clamped == 1);
    CHECK(PagingApertureBytesForSetting(255, &clamped) == 256 * MIB && clamped == 1);
    CHECK(PagingApertureBytesForSetting(0, &clamped) == 256 * MIB && clamped == 1);
    CHECK(PagingApertureBytesForSetting(0xFFFFFFFFul, &clamped) == 447 * MIB && clamped == 1);
    CHECK(PagingApertureBytesForSetting(300, NULL) == 300 * MIB);
    CHECK(PagingApertureBytesValid(256 * MIB) && PagingApertureBytesValid(447 * MIB) && PagingApertureBytesValid(384 * MIB));
    CHECK(!PagingApertureBytesValid(0) && !PagingApertureBytesValid(255 * MIB) && !PagingApertureBytesValid(448 * MIB));
    CHECK(!PagingApertureBytesValid(384 * MIB + 4096));                // whole MiB only
    // MAX is the largest whole MiB the real GART holds: 448 MiB does not fit it, 447 does.
    CHECK(!PagingApertureInit(0x300000000ull, GART_BYTES, 0x400000000ull, TABLE_BYTES, 448 * MIB, &a) && !a.bytes);
    CHECK(67117056ull + maxBytes <= GART_BYTES && 67117056ull + maxBytes + MIB > GART_BYTES);
    CHECK(131088ull + (maxBytes / 4096) * 8 <= TABLE_BYTES);
    CHECK(!PagingApertureInit(0x300000000ull, GART_BYTES, 0x400000000ull, TABLE_BYTES, 0, &a) && !a.bytes);
    CHECK(!PagingApertureInit(0x300000000ull, GART_BYTES, 0x400000000ull, TABLE_BYTES, 384 * MIB, NULL));
    if (sized(256 * MIB) || sized(384 * MIB) || sized(447 * MIB) || sized(300 * MIB)) return 1;
    if (state_at(384 * MIB) || state_at(256 * MIB)) return 1;
    printf("PASS: %u checks - aperture size setting clamp, Init/Range at 256/300/384/447 MiB in the 512 MiB GART, "
           "448 refused, logical state at 256 and 384 MiB\n", checks);
    return 0;
}
