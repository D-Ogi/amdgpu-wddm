// Host test of the 64 KiB page table code of vidmm.c: the functions are the production text
// (generate_vidmm_64k_test.py puts them at ACTUAL_SOURCE) and the translation is the production shim
// (driver/shim/bc250_pte.c, linked). What is under test is the arithmetic between them: 32 entries of 16 slots,
// where a slice starts in the table page, what a refused update leaves, and the table marks.
// The positive control is evidence E17 buffer k64, the 16 entries amdgpu wrote on unit A for a 64 KiB host buffer.
#include <windows.h>
#include <d3dkmthk.h>       // the real DXGK_PTE and D3DGPU_PHYSICAL_ADDRESS (d3dukmdt.h)
#include <stdio.h>
#include <string.h>

#include "bc250_gmc.h"
#include "bc250_pte.h"

// The kernel names the extracted text uses. d3dkmddi.h is a kernel header; the members are those of
// d3dkmddi.h:2081-2095 and :4693-4709 (WDK 10.0.26100.0).
typedef struct { UINT Repeat : 1, InitialUpdate : 1, NotifyEviction : 1, Use64KBPages : 1, Reserved : 28; } DXGK_UPDATEPAGETABLEFLAGS;
typedef enum { DXGK_PAGETABLEUPDATE_CPU_VIRTUAL, DXGK_PAGETABLEUPDATE_GPU_VIRTUAL, DXGK_PAGETABLEUPDATE_GPU_PHYSICAL } DXGK_PAGETABLEUPDATEMODE;
typedef union { PVOID CpuVirtual; D3DGPU_VIRTUAL_ADDRESS GpuVirtual; D3DGPU_PHYSICAL_ADDRESS GpuPhysical; } DXGK_PAGETABLEUPDATEADDRESS;
typedef struct {
    UINT PageTableLevel; HANDLE hAllocation; DXGK_PAGETABLEUPDATEADDRESS PageTableAddress; DXGK_PTE* pPageTableEntries;
    UINT StartIndex, NumPageTableEntries, Reserved0; DXGK_UPDATEPAGETABLEFLAGS Flags; UINT64 DriverProtection;
    UINT64 AllocationOffsetInBytes; HANDLE hProcess; DXGK_PAGETABLEUPDATEMODE UpdateMode; DXGK_PTE* pPageTableEntries64KB;
    D3DGPU_VIRTUAL_ADDRESS FirstPteVirtualAddress;
} DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE;

#define BC250_VIDMM_LEVELS 4u
#define BC250_VIDMM_PTES 512u
#define BC250_VIDMM_LOG_CALLS 4
#define BC250_VIDMM_LOG_REFUSALS 6
#define BC250_VIDMM_PAGE_64K_SLOTS 16u
#define DISPATCH_LEVEL 2
#define KeGetCurrentIrql() 0
#define PAGE_SIZE 4096ull
#define MAXULONG 0xFFFFFFFFul
#define TABLE_SEGMENT 3u
#define TABLE_BASE 0x0000000470000000ull
#define TABLE_BYTES (64ull * 4096)

typedef struct { ULONG SizeOfBitMap; PULONG Buffer; } TEST_BITMAP;
typedef struct {
    struct bc250_pte_context Pte;
    ULONGLONG SegmentPhysical, SegmentLength;
    volatile LONG64 Updates64K, Pages64KFragment, Pages64KPlain, Pages64KZero, Pages64KInvalid;
    volatile LONG64 Tables64K, Tables64KSplit, Tables64KReused, Refused64K;
    TEST_BITMAP Table64K;
    PULONG Table64KBits;
} BC250_VIDMM;
static BC250_VIDMM g_VidMm;
static ULONG g_Bits[2];
static unsigned g_Refusals, g_Counted, g_CountedSlots, g_Logs, g_Failures;

static ULONG RtlCheckBit(TEST_BITMAP* map, ULONG bit) { return (map->Buffer[bit / 32] >> (bit % 32)) & 1u; }
static void RtlSetBit(TEST_BITMAP* map, ULONG bit) { map->Buffer[bit / 32] |= 1u << (bit % 32); }
static void RtlClearBit(TEST_BITMAP* map, ULONG bit) { map->Buffer[bit / 32] &= ~(1u << (bit % 32)); }
static void GuardLog(const char* format, ...) { (void)format; g_Logs++; }
static void VidMmNoteRefusal(const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE* Update, const DXGK_PTE* Pte, BOOLEAN Gpu)
{ (void)Update; (void)Pte; (void)Gpu; g_Refusals++; }
static void VidMmCountEncoding(const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE* Update, ULONG Start, ULONG Count,
                               const ULONGLONG* Entries, ULONG Slots)
{ (void)Update; (void)Start; (void)Entries; g_Counted += Count; g_CountedSlots = Slots; }
static BOOLEAN VidMmRootPhysical(const D3DGPU_PHYSICAL_ADDRESS* Address, ULONGLONG* Physical)
{
    *Physical = 0;
    if (Address->SegmentId != TABLE_SEGMENT || (Address->SegmentOffset & 4095) != 0 ||
        Address->SegmentOffset > TABLE_BYTES - 4096) return FALSE;
    *Physical = TABLE_BASE + Address->SegmentOffset;
    return TRUE;
}

