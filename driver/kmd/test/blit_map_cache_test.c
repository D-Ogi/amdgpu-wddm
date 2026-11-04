// BD-289 second form: the kept mappings of the CPU present blit (driver/kmd/blit_map_cache.h).
//
// The model of the system PTE pool: a VA range of 4 KiB units, given out first fit, a 2 MiB aligned run for a
// mapping that gets large pages (a 2 MiB aligned physical range of 2 MiB or more, as MmMapIoSpaceEx does for the
// primary). Other drivers map and unmap ranges of their own between presents. For each 2 MiB region of the pool the
// model records how this driver last mapped it: as a large page or with 4 KiB pages. A change of that kind is a
// "kind flip": the PDE of the region goes from a large page to a fresh page table or back, which is the state that
// the two dumps show under the source mapping. The test proves that the kept mappings remove this churn from the
// blit path; it does not prove that the churn caused the fault (that needs the lab).
//
//   blit_map_cache_test.exe                the kept-mapping rule; must give no kind flip and no fault
//   blit_map_cache_test.exe --per-present  the tester.36 shape (map and unmap per present); must give kind flips
#include <stdio.h>
#include <string.h>
#include "blit_map_cache.h"

#define UNIT        0x1000ull
#define LARGE       0x200000ull
#define POOL_UNITS  (0x40000000ull / UNIT)          // 1 GiB of VA
#define REGIONS     (POOL_UNITS * UNIT / LARGE)
#define FAKE_BASE   0x10000000000ull

static unsigned char g_Used[POOL_UNITS];            // 0 free, 1 ours, 2 foreign
static unsigned char g_Kind[REGIONS];               // 0 never ours, 1 ours as large pages, 2 ours as 4 KiB pages
static long g_Flips, g_Maps, g_Unmaps, g_Faults, g_Live, g_FailNext, g_Fails;
static int g_Failed;

#define CHECK(c) do { if (!(c)) { printf("CHECK failed at line %d: %s\n", __LINE__, #c); g_Failed = 1; } } while (0)

static unsigned long long g_Seed = 0x289;
static unsigned Rand(unsigned n) { g_Seed = g_Seed * 6364136223846793005ull + 1442695040888963407ull; return (unsigned)((g_Seed >> 33) % n); }

static int IsLarge(unsigned long long Physical, size_t Length) { return Physical % LARGE == 0 && Length >= LARGE; }

// First fit; Align in units. Returns the first unit or -1.
static long long Take(unsigned long long Units, unsigned long long Align, unsigned char Owner)
{
    unsigned long long u, k;
    for (u = 0; u + Units <= POOL_UNITS; u += Align)
    {
        for (k = 0; k < Units && !g_Used[u + k]; k++) {}
        if (k == Units) { memset(&g_Used[u], Owner, (size_t)Units); return (long long)u; }
    }
    return -1;
}

static unsigned long long Units(size_t Length) { return ((unsigned long long)Length + UNIT - 1) / UNIT; }

static void NoteKinds(unsigned long long First, size_t Length, int Large)
{
    unsigned long long whole = Large ? (unsigned long long)Length / LARGE : 0, r, end;
    unsigned long long base = First * UNIT / LARGE;
    end = (First * UNIT + (unsigned long long)Length + LARGE - 1) / LARGE;
    for (r = base; r < end && r < REGIONS; r++)
    {
        unsigned char kind = (unsigned char)(r - base < whole ? 1 : 2);
        if (g_Kind[r] != 0 && g_Kind[r] != kind) g_Flips++;
        g_Kind[r] = kind;
    }
}

static void* FakeMap(void* Context, unsigned long long Physical, size_t Length, unsigned long Access)
{
    int large = IsLarge(Physical, Length);
    long long first;
    (void)Context; (void)Access;
    if (g_FailNext) { g_FailNext = 0; g_Fails++; return NULL; }
    first = Take(Units(Length), large ? LARGE / UNIT : 1, 1);
    if (first < 0) return NULL;
    NoteKinds((unsigned long long)first, Length, large);
    g_Maps++; g_Live++;
    return (void*)(FAKE_BASE + (unsigned long long)first * UNIT);
}

static void FakeUnmap(void* Context, void* Mapping, size_t Length)
{
    unsigned long long first = ((unsigned long long)Mapping - FAKE_BASE) / UNIT, k;
    (void)Context;
    for (k = 0; k < Units(Length); k++)
    {
        if (g_Used[first + k] != 1) { g_Faults++; break; }      // releases PTEs this driver does not own
        g_Used[first + k] = 0;
    }
    g_Unmaps++; g_Live--;
}

// Is the whole range still this driver's? A copy through a released VA is the fault of the dumps.
static int Owned(const void* Mapping, size_t Length)
{
    unsigned long long first = ((unsigned long long)Mapping - FAKE_BASE) / UNIT, k;
    for (k = 0; k < Units(Length); k++) if (g_Used[first + k] != 1) return 0;
    return 1;
}

// Other drivers: up to four foreign mappings of 4 KiB to 3 MiB, each kept for a few presents.
static struct { long long First; unsigned long long Units; int Left; } g_Foreign[4];
static void ForeignStep(void)
{
    unsigned i;
    for (i = 0; i < 4; i++)
    {
        if (g_Foreign[i].Units && --g_Foreign[i].Left <= 0)
        {
            memset(&g_Used[g_Foreign[i].First], 0, (size_t)g_Foreign[i].Units);
            g_Foreign[i].Units = 0;
        }
        if (!g_Foreign[i].Units && Rand(3) == 0)
        {
            unsigned long long units = 1 + Rand(768);
            long long first = Take(units, 1, 2);
            if (first >= 0) { g_Foreign[i].First = first; g_Foreign[i].Units = units; g_Foreign[i].Left = 1 + (int)Rand(5); }
        }
    }
}

