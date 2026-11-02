// BD-028 stage A host test: table budget computation.
// Compile: cl /TC /W4 /WX /O2 /MT table_budget_test.c
#include <stdio.h>
#include <stdlib.h>

#include "../table_budget.h"

static int g_pass, g_fail;
#define CHECK(cond, ...) do { if (cond) { g_pass++; } else { g_fail++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while(0)

static void test_paging_pages(void)
{
    // 1 GiB paging window: 512 L0 + 1 L1 + 1 L2 + 1 L3 = 515
    unsigned p = Bc250PinnedPagingPages();
    CHECK(p == 515, "paging pages %u != 515", p);
}

static void test_table_pages_for_range(void)
{
    // 4 KiB range at VA 0: 1 page at each of 4 levels = 4
    CHECK(Bc250TablePagesForRange(0, 4096) == 4, "4K range");
    // 2 MiB aligned: 1 L0 + 1 L1 + 1 L2 + 1 L3 = 4
    CHECK(Bc250TablePagesForRange(0, 2ull * 1024 * 1024) == 4, "2M aligned");
    // 2 MiB + 1 page spans 2 L0 entries: 2 L0 + 1 L1 + 1 L2 + 1 L3 = 5
    CHECK(Bc250TablePagesForRange(0, 2ull * 1024 * 1024 + 4096) == 5, "2M+4K");
    // 4 GiB: 2048 L0 + 4 L1 + 1 L2 + 1 L3 = 2054
    CHECK(Bc250TablePagesForRange(0, 4ull * 1024 * 1024 * 1024) == 2054, "4G");
    // 8 GiB: 4096 L0 + 8 L1 + 1 L2 + 1 L3 = 4106
    CHECK(Bc250TablePagesForRange(0, 8ull * 1024 * 1024 * 1024) == 4106, "8G");
    // Zero bytes: 0
    CHECK(Bc250TablePagesForRange(0, 0) == 0, "zero bytes");
    // Unaligned base: same count for 4K within one page table entry
    CHECK(Bc250TablePagesForRange(4096, 4096) == 4, "offset 4K");
    // Cross a 2 MiB boundary
    unsigned p = Bc250TablePagesForRange(2ull * 1024 * 1024 - 4096, 8192);
    CHECK(p == 5, "cross 2M boundary: %u", p);
}

static void test_current_budget(void)
{
    BC250_TABLE_BUDGET b;
    // 8 GiB available
    unsigned long long avail = 8ull * 1024 * 1024 * 1024;
    CHECK(Bc250TableBudgetCurrent(avail, &b), "8G current ok");
    // 1/32 of 8 GiB = 256 MiB
    CHECK(b.TotalTableBytes == 256ull * 1024 * 1024, "8G table %llu", b.TotalTableBytes);
    CHECK(b.ApplicationBytes == avail - 256ull * 1024 * 1024, "8G app");
    // 4 GiB: 1/32 = 128 MiB
    avail = 4ull * 1024 * 1024 * 1024;
    CHECK(Bc250TableBudgetCurrent(avail, &b), "4G current ok");
    CHECK(b.TotalTableBytes == 128ull * 1024 * 1024, "4G table %llu", b.TotalTableBytes);
    // 12 GiB: 1/32 = 384 MiB
    avail = 12ull * 1024 * 1024 * 1024;
    CHECK(Bc250TableBudgetCurrent(avail, &b), "12G current ok");
    CHECK(b.TotalTableBytes == 384ull * 1024 * 1024, "12G table %llu", b.TotalTableBytes);
    // 64 MiB: 1/32 = 2 MiB, min 4 MiB applies
    avail = 64ull * 1024 * 1024;
    CHECK(Bc250TableBudgetCurrent(avail, &b), "64M current ok");
    CHECK(b.TotalTableBytes == 4ull * 1024 * 1024, "64M table %llu", b.TotalTableBytes);
    // Too small
    CHECK(!Bc250TableBudgetCurrent(32768, &b), "32K too small");
    CHECK(!Bc250TableBudgetCurrent(0, &b), "0 too small");
}

static void test_proposed_budget(void)
{
    BC250_TABLE_BUDGET b;
    // 8 GiB available: pinned paging = 515 * 4K = 2109440, rounded to 64K = 2162688
    unsigned long long avail = 8ull * 1024 * 1024 * 1024;
    CHECK(Bc250TableBudgetProposed(avail, &b), "8G proposed ok");
    unsigned long long expectedPinned = ((515ull * 4096 + 65535) / 65536) * 65536;
    CHECK(b.PinnedPagingBytes == expectedPinned, "8G pinned %llu != %llu", b.PinnedPagingBytes, expectedPinned);
    CHECK(b.TotalTableBytes == expectedPinned, "8G total == pinned");
    CHECK(b.ApplicationBytes == avail - expectedPinned, "8G app");
    // Wasted = current table reserve - pinned = 256 MiB - pinned
    CHECK(b.WastedBytes == 256ull * 1024 * 1024 - expectedPinned, "8G wasted");
    // 12 GiB: wasted = 384 MiB - pinned
    avail = 12ull * 1024 * 1024 * 1024;
    CHECK(Bc250TableBudgetProposed(avail, &b), "12G proposed ok");
    CHECK(b.WastedBytes == 384ull * 1024 * 1024 - expectedPinned, "12G wasted %llu", b.WastedBytes);
    // Too small
    CHECK(!Bc250TableBudgetProposed(32768, &b), "32K too small");
}