/* ACTUAL_SOURCE */

#define CHECK(expression) do { if (!(expression)) { printf("FAIL line %d: %s\n", __LINE__, #expression); g_Failures++; } } while (0)

// Evidence E17 (evidence/linux/2026-10-06-E17-pte-l25-l38, out/e17/hold/pt/vram-00046ffdb000.hex, 0x80..0xF8).
static const ULONGLONG g_k64[16] = {
    0x000000010f1e0267ull, 0x000000010f1e1267ull, 0x000000010f1e2267ull, 0x000000010f1e3267ull,
    0x000000010f1e4267ull, 0x000000010f1e5267ull, 0x000000010f1e6267ull, 0x000000010f1e7267ull,
    0x000000010f1e8267ull, 0x000000010f1e9267ull, 0x000000010f1ea267ull, 0x000000010f1eb267ull,
    0x000000010f1ec267ull, 0x000000010f1ed267ull, 0x000000010f1ee267ull, 0x000000010f1ef267ull,
};
#define HOST_RW (BC250_DXGK_PTE_VALID | BC250_DXGK_PTE_CACHECOHERENT | BC250_DXGK_PTE_NOEXECUTE)

static void Reset(u32 pages64k)
{
    memset(&g_VidMm, 0, sizeof(g_VidMm));
    memset(g_Bits, 0, sizeof(g_Bits));
    g_VidMm.Pte.units = BC250_PTE_ADDR_PAGES;
    g_VidMm.Pte.aperture = BC250_PTE_VM;
    g_VidMm.Pte.system_segment = 0;
    g_VidMm.Pte.vram_segment = 1;
    g_VidMm.Pte.vram_base = 0x0000000450000000ull;
    g_VidMm.Pte.vram_size = 0x0000000010000000ull;
    g_VidMm.Pte.table_segment = TABLE_SEGMENT;
    g_VidMm.Pte.table_base = TABLE_BASE;
    g_VidMm.Pte.table_size = TABLE_BYTES;
    g_VidMm.Pte.system_limit = 0x0000000440000000ull;
    g_VidMm.Pte.system_64k = pages64k;
    g_VidMm.SegmentPhysical = TABLE_BASE;
    g_VidMm.SegmentLength = TABLE_BYTES;
    if (pages64k) {
        g_VidMm.Table64KBits = g_Bits;
        g_VidMm.Table64K.Buffer = g_Bits;
        g_VidMm.Table64K.SizeOfBitMap = (ULONG)(TABLE_BYTES / 4096);
    }
    g_Refusals = g_Counted = g_CountedSlots = g_Logs = 0;
}

static DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE Update(UINT level, UINT start, UINT count, DXGK_PTE* ptes, UINT use64k)
{
    DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE u;
    memset(&u, 0, sizeof(u));
    u.PageTableLevel = level; u.StartIndex = start; u.NumPageTableEntries = count; u.pPageTableEntries = ptes;
    u.Flags.Use64KBPages = use64k; u.UpdateMode = DXGK_PAGETABLEUPDATE_GPU_PHYSICAL;
    u.PageTableAddress.GpuPhysical.SegmentId = TABLE_SEGMENT;
    u.PageTableAddress.GpuPhysical.SegmentOffset = 5 * 4096;       // the sixth table page
    return u;
}