// The two surfaces of the dumps: a shadow source (not 2 MiB aligned, 4 KiB pages) and the primary (large pages).
#define SRC_PHYS 0x2808ca000ull
#define DST_PHYS 0x280000000ull
#define SURFACE  0x8ca000ull

static const BC250_BLIT_MAP_OPS g_Ops = { NULL, FakeMap, FakeUnmap };

static void PresentKept(BC250_BLIT_MAP_CACHE* Cache, unsigned long long Src, unsigned long long Dst)
{
    void* s = BlitMapGet(Cache, Src, SURFACE, 2, &g_Ops);
    void* d = BlitMapGet(Cache, Dst, SURFACE, 4, &g_Ops);
    CHECK(s != NULL && d != NULL);
    if (s && d) { CHECK(Owned(s, SURFACE)); CHECK(Owned(d, SURFACE)); }    // the destination never evicts the source
    ForeignStep();
}

static void PresentPerPresent(unsigned long long Src, unsigned long long Dst)
{
    void* s = FakeMap(NULL, Src, SURFACE, 2);
    void* d = FakeMap(NULL, Dst, SURFACE, 4);
    if (s && d && (!Owned(s, SURFACE) || !Owned(d, SURFACE))) g_Faults++;
    if (d) FakeUnmap(NULL, d, SURFACE);
    if (s) FakeUnmap(NULL, s, SURFACE);
    ForeignStep();
}

int main(int argc, char** argv)
{
    BC250_BLIT_MAP_CACHE cache;
    int perPresent = argc > 1 && !strcmp(argv[1], "--per-present");
    long i, steadyFlips;
    memset(&cache, 0, sizeof(cache));

    if (perPresent)
    {
        for (i = 0; i < 4000; i++) PresentPerPresent(SRC_PHYS, DST_PHYS);
        printf("per-present: maps %ld unmaps %ld kind-flips %ld faults %ld\n", g_Maps, g_Unmaps, g_Flips, g_Faults);
        // The negative control passes only when the old shape shows the churn.
        return g_Flips > 0 ? 0 : 1;
    }

    // 1. Steady state: two mappings for 4000 presents, and nothing released.
    for (i = 0; i < 4000; i++) PresentKept(&cache, SRC_PHYS, DST_PHYS);
    steadyFlips = g_Flips;
    CHECK(g_Maps == 2 && g_Unmaps == 0 && cache.Hits == 2 * 4000 - 2 && cache.Evictions == 0);
    CHECK(steadyFlips == 0);

    // 2. Window surfaces: eight sources to one primary, more keys than slots. Evictions happen; the source and the
    //    destination of the present are always owned at the copy, and nothing is unmapped twice.
    for (i = 0; i < 4000; i++) PresentKept(&cache, SRC_PHYS + LARGE * 8 * (unsigned long long)(1 + Rand(8)), DST_PHYS);
    CHECK(cache.Evictions > 0 && g_Faults == 0);
    CHECK(g_Live == BC250_BLIT_MAP_SLOTS);

    // 3. A failed map: NULL, counted, the cache stays usable.
    g_FailNext = 1;
    CHECK(BlitMapGet(&cache, 0x300000000ull, SURFACE, 2, &g_Ops) == NULL);
    CHECK(cache.Failures == 1 && g_Fails == 1);
    PresentKept(&cache, SRC_PHYS, DST_PHYS);

    // 4. Forget: the slot goes without an unmap (the VA stays reserved), the next get maps the range again.
    {
        void* s = BlitMapGet(&cache, SRC_PHYS, SURFACE, 2, &g_Ops);
        long unmaps = g_Unmaps, maps = g_Maps;
        CHECK(BlitMapForget(&cache, s) == 1 && cache.Forgotten == 1);
        CHECK(BlitMapForget(&cache, s) == 0);
        CHECK(g_Unmaps == unmaps && Owned(s, SURFACE));
        CHECK(BlitMapGet(&cache, SRC_PHYS, SURFACE, 2, &g_Ops) != s && g_Maps == maps + 1);
        FakeUnmap(NULL, s, SURFACE);                     // the leaked VA, so that the release check below balances
    }

    // 5. Stop: every kept mapping is released once, and the cache is empty.
    BlitMapRelease(&cache, &g_Ops);
    CHECK(g_Live == 0 && g_Faults == 0);
    for (i = 0; i < BC250_BLIT_MAP_SLOTS; i++) CHECK(cache.Slots[i].Mapping == NULL);
    BlitMapRelease(&cache, &g_Ops);                      // twice is harmless
    CHECK(g_Live == 0 && g_Faults == 0);

    // 6. Length 0 is refused without a map.
    CHECK(BlitMapGet(&cache, SRC_PHYS, 0, 2, &g_Ops) == NULL);

    printf("kept: maps %ld unmaps %ld hits %lu evictions %lu kind-flips steady %ld total %ld faults %ld\n", g_Maps,
           g_Unmaps, cache.Hits, cache.Evictions, steadyFlips, g_Flips, g_Faults);
    printf(g_Failed ? "FAIL\n" : "PASS\n");
    return g_Failed;
}