static void test_reclaimed_bytes(void)
{
    // At each VRAM size: how much does the proposed policy reclaim?
    unsigned long long sizes[] = {
        4ull * 1024 * 1024 * 1024,
        8ull * 1024 * 1024 * 1024,
        12ull * 1024 * 1024 * 1024,
        16ull * 1024 * 1024 * 1024
    };
    unsigned long long expectedPinned = ((515ull * 4096 + 65535) / 65536) * 65536;
    for (int i = 0; i < 4; i++) {
        BC250_TABLE_BUDGET curr, prop;
        CHECK(Bc250TableBudgetCurrent(sizes[i], &curr), "current %d", i);
        CHECK(Bc250TableBudgetProposed(sizes[i], &prop), "proposed %d", i);
        unsigned long long reclaimed = prop.ApplicationBytes - curr.ApplicationBytes;
        CHECK(reclaimed == curr.TotalTableBytes - expectedPinned,
              "reclaimed %llu at %llu GiB", reclaimed, sizes[i] / (1024*1024*1024));
        // The reclaimed bytes match what the design calls "wasted"
        CHECK(reclaimed == prop.WastedBytes, "reclaimed == wasted %d", i);
    }
}

static unsigned long long cap_policy(unsigned long long avail)
{
    unsigned long long t = (avail / 32u) & ~65535ull;
    if (t > BC250_TABLE_BUDGET_CAP) t = BC250_TABLE_BUDGET_CAP;
    if (t < 4ull * 1024 * 1024) t = 4ull * 1024 * 1024;
    return t;
}

static void test_budget_cap(void)
{
    unsigned long long avail, tableBytes, uncapped;
    // At 8 GiB: 1/32 = 256 MiB, capped to 64 MiB -> app gains 192 MiB
    avail = 8ull * 1024 * 1024 * 1024;
    tableBytes = cap_policy(avail);
    uncapped = (avail / 32u) & ~65535ull;
    CHECK(uncapped == 256ull * 1024 * 1024, "8G uncapped %llu", uncapped);
    CHECK(tableBytes == 64ull * 1024 * 1024, "8G capped %llu", tableBytes);
    CHECK(uncapped - tableBytes == 192ull * 1024 * 1024, "8G gain");
    // At 12 GiB: 1/32 = 384 MiB, capped to 64 MiB -> app gains 320 MiB
    avail = 12ull * 1024 * 1024 * 1024;
    tableBytes = cap_policy(avail);
    uncapped = (avail / 32u) & ~65535ull;
    CHECK(uncapped == 384ull * 1024 * 1024, "12G uncapped %llu", uncapped);
    CHECK(tableBytes == 64ull * 1024 * 1024, "12G capped %llu", tableBytes);
    CHECK(uncapped - tableBytes == 320ull * 1024 * 1024, "12G gain");
    // At 4 GiB: 1/32 = 128 MiB, capped to 64 MiB -> app gains 64 MiB
    avail = 4ull * 1024 * 1024 * 1024;
    tableBytes = cap_policy(avail);
    uncapped = (avail / 32u) & ~65535ull;
    CHECK(uncapped == 128ull * 1024 * 1024, "4G uncapped %llu", uncapped);
    CHECK(tableBytes == 64ull * 1024 * 1024, "4G capped %llu", tableBytes);
    // At 1 GiB: 1/32 = 32 MiB, below cap -> unchanged at 32 MiB
    avail = 1ull * 1024 * 1024 * 1024;
    tableBytes = cap_policy(avail);
    uncapped = (avail / 32u) & ~65535ull;
    CHECK(uncapped == 32ull * 1024 * 1024, "1G uncapped %llu", uncapped);
    CHECK(tableBytes == uncapped, "1G below cap");
    // At 64 MiB: 1/32 = 2 MiB, below cap, min 4 MiB applies
    avail = 64ull * 1024 * 1024;
    tableBytes = cap_policy(avail);
    CHECK(tableBytes == 4ull * 1024 * 1024, "64M min %llu", tableBytes);
    // Cap headroom: 64 MiB is >30x the pinned paging reserve
    {
        unsigned long long pinnedBytes = (unsigned long long)Bc250PinnedPagingPages() * PAGE_SIZE;
        pinnedBytes = (pinnedBytes + BC250_TABLE_ALIGNMENT - 1) & ~(unsigned long long)(BC250_TABLE_ALIGNMENT - 1);
        CHECK(cap_policy(8ull*1024*1024*1024) > pinnedBytes * 10,
              "cap %llu >> pinned %llu", cap_policy(8ull*1024*1024*1024), pinnedBytes);
    }
}

int main(void)
{
    test_paging_pages();
    test_table_pages_for_range();
    test_current_budget();
    test_proposed_budget();
    test_reclaimed_bytes();
    test_budget_cap();
    printf("%d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