int main(void)
{
    ULONGLONG entries[512], physical, leaf;
    DXGK_PTE ptes[32];
    DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE u;
    const ULONGLONG table = TABLE_BASE + 5 * 4096;
    unsigned i, n;

    // 1. A 64 KiB table: 32 entries, 16 slots each. Three entries from index 5: the k64 page, the next 64 KiB,
    // an invalid entry.
    Reset(1);
    memset(ptes, 0, sizeof(ptes));
    ptes[0].Flags = HOST_RW; ptes[0].PageAddress = 0x10f1e0;
    ptes[1].Flags = HOST_RW; ptes[1].PageAddress = 0x10f1f0;
    ptes[2].Flags = 0;       ptes[2].PageAddress = 0x123450;
    u = Update(0, 5, 3, ptes, 1);
    CHECK(VidMmPageTableEntrySlots(&u) == 16);
    memset(entries, 0xA5, sizeof(entries));
    CHECK(VidMmEncodePageTable(&u, 0, 3, &physical, entries));
    CHECK(physical == table + 5 * 16 * 8);                          // entry 5 starts at slot 80
    CHECK(memcmp(entries, g_k64, sizeof(g_k64)) == 0);              // evidence E17 k64, slot by slot
    for (n = 0; n < 16; n++) CHECK(entries[16 + n] == 0x000000010f1f0267ull + n * 0x1000ull && entries[32 + n] == 0);
    CHECK(entries[48] == 0xA5A5A5A5A5A5A5A5ull);                    // nothing past 3 * 16 values
    CHECK(g_Counted == 3 && g_CountedSlots == 16 && g_Refusals == 0);
    // A slice of it: entries 1 and 2 of the update, at table entry 6 = slot 96.
    memset(entries, 0xA5, sizeof(entries));
    CHECK(VidMmEncodePageTable(&u, 1, 2, &physical, entries));
    CHECK(physical == table + 6 * 16 * 8 && entries[0] == 0x000000010f1f0267ull && entries[15] == 0x000000010f1ff267ull);
    CHECK(entries[16] == 0 && entries[31] == 0 && entries[32] == 0xA5A5A5A5A5A5A5A5ull);
    // Repeat: one DXGK_PTE for every entry of the update.
    u = Update(0, 0, 32, ptes, 1); u.Flags.Repeat = 1;
    CHECK(VidMmEncodePageTable(&u, 0, 32, &physical, entries) && physical == table);
    for (i = 0; i < 32; i++) CHECK(memcmp(entries + i * 16, g_k64, sizeof(g_k64)) == 0);
    // The table bounds are those of 32 entries.
    u = Update(0, 31, 1, ptes, 1); CHECK(VidMmEncodePageTable(&u, 0, 1, &physical, entries) && physical == table + 31 * 128);
    u = Update(0, 32, 1, ptes, 1); CHECK(!VidMmEncodePageTable(&u, 0, 1, &physical, entries));
    u = Update(0, 30, 3, ptes, 1); CHECK(!VidMmEncodePageTable(&u, 0, 3, &physical, entries));
    u = Update(0, 0, 0, ptes, 1);  CHECK(!VidMmEncodePageTable(&u, 0, 1, &physical, entries));
    u = Update(0, 0, 3, ptes, 1);  CHECK(!VidMmEncodePageTable(&u, 2, 2, &physical, entries));   // slice past the update
    CHECK(g_Refusals == 0 && g_VidMm.Refused64K == 0);
    // A later entry that the translation refuses fails the whole update.
    ptes[1].Flags = HOST_RW | (1ull << 20);
    u = Update(0, 5, 3, ptes, 1);
    CHECK(!VidMmEncodePageTable(&u, 0, 1, &physical, entries) && g_Refusals == 1);
    ptes[1].Flags = HOST_RW;
    // Not a 64 KiB table of this start: a level above the leaf, or the second array of dual PTE mode.
    u = Update(1, 0, 1, ptes, 1); CHECK(VidMmPageTableEntrySlots(&u) == 1 && !VidMmEncodePageTable(&u, 0, 1, &physical, entries));
    u = Update(0, 0, 1, ptes, 1); u.pPageTableEntries64KB = ptes;
    CHECK(!VidMmEncodePageTable(&u, 0, 1, &physical, entries) && g_VidMm.Refused64K == 2);

    // 2. The switch off: the update that section 1 accepts is refused, as by every earlier driver, and counted.
    Reset(0);
    u = Update(0, 5, 3, ptes, 1);
    CHECK(VidMmPageTableEntrySlots(&u) == 1);
    CHECK(!VidMmEncodePageTable(&u, 0, 3, &physical, entries) && g_VidMm.Refused64K == 1 && g_Counted == 0);

    // 3. A 4 KB table is what it was, with the switch on or off: 512 entries, one slot each.
    for (i = 0; i < 2; i++) {
        Reset(i);
        ptes[0].Flags = HOST_RW; ptes[0].PageAddress = 0x10f1e0;
        ptes[1].Flags = HOST_RW; ptes[1].PageAddress = 0x222222;
        ptes[2].Flags = HOST_RW; ptes[2].PageAddress = 0x333333;
        ptes[3].Flags = 0;       ptes[3].PageAddress = 0;
        u = Update(0, 100, 4, ptes, 0);
        memset(entries, 0xA5, sizeof(entries));
        CHECK(VidMmPageTableEntrySlots(&u) == 1 && VidMmEncodePageTable(&u, 1, 2, &physical, entries));
        CHECK(physical == table + 101 * 8 && entries[2] == 0xA5A5A5A5A5A5A5A5ull && g_CountedSlots == 1);
        CHECK(bc250_pte_from_dxgk_level(&g_VidMm.Pte, 0, ptes[1].Flags, ptes[1].PageAddress, &leaf) == 0 && entries[0] == leaf);
        CHECK(entries[0] == 0x0000000222222067ull && entries[1] == 0x0000000333333067ull);       // no fragment
        u = Update(0, 511, 1, ptes, 0); CHECK(VidMmEncodePageTable(&u, 0, 1, &physical, entries) && physical == table + 511 * 8);
        u = Update(0, 512, 1, ptes, 0); CHECK(!VidMmEncodePageTable(&u, 0, 1, &physical, entries));
        u = Update(0, 510, 3, ptes, 0); CHECK(!VidMmEncodePageTable(&u, 0, 3, &physical, entries));
        CHECK(g_VidMm.Refused64K == 0);
    }

    // 4. The table marks and the published counters.
    Reset(1);
    ptes[0].Flags = HOST_RW; ptes[0].PageAddress = 0x10f1e0;                                       // fragment
    ptes[1].Flags = HOST_RW; ptes[1].PageAddress = 0x10f1e1;                                       // not aligned
    ptes[2].Flags = 0;       ptes[2].PageAddress = 0;                                              // invalid
    ptes[3].Flags = BC250_DXGK_PTE_VALID | BC250_DXGK_PTE_ZERO; ptes[3].PageAddress = 0;           // Zero
    u = Update(0, 0, 4, ptes, 1);
    CHECK(VidMmEncodePageTable(&u, 0, 4, &physical, entries));
    VidMmNotePublished(&g_VidMm, &u, 0, 4, table, entries, 16);
    CHECK(g_VidMm.Updates64K == 1 && g_VidMm.Pages64KFragment == 1 && g_VidMm.Pages64KPlain == 1 &&
          g_VidMm.Pages64KInvalid == 1 && g_VidMm.Pages64KZero == 1 && g_VidMm.Tables64K == 1);
    CHECK(VidMmTable64KMarked(&g_VidMm, table) && VidMmTable64KMarked(&g_VidMm, table + 4088) &&
          !VidMmTable64KMarked(&g_VidMm, table + 4096) && !VidMmTable64KMarked(&g_VidMm, TABLE_BASE - 8) &&
          !VidMmTable64KMarked(&g_VidMm, TABLE_BASE + TABLE_BYTES));
    VidMmNotePublished(&g_VidMm, &u, 0, 4, table, entries, 16);                                    // a second slice
    CHECK(g_VidMm.Updates64K == 2 && g_VidMm.Tables64K == 1 && g_VidMm.Pages64KFragment == 2);
    // VidMm's conversion: a 4 KB update of the marked page clears the mark, once.
    u = Update(0, 0, 4, ptes, 0);
    VidMmNotePublished(&g_VidMm, &u, 0, 4, table, entries, 1);
    VidMmNotePublished(&g_VidMm, &u, 0, 4, table, entries, 1);
    CHECK(g_VidMm.Tables64KSplit == 1 && g_VidMm.Tables64KReused == 0 && !VidMmTable64KMarked(&g_VidMm, table));
    // The page as the storage of a new 4 KB table: its first update is not a conversion.
    u = Update(0, 0, 4, ptes, 1);
    VidMmNotePublished(&g_VidMm, &u, 0, 4, table, entries, 16);
    u = Update(0, 0, 4, ptes, 0); u.Flags.InitialUpdate = 1;
    VidMmNotePublished(&g_VidMm, &u, 0, 4, table, entries, 1);
    CHECK(g_VidMm.Tables64K == 2 && g_VidMm.Tables64KSplit == 1 && g_VidMm.Tables64KReused == 1);
    // A directory update and a table outside the segment change no mark; the entries are still counted.
    u = Update(1, 0, 4, ptes, 0);
    VidMmNotePublished(&g_VidMm, &u, 0, 4, table, entries, 1);
    u = Update(0, 0, 4, ptes, 1);
    VidMmNotePublished(&g_VidMm, &u, 0, 4, TABLE_BASE + TABLE_BYTES, entries, 16);
    CHECK(g_VidMm.Tables64K == 2 && g_VidMm.Updates64K == 4 && g_Bits[0] == 0 && g_Bits[1] == 0);
    // The switch off: nothing is counted and no mark exists.
    Reset(0);
    u = Update(0, 0, 4, ptes, 0);
    VidMmNotePublished(&g_VidMm, &u, 0, 4, table, entries, 1);
    CHECK(g_VidMm.Updates64K == 0 && g_VidMm.Tables64KSplit == 0 && !VidMmTable64KMarked(&g_VidMm, table));

    printf("vidmm 64 KiB page tables: %u failures\n", g_Failures);
    return g_Failures ? 1 : 0;
}
