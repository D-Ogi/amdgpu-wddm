// BD-029 host test: ADL page reading through Bc250DmaAdlPage.
// Compile: cl /TC /W4 /WX /O2 /MT adl_test.c
#include <stdio.h>
#include <string.h>
typedef unsigned long long ULONGLONG;
typedef unsigned long ULONG;
#define PAGE_SIZE 4096u
#define MAXULONGLONG ((ULONGLONG)~0)
typedef struct {long long QuadPart;} PHYSICAL_ADDRESS;
static PHYSICAL_ADDRESS MmGetPhysicalAddress(void*p){PHYSICAL_ADDRESS a={(long long)(uintptr_t)p};return a;}

#include "bc250_dma.h"

static unsigned checks, failures;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %u: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while(0)

// Model a DXGK_ADL: { UINT32 PageCount, UINT32 Flags, union { PFN Base / PFN* Pages } }
#pragma warning(push)
#pragma warning(disable:4201)
typedef struct {
    unsigned PageCount;
    unsigned Flags;
    union {
        unsigned long long BasePageNumber;
        const unsigned long long* Pages;
    };
} MODEL_ADL;
#pragma warning(pop)

static void test_contiguous_adl(void)
{
    MODEL_ADL adl;
    ULONGLONG addr;
    memset(&adl, 0, sizeof(adl));
    adl.PageCount = 100;
    adl.Flags = 1; // Contiguous
    adl.BasePageNumber = 0x10000; // PFN 0x10000 = physical 0x10000000

    CHECK(Bc250DmaAdlPage(&adl, 0, 0, &addr), "contiguous page 0");
    CHECK(addr == 0x10000ull * PAGE_SIZE, "page 0 addr 0x%llX", addr);

    CHECK(Bc250DmaAdlPage(&adl, 0, 50, &addr), "contiguous page 50");
    CHECK(addr == (0x10000ull + 50) * PAGE_SIZE, "page 50 addr 0x%llX", addr);

    CHECK(Bc250DmaAdlPage(&adl, 0, 99, &addr), "contiguous last page");
    CHECK(addr == (0x10000ull + 99) * PAGE_SIZE, "page 99 addr 0x%llX", addr);

    // Out of range
    CHECK(!Bc250DmaAdlPage(&adl, 0, 100, &addr), "contiguous out of range");

    // With offset
    CHECK(Bc250DmaAdlPage(&adl, 10, 5, &addr), "contiguous offset 10+5");
    CHECK(addr == (0x10000ull + 15) * PAGE_SIZE, "offset addr 0x%llX", addr);

    // Offset + index out of range
    CHECK(!Bc250DmaAdlPage(&adl, 90, 11, &addr), "contiguous offset overflow");
    CHECK(!Bc250DmaAdlPage(&adl, 100, 0, &addr), "contiguous offset at limit");
}

static void test_noncontiguous_adl(void)
{
    unsigned long long pfns[] = { 0x20000, 0x30000, 0x10000, 0x50000 };
    MODEL_ADL adl;
    ULONGLONG addr;
    memset(&adl, 0, sizeof(adl));
    adl.PageCount = 4;
    adl.Flags = 0; // Non-contiguous
    adl.Pages = pfns;

    CHECK(Bc250DmaAdlPage(&adl, 0, 0, &addr), "noncontig page 0");
    CHECK(addr == 0x20000ull * PAGE_SIZE, "page 0 addr 0x%llX", addr);

    CHECK(Bc250DmaAdlPage(&adl, 0, 2, &addr), "noncontig page 2");
    CHECK(addr == 0x10000ull * PAGE_SIZE, "page 2 addr 0x%llX", addr);

    CHECK(Bc250DmaAdlPage(&adl, 0, 3, &addr), "noncontig page 3");
    CHECK(addr == 0x50000ull * PAGE_SIZE, "page 3 addr 0x%llX", addr);

    CHECK(!Bc250DmaAdlPage(&adl, 0, 4, &addr), "noncontig out of range");

    // With offset
    CHECK(Bc250DmaAdlPage(&adl, 1, 1, &addr), "noncontig offset 1+1");
    CHECK(addr == 0x10000ull * PAGE_SIZE, "offset 1+1 = pfn[2]");

    CHECK(!Bc250DmaAdlPage(&adl, 2, 3, &addr), "noncontig offset+index overflow");
}

static void test_null_and_edge(void)
{
    ULONGLONG addr;
    MODEL_ADL adl;
    memset(&adl, 0, sizeof(adl));

    CHECK(!Bc250DmaAdlPage(NULL, 0, 0, &addr), "null adl");
    CHECK(!Bc250DmaAdlPage(&adl, 0, 0, NULL), "null address");

    // Zero PageCount
    adl.PageCount = 0;
    adl.Flags = 1;
    adl.BasePageNumber = 0x1000;
    CHECK(!Bc250DmaAdlPage(&adl, 0, 0, &addr), "zero pages");

    // Non-contiguous with NULL pages pointer
    adl.PageCount = 10;
    adl.Flags = 0;
    adl.Pages = NULL;
    CHECK(!Bc250DmaAdlPage(&adl, 0, 0, &addr), "null pages pointer");
}

static void test_alignment(void)
{
    // Bc250DmaAdlPage always produces page-aligned addresses from PFN shift
    MODEL_ADL adl;
    ULONGLONG addr;
    unsigned long long pfns[] = { 1, 0, 0xFFFFF };
    memset(&adl, 0, sizeof(adl));
    adl.PageCount = 3;
    adl.Flags = 0;
    adl.Pages = pfns;

    CHECK(Bc250DmaAdlPage(&adl, 0, 0, &addr), "pfn 1");
    CHECK(addr == PAGE_SIZE, "pfn 1 = 0x%llX", addr);
    CHECK((addr & (PAGE_SIZE - 1)) == 0, "aligned");

    CHECK(Bc250DmaAdlPage(&adl, 0, 1, &addr), "pfn 0");
    CHECK(addr == 0, "pfn 0 = 0x%llX", addr);

    CHECK(Bc250DmaAdlPage(&adl, 0, 2, &addr), "pfn 0xFFFFF");
    CHECK(addr == 0xFFFFFull * PAGE_SIZE, "large pfn");
}

static void test_legacy_view_compatibility(void)
{
    // Verify that Bc250DmaPage and Bc250DmaAdlPage give the same address
    // for the same physical page.
    unsigned long long pfn = 0x42000;
    ULONGLONG adlAddr, viewAddr;
    MODEL_ADL adl;
    BC250_DMA_PAGES view;
    memset(&adl, 0, sizeof(adl));
    adl.PageCount = 1;
    adl.Flags = 1;
    adl.BasePageNumber = pfn;

    view.Pages = NULL;
    view.Base = pfn * PAGE_SIZE;
    view.Count = 1;

    CHECK(Bc250DmaAdlPage(&adl, 0, 0, &adlAddr), "adl read");
    CHECK(Bc250DmaPage(&view, 0, &viewAddr), "view read");
    CHECK(adlAddr == viewAddr, "adl == view: 0x%llX vs 0x%llX", adlAddr, viewAddr);
}

int main(void)
{
    test_contiguous_adl();
    test_noncontiguous_adl();
    test_null_and_edge();
    test_alignment();
    test_legacy_view_compatibility();
    printf("%u PASS, %u FAIL\n", checks - failures, failures);
    return failures ? 1 : 0;
}
