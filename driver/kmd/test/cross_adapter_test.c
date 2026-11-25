/* Host control of cross_adapter.h: from which WDDM level the adapter reports
 * DXGK_DRIVERCAPS.MemoryManagementCaps.CrossAdapterResource, and which answer DxgkDdiDescribeAllocation gives.
 *
 * The cases: the cap is clear at 2.0, 2.1 and 2.2 and set at every level of the set from 2.4 (the default 2.9
 * among them); an LB7A allocation and a BC2A scan-out request keep the surface answer whatever their size; a BC2A
 * allocation without scan-out is a buffer from 1 byte to 4 GiB - 1, and is refused at 0 bytes and above 4 GiB - 1,
 * because the answer's Width is 32 bits. Two negative controls of the runner change one rule each in a copy of the
 * header and must fail this program.
 */
#include <stdio.h>
#include "cross_adapter.h"

#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)

int main(void)
{
    static const unsigned long long sizes[] = { 0ull, 1ull, 4096ull, 65536ull, 0x80000000ull, 0xFFFFFFFFull,
                                                0x100000000ull, 0xFFFFFFFFFFFFF000ull };
    unsigned long checks = 0, failures = 0, i;
    int umd, scanout;

    /* ---- the cap, at every level of the set ------------------------------------------------------------------ */
    for (i = 0; i < BC250_WDDM_LEVEL_SET_COUNT; i++)
        CHECK(Bc250CrossAdapterCap(Bc250WddmLevels[i]) == (Bc250WddmLevels[i] >= BC250_WDDM_LEVEL_2_4));
    CHECK(!Bc250CrossAdapterCap(BC250_WDDM_LEVEL_2_0));
    CHECK(!Bc250CrossAdapterCap(BC250_WDDM_LEVEL_2_2));
    CHECK(Bc250CrossAdapterCap(BC250_WDDM_LEVEL_2_4));
    CHECK(Bc250CrossAdapterCap(BC250_WDDM_LEVEL_DEFAULT));
    CHECK(Bc250CrossAdapterCap(BC250_WDDM_LEVEL_MAX));

    /* ---- the describe route: every kind at every size -------------------------------------------------------- */
    for (umd = 0; umd < 2; umd++)
        for (scanout = 0; scanout < 2; scanout++)
            for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
                enum bc250_describe_route r = Bc250DescribeRoute(umd, scanout, sizes[i]);
                if (!umd || scanout) CHECK(r == BC250_DESCRIBE_SURFACE);
                else if (sizes[i] == 0 || sizes[i] > 0xFFFFFFFFull) CHECK(r == BC250_DESCRIBE_REFUSED);
                else CHECK(r == BC250_DESCRIBE_BUFFER);
            }
    /* The heap of the D3D12 probe: 64 KiB, a BC2A allocation without scan-out. */
    CHECK(Bc250DescribeRoute(1, 0, 65536ull) == BC250_DESCRIBE_BUFFER);
    /* The largest Width: 4 GiB - 1 is a buffer, one byte more is refused, never truncated. */
    CHECK(Bc250DescribeRoute(1, 0, 0xFFFFFFFFull) == BC250_DESCRIBE_BUFFER);
    CHECK(Bc250DescribeRoute(1, 0, 0x100000000ull) == BC250_DESCRIBE_REFUSED);
    /* A primary keeps the surface answer. */
    CHECK(Bc250DescribeRoute(0, 0, 8294400ull) == BC250_DESCRIBE_SURFACE);
    CHECK(Bc250DescribeRoute(1, 1, 8294400ull) == BC250_DESCRIBE_SURFACE);

    printf("cross_adapter: %lu checks, %lu failures\n", checks, failures);
    return failures ? 1 : 0;
}
