// Host test of the two large page features on one start: 2 MiB VRAM pages on a carve-out with no 1 GiB boundary
// (LargePages with LargePagesSmallCarveout) and 64 KiB host pages (bits 31 and 30 of ApertureSegmentMegabytes).
// Each has its own host test (vram_geometry_test.c, vidmm_64k_test.c, native_bound_test.c, replay_pte.c). This one
// holds what neither of them can: both on, and the three other arms, through the production text of both.
//
// generate_large_pages_combined_test.py puts at ACTUAL_SOURCE the state, the start, the GPU encoder, the publication,
// the table marks, the page walk and the summary of vidmm.c, the memory layout and the layout choice of wddm.c with
// the framebuffer range of vram.c, and the constants they name; at CREATE_ADMIT_SOURCE the lines of WddmCreateAdmit
// that size and align a UMD allocation. The translation (driver/shim/bc250_pte.c), the logical table state
// (paging_pt_shadow.c), the 64 KiB size rule (paging_window.c) and the placement rule (umd_blob.c) are linked.
//
// The numbers are those of the unit that needs the 2 MiB layout: carve-out 0x450000000 + 0x20000000, display
// reservation end 0x3050000 (vram_geometry_test.c says where they come from). The table memory is host memory: the
// test writes into it what the SDMA packet of GfxPagingBuildUpdate writes (the entries, at the address, that
// VidMmEncodePageTable returns), and the production walk reads it back. A level 1 entry is then one of three things:
// a 2 MiB page, a pointer to a 64 KiB leaf table, a pointer to a 4 KB leaf table.
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <d3dkmthk.h>       // the real DXGK_PTE and D3DGPU_PHYSICAL_ADDRESS (d3dukmdt.h)
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_SIZE 0x1000        // wdm.h; table_budget.h takes it when it is there
#include "bc250_gmc.h"
#include "bc250_pte.h"
#include "display_start.h"
#include "dcn_translate.h"
#include "paging_aperture_state.h"
#include "paging_pt_shadow.h"
#include "paging_window.h"
#include "scanout_admit.h"
#include "table_budget.h"
#include "umd_blob.h"

// ---- the kernel names the extracted text uses -------------------------------------------------------------------
typedef LONG NTSTATUS;
#define NT_SUCCESS(status) ((NTSTATUS)(status) >= 0)
#ifndef MAXULONGLONG
#define MAXULONGLONG (~0ull)
#endif
#ifndef MAXULONG
#define MAXULONG 0xFFFFFFFFul
#endif
#define PAGE_SHIFT 12
#define ROUND_TO_PAGES(size) (((ULONG_PTR)(size) + PAGE_SIZE - 1) & ~((ULONG_PTR)PAGE_SIZE - 1))
#define PASSIVE_LEVEL 0
#define DISPATCH_LEVEL 2
static UCHAR g_Irql;
static UCHAR KeGetCurrentIrql(void) { return g_Irql; }
#define POOL_FLAG_PAGED 0x100ull
typedef ULONG_PTR EX_PUSH_LOCK;
#define ExInitializePushLock(lock) (*(lock) = 0)
#define ExAcquirePushLockExclusive(lock) ((void)(lock))
#define ExReleasePushLockExclusive(lock) ((void)(lock))
#define ExAcquirePushLockShared(lock) ((void)(lock))
#define ExReleasePushLockShared(lock) ((void)(lock))
#define KeEnterCriticalRegion() ((void)0)
#define KeLeaveCriticalRegion() ((void)0)
typedef struct { ULONG SizeOfBitMap; PULONG Buffer; } RTL_BITMAP;
static void RtlInitializeBitMap(RTL_BITMAP* map, PULONG buffer, ULONG bits) { map->SizeOfBitMap = bits; map->Buffer = buffer; }
static ULONG RtlCheckBit(RTL_BITMAP* map, ULONG bit) { return (map->Buffer[bit / 32] >> (bit % 32)) & 1u; }
static void RtlSetBit(RTL_BITMAP* map, ULONG bit) { map->Buffer[bit / 32] |= 1u << (bit % 32); }
static void RtlClearBit(RTL_BITMAP* map, ULONG bit) { map->Buffer[bit / 32] &= ~(1u << (bit % 32)); }

// d3dkmddi.h is a kernel header; the members are those of d3dkmddi.h:2081-2095 and :4693-4709 (WDK 10.0.26100.0).
typedef struct { UINT Repeat : 1, InitialUpdate : 1, NotifyEviction : 1, Use64KBPages : 1, Reserved : 28; } DXGK_UPDATEPAGETABLEFLAGS;
typedef enum { DXGK_PAGETABLEUPDATE_CPU_VIRTUAL, DXGK_PAGETABLEUPDATE_GPU_VIRTUAL, DXGK_PAGETABLEUPDATE_GPU_PHYSICAL } DXGK_PAGETABLEUPDATEMODE;
typedef union { PVOID CpuVirtual; D3DGPU_VIRTUAL_ADDRESS GpuVirtual; D3DGPU_PHYSICAL_ADDRESS GpuPhysical; } DXGK_PAGETABLEUPDATEADDRESS;
typedef struct {
    UINT PageTableLevel; HANDLE hAllocation; DXGK_PAGETABLEUPDATEADDRESS PageTableAddress; DXGK_PTE* pPageTableEntries;
    UINT StartIndex, NumPageTableEntries, Reserved0; DXGK_UPDATEPAGETABLEFLAGS Flags; UINT64 DriverProtection;
    UINT64 AllocationOffsetInBytes; HANDLE hProcess; DXGK_PAGETABLEUPDATEMODE UpdateMode; DXGK_PTE* pPageTableEntries64KB;
    D3DGPU_VIRTUAL_ADDRESS FirstPteVirtualAddress;
} DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE;
// DXGK_ALLOCATIONINFO: the members that the lines of WddmCreateAdmit write (d3dkmddi.h).
typedef struct {
    HANDLE hAllocation; SIZE_T Size; UINT Alignment; struct { UINT Value; } HintedBank; UINT MaximumRenamingListLength;
    void* pAllocationUsageHint; SIZE_T PitchAlignedSize; struct { UINT Value, SegmentId0, SegmentId1; } PreferredSegment;
    UINT SupportedReadSegmentSet, SupportedWriteSegmentSet;
} DXGK_ALLOCATIONINFO;

// The members of BC250_DEVICE (bc250kmd.h) that the extracted text reads, with its names and types.
typedef struct {
    BOOLEAN FullWddm, VramEnabled, VramWriteEnabled;
    PHYSICAL_ADDRESS VramPhysical, Bar0Physical;
    ULONGLONG VramLength, VramMcBase, Bar0Length, DisplayReservationEnd;
    UCHAR LargePageLayout;
    BOOLEAN LargePages, SystemPages64K, SystemSizes64K;
    PAGING_APERTURE WddmAperture;
    struct { ULONG Pitch, Height; PHYSICAL_ADDRESS PhysicAddress; } Post;
    unsigned long DisplayStart, ColdScanoutBytes;
} BC250_DEVICE;
// The members of BC250_WDDM and BC250_WDDM_OBJECT (wddm.c) that the lines of WddmCreateAdmit write.
typedef struct {
    BOOLEAN SharedResidency;
    volatile LONG UmdAllocsLocalOnly, UmdAllocsShared, UmdAllocsAperture, UmdAllocs64K, UmdAllocs64KGrown, UmdAllocs64KAligned;
    volatile LONG64 UmdAllocs64KGrownBytes;
} BC250_WDDM;
typedef struct { ULONGLONG UmdVidMmBytes; } BC250_WDDM_OBJECT;

// The log of the extracted text, kept: the start and the summary must name both features, and the test reads them.
#define LOG_LINES 96
static char g_Log[LOG_LINES][200];
static unsigned g_LogCount, g_Checks, g_Failures;
static void GuardLog(const char* Format, ...)
{
    va_list arguments;
    va_start(arguments, Format);
    vsnprintf(g_Log[g_LogCount % LOG_LINES], sizeof(g_Log[0]), Format, arguments);
    va_end(arguments);
    g_LogCount++;
}
static int Logged(const char* Text)
{
    unsigned i;
    for (i = 0; i < LOG_LINES; i++) if (strstr(g_Log[i], Text)) return 1;
    return 0;
}
static void LogReset(void) { memset(g_Log, 0, sizeof(g_Log)); g_LogCount = 0; }
#define CHECK(expression) do { g_Checks++; if (!(expression)) { printf("FAIL line %d: %s\n", __LINE__, #expression); g_Failures++; } } while (0)

static ULONG GuardReadSetting(const wchar_t* Name, ULONG Fallback) { (void)Name; (void)Fallback; return 1; }   // EnableGpuVa
// The end of host memory of the test: the RAM below the carve-out. Every host page of the test is below it.
#define HOST_LIMIT 0x0000000450000000ull
static ULONGLONG VidMmSystemLimit(void) { return HOST_LIMIT; }
static void* ExAllocatePool2(ULONGLONG Flags, SIZE_T Bytes, ULONG Tag) { (void)Flags; (void)Tag; return calloc(1, Bytes); }
static void ExFreePoolWithTag(void* Memory, ULONG Tag) { (void)Tag; free(Memory); }
// The table segment as the driver maps it. Here it is host memory, and the test is the GPU that writes it.
static PUCHAR g_Tables;
static SIZE_T g_TableBytes;
static void* MmMapIoSpaceEx(PHYSICAL_ADDRESS Physical, SIZE_T Bytes, ULONG Protect)
{
    (void)Physical; (void)Protect;
    g_Tables = (PUCHAR)calloc(1, Bytes); g_TableBytes = Bytes;
    return g_Tables;
}
static void MmUnmapIoSpace(void* Mapping, SIZE_T Bytes) { (void)Bytes; free(Mapping); g_Tables = NULL; g_TableBytes = 0; }

void VidMmStop(void);
BOOLEAN VidMmRootPhysical(_In_ const D3DGPU_PHYSICAL_ADDRESS* Address, _Out_ ULONGLONG* Physical);

/* ACTUAL_SOURCE */

// WddmCreateAdmit for a UMD allocation, from the segment of its heap to the end of the 64 KiB sizes.
static void CreateAdmitPlacement(const BC250_DEVICE* device, BC250_WDDM* wddm, struct umd_alloc_view view,
                                 DXGK_ALLOCATIONINFO* info, BC250_WDDM_OBJECT* object)
{
    UINT segment;
    UINT align;
    /* CREATE_ADMIT_SOURCE */
}

// ---- the unit ------------------------------------------------------------------------------------------------------
#define UNIT_BASE 0x0000000450000000ull
#define UNIT_BYTES 0x0000000020000000ull
#define UNIT_MC_BASE 0x000000F400000000ull          // a register value shifted by 24, as vram.c makes every MC base
#define UNIT_RESERVED_END 0x0000000003050000ull     // the display reservation of the unit's tester.38 log
#define PLAIN_VRAM 0x0000000453050000ull            // the tester.38 layout: "VRAM segment 1 at physical 0x453050000 + 0x1A240000"
#define PLAIN_BYTES 0x000000001A240000ull
#define HUGE_VRAM 0x0000000453200000ull             // the 2 MiB layout
#define HUGE_BYTES 0x000000001A090000ull
#define TABLE_BASE 0x000000046D290000ull            // the table segment, the same in both layouts
#define TABLE_BYTES 0x0000000000D70000ull
#define MIB2 0x200000ull
#define SEGMENT(n) ((ULONGLONG)(n) << BC250_DXGK_PTE_SEGMENT_SHIFT)
#define PAGE_64K ((ULONGLONG)BC250_DXGK_PAGE_TABLE_PAGE_64KB << BC250_DXGK_PTE_PAGESIZE_SHIFT)
#define HOST_RW (BC250_DXGK_PTE_VALID | BC250_DXGK_PTE_CACHECOHERENT | BC250_DXGK_PTE_NOEXECUTE)
#define VRAM_RWX (BC250_DXGK_PTE_VALID | SEGMENT(BC250_WDDM_SEGMENT_VRAM))
#define VRAM_LARGE (VRAM_RWX | BC250_DXGK_PTE_LARGEPAGE)
#define TABLE_POINTER (BC250_DXGK_PTE_VALID | SEGMENT(BC250_WDDM_SEGMENT_TABLES))
// Hardware entries written out, not computed: VALID 0x01, SYSTEM 0x02, SNOOPED 0x04, EXECUTABLE 0x10, READABLE 0x20,
// WRITEABLE 0x40, FRAG 4 0x200, FRAG 9 0x480, PDE_PTE bit 54.
#define HUGE_PAGE_BITS 0x00400000000004F1ull        // a 2 MiB page of VRAM, read write execute
#define VRAM_LEAF_BITS 0x0000000000000071ull        // a 4 KB page of VRAM, read write execute
// Table pages of the test, by their offset in the table segment.
#define T_ROOT 0x0000ull
#define T_L2 0x1000ull
#define T_L1 0x2000ull
#define T_A 0x3000ull       // a 64 KiB leaf table, later converted to 4 KB
#define T_B 0x4000ull       // a 4 KB leaf table
#define T_C 0x5000ull       // the 64 KiB leaf table that replaces a 2 MiB page, and is replaced by one

// Evidence E17 (evidence/linux/2026-10-06-E17-pte-l25-l38, out/e17/hold/pt/vram-00046ffdb000.hex, 0x80..0xF8).
static const ULONGLONG g_k64[16] = {
    0x000000010f1e0267ull, 0x000000010f1e1267ull, 0x000000010f1e2267ull, 0x000000010f1e3267ull,
    0x000000010f1e4267ull, 0x000000010f1e5267ull, 0x000000010f1e6267ull, 0x000000010f1e7267ull,
    0x000000010f1e8267ull, 0x000000010f1e9267ull, 0x000000010f1ea267ull, 0x000000010f1eb267ull,
    0x000000010f1ec267ull, 0x000000010f1ed267ull, 0x000000010f1ee267ull, 0x000000010f1ef267ull,
};

static BC250_DEVICE g_Device;
static ULONGLONG g_SegmentOffset, g_SegmentLength, g_TableOffset, g_TableLength;

// One start. The request is what WddmStart leaves for LargePages 0, 1 and 2 and for the two bits of
// ApertureSegmentMegabytes; the choice, the layout and the translation context are the production text.
static NTSTATUS Start(ULONG LargePages, ULONG SmallCarveout, int Pages64K, int Sizes64K)
{
    BC250_DEVICE* d = &g_Device;
    VidMmStop();
    memset(d, 0, sizeof(*d));
    d->FullWddm = TRUE; d->VramEnabled = TRUE; d->VramWriteEnabled = TRUE;
    d->VramPhysical.QuadPart = (LONGLONG)UNIT_BASE; d->VramLength = UNIT_BYTES; d->VramMcBase = UNIT_MC_BASE;
    d->Post.Pitch = 2560 * 4; d->Post.Height = 1440; d->Post.PhysicAddress = d->VramPhysical;
    d->DisplayStart = BC250_DISPLAY_START_INHERITED;
    d->DisplayReservationEnd = UNIT_RESERVED_END;
    d->LargePageLayout = (UCHAR)((LargePages == 1 || LargePages == 2) ? BC250_LARGE_PAGE_LAYOUT_GIANT : BC250_LARGE_PAGE_LAYOUT_NONE);
    d->LargePages = (BOOLEAN)(LargePages == 1);
    d->SystemPages64K = (BOOLEAN)(Pages64K != 0);
    d->SystemSizes64K = (BOOLEAN)(Pages64K != 0 && Sizes64K != 0);
    LogReset();
    WddmSettleLargePageLayout(d, SmallCarveout);
    if (!WddmMemoryLayout(d, &g_SegmentOffset, &g_SegmentLength, &g_TableOffset, &g_TableLength)) return STATUS_UNSUCCESSFUL;
    return VidMmStartLayout(d, g_SegmentOffset, g_SegmentLength, BC250_WDDM_SEGMENT_VRAM,
                            g_TableOffset, g_TableLength, BC250_WDDM_SEGMENT_TABLES);
}

static DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE Update(UINT level, ULONGLONG table, UINT start, UINT count, DXGK_PTE* ptes, UINT use64k)
{
    DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE u;
    memset(&u, 0, sizeof(u));
    u.PageTableLevel = level; u.StartIndex = start; u.NumPageTableEntries = count; u.pPageTableEntries = ptes;
    u.Flags.Use64KBPages = use64k; u.UpdateMode = DXGK_PAGETABLEUPDATE_GPU_PHYSICAL;
    u.PageTableAddress.GpuPhysical.SegmentId = BC250_WDDM_SEGMENT_TABLES;
    u.PageTableAddress.GpuPhysical.SegmentOffset = table;
    return u;
}

// One GPU_PHYSICAL update as the driver handles it after the first engine RUN: GfxPagingBuildUpdate encodes a slice
// (VidMmEncodePageTable), the SDMA packet writes Count * slots entries at the address it returned, and the paging
// record publishes the slice (VidMmCommitPagingUpdate, which encodes it again). Slice is the number of entries of
// the update in one pass; 0 is the whole update. FALSE: refused, and nothing was written.
static ULONGLONG g_Encodes;     // successful encode calls: the Encoded counters count two observations for each slice
static BOOLEAN Gpu(DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE* u, UINT Slice)
{
    static ULONGLONG entries[512];
    UINT start = 0;
    if (Slice == 0) Slice = u->NumPageTableEntries;
    do {
        ULONGLONG physical = 0;
        UINT count = u->NumPageTableEntries - start;
        const ULONG slots = VidMmPageTableEntrySlots(u);
        if (count > Slice) count = Slice;
        if (!VidMmEncodePageTable(u, start, count, &physical, entries)) return FALSE;
        g_Encodes++;
        if (physical < TABLE_BASE || physical - TABLE_BASE > g_TableBytes - (SIZE_T)count * slots * 8) return FALSE;
        memcpy(g_Tables + (SIZE_T)(physical - TABLE_BASE), entries, (SIZE_T)count * slots * 8);
        if (VidMmCommitPagingUpdate(u, start, count) != STATUS_SUCCESS) return FALSE;
        g_Encodes++;
        start += count;
    } while (start < u->NumPageTableEntries);
    return TRUE;
}
static BOOLEAN GpuOne(UINT level, ULONGLONG table, UINT index, ULONGLONG flags, ULONGLONG address, UINT use64k)
{
    DXGK_PTE pte;
    DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE u;
    memset(&pte, 0, sizeof(pte));
    pte.Flags = flags; pte.PageAddress = address;
    u = Update(level, table, index, 1, &pte, use64k);
    return Gpu(&u, 0);
}
static ULONGLONG Entry(ULONGLONG table, UINT index)
{
    ULONGLONG value;
    memcpy(&value, g_Tables + (SIZE_T)table + (SIZE_T)index * 8, 8);
    return value;
}
static void Poke(ULONGLONG table, UINT index, ULONGLONG value) { memcpy(g_Tables + (SIZE_T)table + (SIZE_T)index * 8, &value, 8); }
// The production walk of the table memory. 0 when it refuses; *System says where the page is.
static ULONGLONG Walk(ULONGLONG va, BOOLEAN* System)
{
    ULONGLONG physical = 0;
    BOOLEAN system = FALSE;
    const BOOLEAN ok = VidMmTranslateViewLocked(TABLE_BASE + T_ROOT, va, &physical, &system, FALSE, FALSE, FALSE);
    if (System) *System = system;
    return ok ? physical : 0;
}
// The VA of byte `offset` under level 1 entry `slot` (root entry 0, level 2 entry 0).
#define VA(slot, offset) ((ULONGLONG)(slot) * MIB2 + (ULONGLONG)(offset))
// A refused update leaves the table memory as it was.
static ULONGLONG TableSum(void)
{
    ULONGLONG sum = 0;
    SIZE_T i;
    for (i = 0; i < 6 * 4096; i += 8) { ULONGLONG v; memcpy(&v, g_Tables + i, 8); sum = sum * 1099511628211ull + v; }
    return sum;
}
// A start that fails has no table memory: the cases of that start are failures, not a crash of the test.
#define STARTED(...) do { const NTSTATUS started = Start(__VA_ARGS__); \
                          CHECK(started == STATUS_SUCCESS && g_Tables != NULL); \
                          if (started != STATUS_SUCCESS || g_Tables == NULL) { VidMmStop(); return; } } while (0)
#define REFUSED(call) do { const ULONGLONG before = TableSum(); CHECK(!(call)); CHECK(TableSum() == before); } while (0)

static void Hierarchy(void)
{
    // Root entry 0 -> the level 2 table, its entry 0 -> the level 1 table. PageAddress is a page frame number of the
    // table segment (E18 run 001).
    CHECK(GpuOne(3, T_ROOT, 0, TABLE_POINTER, T_L2 >> 12, 0) && Entry(T_ROOT, 0) == ((TABLE_BASE + T_L2) | 1));
    CHECK(GpuOne(2, T_L2, 0, TABLE_POINTER, T_L1 >> 12, 0) && Entry(T_L2, 0) == ((TABLE_BASE + T_L1) | 1));
}

// ---- both features on --------------------------------------------------------------------------------------------
static void BothOn(void)
{
    BC250_VIDMM* vm = &g_VidMm;
    DXGK_PTE ptes[512];
    DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE u;
    BOOLEAN system;
    unsigned n;
    char line[200];

    STARTED(1, 1, 1, 1);
    // The layout and the context: the 2 MiB layout of vram_geometry_test.c, with 64 KiB host pages next to it.
    CHECK(g_Device.LargePageLayout == BC250_LARGE_PAGE_LAYOUT_HUGE && g_Device.LargePages && g_Device.SystemPages64K);
    CHECK(UNIT_BASE + g_SegmentOffset == HUGE_VRAM && g_SegmentLength == HUGE_BYTES);
    CHECK(UNIT_BASE + g_TableOffset == TABLE_BASE && g_TableLength == TABLE_BYTES);
    CHECK(vm->Ready && vm->Write && g_TableBytes == TABLE_BYTES);
    CHECK(vm->Pte.vram_base == HUGE_VRAM && vm->Pte.vram_size == HUGE_BYTES && vm->Pte.table_base == TABLE_BASE &&
          vm->Pte.table_size == TABLE_BYTES && vm->Pte.system_limit == HOST_LIMIT);
    CHECK(vm->Pte.large_pages == 1 && vm->Pte.huge_pages_only == 1 && vm->Pte.system_64k == 1);
    // One mark for each page of the table segment, whatever the VRAM segment in front of it is.
    CHECK(vm->Table64KBits != NULL && vm->Table64K.SizeOfBitMap == TABLE_BYTES / 4096);
    CHECK(Logged("wddm: VRAM layout 2 MiB base (no 1 GiB boundary, LargePagesSmallCarveout 1): segment 0x453200000 + 0x1A090000"));
    CHECK(Logged("vidmm: table segment 3 at physical 0x46D290000 + 0xD70000, host memory below 0x450000000, updates are WRITTEN"));
    CHECK(Logged("vidmm: VRAM segment 1 at physical 0x453200000 + 0x1A090000, large pages 2 MiB at level 1 only (base on 2 MiB)"));
    CHECK(Logged("vidmm: 64 KiB host pages on: a 64 KiB table entry is 16 leaf entries, fragment 4 (tables tracked)"));
    g_Encodes = 0;
    Hierarchy();

    // 1. Level 1 entry 1: a 2 MiB page of VRAM at segment offset 4 MiB.
    CHECK(GpuOne(1, T_L1, 1, VRAM_LARGE, 0x400000ull >> 12, 0));
    CHECK(Entry(T_L1, 1) == ((HUGE_VRAM + 0x400000ull) | HUGE_PAGE_BITS) && Entry(T_L1, 1) == 0x00400004536004F1ull);
    CHECK(Walk(VA(1, 0x12345), &system) == 0x453612345ull && !system);
    CHECK(Walk(VA(1, 0x1FFFFF), &system) == 0x4537FFFFFull && !system);

    // 2. Level 1 entry 2: a pointer to a 64 KiB leaf table (PageTablePageSize 1). The same directory entry as for a
    // 4 KB table. Then three entries of that table in two passes: the k64 host page, the next 64 KiB of host
    // memory, and the last 64 KiB of the VRAM segment (the 576 KiB tail that no 2 MiB page can map).
    CHECK(GpuOne(1, T_L1, 2, TABLE_POINTER | PAGE_64K, T_A >> 12, 0) && Entry(T_L1, 2) == ((TABLE_BASE + T_A) | 1));
    memset(ptes, 0, sizeof(ptes));
    ptes[0].Flags = HOST_RW;  ptes[0].PageAddress = 0x10f1e0;
    ptes[1].Flags = HOST_RW;  ptes[1].PageAddress = 0x10f1f0;
    ptes[2].Flags = VRAM_RWX; ptes[2].PageAddress = (HUGE_BYTES - 0x10000ull) >> 12;
    u = Update(0, T_A, 0, 3, ptes, 1);
    CHECK(Gpu(&u, 1));
    CHECK(memcmp(g_Tables + T_A, g_k64, sizeof(g_k64)) == 0);                               // evidence E17 k64
    for (n = 0; n < 16; n++) {
        CHECK(Entry(T_A, 16 + n) == 0x000000010f1f0267ull + n * 0x1000ull);
        // A 64 KiB entry in a local segment: 16 plain leaves from the base of this layout, no fragment.
        CHECK(Entry(T_A, 32 + n) == ((HUGE_VRAM + HUGE_BYTES - 0x10000ull + n * 0x1000ull) | VRAM_LEAF_BITS));
    }
    CHECK(Entry(T_A, 32) == 0x000000046D280071ull && Entry(T_A, 47) == 0x000000046D28F071ull && Entry(T_A, 48) == 0);
    CHECK(Walk(VA(2, 0x5123), &system) == 0x10f1e5123ull && system);
    CHECK(Walk(VA(2, 0x10000 + 0xF000 + 0xFFF), &system) == 0x10f1fffffull && system);
    CHECK(Walk(VA(2, 0x20000 + 0xF000 + 0xABC), &system) == 0x46D28FABCull && !system);   // the last page of the segment
    CHECK(Walk(VA(2, 0x30000), &system) == 0);
    CHECK(VidMmTable64KMarked(vm, TABLE_BASE + T_A) && !VidMmTable64KMarked(vm, TABLE_BASE + T_L1));
    CHECK(vm->Updates64K == 3 && vm->Pages64KFragment == 2 && vm->Pages64KPlain == 1 && vm->Tables64K == 1);    // three passes

    // 3. Level 1 entry 3: a pointer to a 4 KB leaf table, with the last 4 KB page of the segment and a host page.
    CHECK(GpuOne(1, T_L1, 3, TABLE_POINTER, T_B >> 12, 0) && Entry(T_L1, 3) == ((TABLE_BASE + T_B) | 1));
    ptes[0].Flags = VRAM_RWX; ptes[0].PageAddress = (HUGE_BYTES - 0x1000ull) >> 12;
    ptes[1].Flags = HOST_RW;  ptes[1].PageAddress = 0x222222;
    u = Update(0, T_B, 5, 2, ptes, 0);
    CHECK(Gpu(&u, 0) && Entry(T_B, 5) == 0x000000046D28F071ull && Entry(T_B, 6) == 0x0000000222222067ull);
    CHECK(Walk(VA(3, 5 * 4096 + 0x10), &system) == 0x46D28F010ull && !system);
    CHECK(Walk(VA(3, 6 * 4096 + 0xFFF), &system) == 0x222222FFFull && system);
    CHECK(!VidMmTable64KMarked(vm, TABLE_BASE + T_B) && vm->Tables64K == 1 && vm->Updates64K == 3);
    // The three forms stand next to each other in one level 1 table, and each walk still ends where it did.
    CHECK(Walk(VA(1, 0x12345), &system) == 0x453612345ull && !system);
    CHECK(Walk(VA(2, 0x5123), &system) == 0x10f1e5123ull && system);
    CHECK(vm->EncodedLarge[1] == 2 && vm->Level1Tables64K == 2 && vm->RefusedLarge[1] == 0 && vm->Refused64K == 0);

    // 4. A 2 MiB page replaced by a 64 KiB table (the allocation moved to host memory). The unmap first: dxgmms2
    // clears Valid and LargePage of the entry and keeps the rest (10.0.26100.9974, RVA 0xEB352 and 0xEB35C), so the
    // segment and the address are still in it. An entry that is not valid is 0 whatever else it holds.
    CHECK(GpuOne(1, T_L1, 1, SEGMENT(BC250_WDDM_SEGMENT_VRAM), 0x400000ull >> 12, 0) && Entry(T_L1, 1) == 0);
    CHECK(Walk(VA(1, 0x12345), &system) == 0);
    memset(ptes, 0, sizeof(ptes));                                                          // 32 entries, one valid
    ptes[4].Flags = HOST_RW; ptes[4].PageAddress = 0x10f200;
    u = Update(0, T_C, 0, 32, ptes, 1);
    u.Flags.InitialUpdate = 1;
    CHECK(Gpu(&u, 7));                                                                      // 5 passes
    // The pointer can still carry a permission bit of the page that was there. A directory entry has no such bit.
    CHECK(GpuOne(1, T_L1, 1, TABLE_POINTER | PAGE_64K | BC250_DXGK_PTE_NOEXECUTE, T_C >> 12, 0));
    CHECK(Entry(T_L1, 1) == ((TABLE_BASE + T_C) | 1));
    for (n = 0; n < 512; n++)
        CHECK(Entry(T_C, n) == ((n >= 64 && n < 80) ? 0x000000010f200267ull + (n - 64) * 0x1000ull : 0));
    CHECK(Walk(VA(1, 4 * 0x10000 + 3), &system) == 0x10f200003ull && system);
    CHECK(Walk(VA(1, 0x12345), &system) == 0);                                              // entry 1 of the table: not mapped
    CHECK(VidMmTable64KMarked(vm, TABLE_BASE + T_C) && vm->Tables64K == 2 && vm->Pages64KFragment == 3);
    CHECK(vm->Pages64KInvalid == 31 && vm->Updates64K == 8);

    // 5. And back: the 64 KiB table replaced by a 2 MiB page at another offset of VRAM.
    CHECK(GpuOne(1, T_L1, 1, VRAM_LARGE, 0x800000ull >> 12, 0) && Entry(T_L1, 1) == 0x0040000453A004F1ull);
    CHECK(Walk(VA(1, 0x12345), &system) == 0x453A12345ull && !system);
    CHECK(Walk(VA(1, 4 * 0x10000 + 3), &system) == 0x453A40003ull && !system);
    CHECK(vm->EncodedLarge[1] == 4 && vm->RefusedLarge[1] == 0);
    // The table page that lost its level 1 entry keeps its mark until VidMm writes it again: a level 1 update names
    // no leaf table. The mark is exact only for updates and fills (vidmm.c). While it stands, a copy of page table
    // entries on that page is refused; a fill clears it, and the copy is then what it was.
    CHECK(VidMmTable64KMarked(vm, TABLE_BASE + T_C));
    CHECK(VidMmCommitPagingCopy(TABLE_BASE + T_B, TABLE_BASE + T_C, 4) == STATUS_INVALID_PARAMETER && vm->Copies64KRefused == 1);
    CHECK(VidMmCommitPagingFill(TABLE_BASE + T_C, 4096, 0) == STATUS_SUCCESS);
    CHECK(!VidMmTable64KMarked(vm, TABLE_BASE + T_C) && vm->Tables64KReused == 1 && vm->Tables64KSplit == 0);
    CHECK(VidMmCommitPagingCopy(TABLE_BASE + T_B, TABLE_BASE + T_C, 4) == STATUS_SUCCESS && vm->Copies64KRefused == 1);
    CHECK(VidMmTable64KMarked(vm, TABLE_BASE + T_A));                                       // the other table is not touched

    // 6. VidMm converts the 64 KiB table of entry 2 to a 4 KB table: 4 KB updates for all 512 entries (a valid 64 KiB
    // entry is 16 entries with consecutive page frame numbers, an invalid one 16 empty entries), then the level 1
    // entry with PageTablePageSize 0. Every fragment goes, every address stays.
    memset(ptes, 0, sizeof(ptes));
    for (n = 0; n < 16; n++) {
        ptes[n].Flags = HOST_RW;       ptes[n].PageAddress = 0x10f1e0 + n;
        ptes[16 + n].Flags = HOST_RW;  ptes[16 + n].PageAddress = 0x10f1f0 + n;
        ptes[32 + n].Flags = VRAM_RWX; ptes[32 + n].PageAddress = ((HUGE_BYTES - 0x10000ull) >> 12) + n;
    }
    u = Update(0, T_A, 0, 48, ptes, 0);
    CHECK(Gpu(&u, 0));
    u = Update(0, T_A, 48, 464, ptes + 48, 0);
    u.Flags.Repeat = 1;
    CHECK(Gpu(&u, 200));
    CHECK(GpuOne(1, T_L1, 2, TABLE_POINTER, T_A >> 12, 0) && Entry(T_L1, 2) == ((TABLE_BASE + T_A) | 1));
    for (n = 0; n < 16; n++) {
        CHECK(Entry(T_A, n) == (g_k64[n] & ~0xF80ull));                                     // the k64 leaves, no fragment
        CHECK(Entry(T_A, 32 + n) == ((HUGE_VRAM + HUGE_BYTES - 0x10000ull + n * 0x1000ull) | VRAM_LEAF_BITS));
    }
    CHECK(Entry(T_A, 0) == 0x000000010f1e0067ull && Entry(T_A, 48) == 0 && Entry(T_A, 511) == 0);
    CHECK(Walk(VA(2, 0x5123), &system) == 0x10f1e5123ull && system);
    CHECK(Walk(VA(2, 0x20000 + 0xF000 + 0xABC), &system) == 0x46D28FABCull && !system);
    CHECK(!VidMmTable64KMarked(vm, TABLE_BASE + T_A) && vm->Tables64KSplit == 1 && vm->Tables64KReused == 1);
    CHECK(Logged("vidmm: 64 KiB table 0x46D293000 converted to 4 KB entries (start 0 count 48)"));
    CHECK(vm->Level1Tables64K == 4 && vm->Tables64K == 2);

    // 7. What this start cannot map is refused with nothing written, each on its own counter.
    CHECK(vm->RefusedLarge[1] == 0 && vm->RefusedLarge[2] == 0 && vm->RefusedLarge[3] == 0 && vm->Refused64K == 0 && vm->GpuRefused == 0);
    REFUSED(GpuOne(1, T_L1, 7, VRAM_LARGE | PAGE_64K, 0x400000ull >> 12, 0));               // a page and a 64 KiB table at once
    CHECK(vm->RefusedLarge[1] == 1 && vm->RefusedSegment[1] == 1 && vm->Refused64K == 0);
    REFUSED(GpuOne(2, T_L2, 5, VRAM_LARGE, 0, 0));                                          // 1 GiB: level 2 on a 2 MiB base
    CHECK(vm->RefusedLarge[2] == 1);
    REFUSED(GpuOne(3, T_ROOT, 5, VRAM_LARGE, 0, 0));
    CHECK(vm->RefusedLarge[3] == 1);
    REFUSED(GpuOne(1, T_L1, 7, BC250_DXGK_PTE_VALID | BC250_DXGK_PTE_LARGEPAGE, 0x109200000ull >> 12, 0));   // host memory
    REFUSED(GpuOne(1, T_L1, 7, VRAM_LARGE, 0x1A000000ull >> 12, 0));                        // the tail: past the segment end
    REFUSED(GpuOne(1, T_L1, 7, VRAM_LARGE, 0x00201000ull >> 12, 0));                        // not a 2 MiB offset
    REFUSED(GpuOne(1, T_L1, 7, VRAM_LARGE, 0x00210000ull >> 12, 0));                        // a 64 KiB offset is not one
    REFUSED(GpuOne(1, T_L1, 7, TABLE_POINTER | BC250_DXGK_PTE_LARGEPAGE, 0, 0));            // the table segment
    CHECK(vm->RefusedLarge[1] == 6 && vm->RefusedSegment[0] == 1 && vm->RefusedSegment[1] == 6 && vm->RefusedSegment[3] == 1);
    REFUSED(GpuOne(2, T_L2, 5, TABLE_POINTER | PAGE_64K, T_A >> 12, 0));                    // a 64 KiB table below level 2
    CHECK(vm->Refused64K == 0 && vm->GpuRefused == 9);
    REFUSED(GpuOne(1, T_L1, 7, TABLE_POINTER, T_A >> 12, 1));                               // Use64KBPages above the leaf
    CHECK(vm->Refused64K == 1);
    REFUSED(GpuOne(0, T_B, 0, HOST_RW | BC250_DXGK_PTE_LARGEPAGE, 0x10f1e0, 1));            // LargePage on a 64 KiB entry
    CHECK(vm->Refused64K == 2 && vm->RefusedLarge[1] == 6);
    REFUSED(GpuOne(0, T_B, 0, VRAM_RWX, (HUGE_BYTES - 0x8000ull) >> 12, 1));                // 64 KiB that runs past the segment
    REFUSED(GpuOne(0, T_B, 0, VRAM_RWX, HUGE_BYTES >> 12, 0));                              // 4 KB past the segment
    REFUSED(GpuOne(0, T_B, 0, HOST_RW, HOST_LIMIT >> 12, 0));                               // past the end of host memory
    CHECK(vm->Refused64K == 3 && vm->GpuRefused == 13);
    // The walk refuses a level 1 or level 2 page that this start cannot have written.
    Poke(T_L1, 9, HUGE_VRAM | 0x0040000000000971ull);                                       // fragment 18 at level 1
    CHECK(Walk(VA(9, 0), &system) == 0);
    Poke(T_L1, 9, (HUGE_VRAM + 0x1A000000ull) | HUGE_PAGE_BITS);                            // 2 MiB that runs past the end
    CHECK(Walk(VA(9, 0), &system) == 0);
    Poke(T_L1, 9, (HUGE_VRAM + 0x10000ull) | HUGE_PAGE_BITS);                               // not a 2 MiB address
    CHECK(Walk(VA(9, 0), &system) == 0);
    Poke(T_L1, 9, 0x0000000109200000ull | HUGE_PAGE_BITS | 2);                              // host memory
    CHECK(Walk(VA(9, 0), &system) == 0);
    Poke(T_L1, 9, 0x000000046D400000ull | HUGE_PAGE_BITS);                                  // a 2 MiB address of the table segment
    CHECK(Walk(VA(9, 0), &system) == 0);
    Poke(T_L1, 9, (HUGE_VRAM + 0x600000ull) | HUGE_PAGE_BITS);                              // the control: this one is a page
    CHECK(Walk(VA(9, 0x777), &system) == 0x453800777ull && !system);
    Poke(T_L1, 9, 0);
    Poke(T_L2, 3, HUGE_VRAM | 0x0040000000000971ull);                                       // a 1 GiB page at level 2
    CHECK(Walk(3ull << 30, &system) == 0);
    Poke(T_L2, 3, 0);

    // 8. The summary: the lines of each feature, with its own numbers. Level 1 large pages: three updates, two
    // observations each (the build and the publication); six refusals.
    CHECK(g_Encodes == 2 * (2 + 1 + 1 + 3 + 1 + 1 + 1 + 5 + 1 + 1 + 1 + 3 + 1));
    LogReset();
    VidMmSummary();
    CHECK(Logged("vidmm summary: large pages reported, level 1 only (base on 2 MiB)"));
    CHECK(Logged("vidmm summary: level 1 large pages encoded 4, refused 6"));
    CHECK(Logged("vidmm summary: level 2 large pages encoded 0, refused 1"));
    CHECK(Logged("vidmm summary: level 3 large pages encoded 0, refused 1"));
    CHECK(Logged("vidmm summary: 64 KiB host pages reported"));
    CHECK(Logged("vidmm summary: 64 KiB entries: fragment 4 3, no fragment 1, refused 3"));
    CHECK(Logged("vidmm summary: 64 KiB entries: zero 0, invalid 31"));
    CHECK(Logged("vidmm summary: 64 KiB tables: 8 updates, 2 tables, 1 converted to 4 KB, 1 reused"));
    CHECK(Logged("vidmm summary: 64 KiB tables: 4 level 1 entries, 1 copies refused"));
    snprintf(line, sizeof(line), "vidmm summary: %ld cpu-virtual calls, %ld gpu-physical calls, %ld gpu-physical refusals", 0L, 0L, 13L);
    CHECK(Logged(line));
    CHECK(Logged("vidmm summary: segment 1 named by") && Logged("vidmm summary: segment 0 named by"));
}

// ---- one feature, and none ---------------------------------------------------------------------------------------
static void OtherArms(void)
{
    BC250_VIDMM* vm = &g_VidMm;
    BOOLEAN system;

    // The 2 MiB layout alone: the 2 MiB page is the entry it was with both, a 64 KiB table is refused as by every
    // earlier driver.
    STARTED(1, 1, 0, 0);
    CHECK(vm->Pte.vram_base == HUGE_VRAM && vm->Pte.large_pages == 1 && vm->Pte.huge_pages_only == 1 && vm->Pte.system_64k == 0);
    CHECK(vm->Table64KBits == NULL && !Logged("64 KiB host pages on"));
    Hierarchy();
    CHECK(GpuOne(1, T_L1, 1, VRAM_LARGE, 0x400000ull >> 12, 0) && Entry(T_L1, 1) == 0x00400004536004F1ull);
    CHECK(Walk(VA(1, 0x12345), &system) == 0x453612345ull && !system);
    REFUSED(GpuOne(1, T_L1, 2, TABLE_POINTER | PAGE_64K, T_A >> 12, 0));
    REFUSED(GpuOne(0, T_A, 0, HOST_RW, 0x10f1e0, 1));
    CHECK(vm->Refused64K == 1 && vm->Asked64K == 1 && vm->Updates64K == 0);
    LogReset(); VidMmSummary();
    CHECK(Logged("vidmm summary: large pages reported, level 1 only (base on 2 MiB)") &&
          Logged("vidmm summary: level 1 large pages encoded 2, refused 0") &&
          Logged("vidmm summary: 64 KiB host pages not reported") && Logged("vidmm summary: 64 KiB updates refused 1") &&
          !Logged("64 KiB entries"));

    // 64 KiB host pages alone, on the plain layout of tester.38: the 64 KiB entries are what they were with both
    // (a VRAM page from the base of this layout), a 2 MiB page is refused as by every earlier driver on this unit.
    STARTED(1, 0, 1, 1);
    CHECK(g_Device.LargePageLayout == BC250_LARGE_PAGE_LAYOUT_NONE && !g_Device.LargePages);
    CHECK(UNIT_BASE + g_SegmentOffset == PLAIN_VRAM && g_SegmentLength == PLAIN_BYTES && UNIT_BASE + g_TableOffset == TABLE_BASE);
    CHECK(vm->Pte.vram_base == PLAIN_VRAM && vm->Pte.large_pages == 0 && vm->Pte.huge_pages_only == 0 && vm->Pte.system_64k == 1);
    CHECK(Logged("wddm: LargePages off: no VRAM segment above the first 1 GiB physical boundary"));
    CHECK(Logged("vidmm: VRAM segment 1 at physical 0x453050000 + 0x1A240000, large pages off"));
    CHECK(Logged("vidmm: 64 KiB host pages on: a 64 KiB table entry is 16 leaf entries, fragment 4 (tables tracked)"));
    Hierarchy();
    CHECK(GpuOne(1, T_L1, 2, TABLE_POINTER | PAGE_64K, T_A >> 12, 0) && Entry(T_L1, 2) == ((TABLE_BASE + T_A) | 1));
    CHECK(GpuOne(0, T_A, 0, HOST_RW, 0x10f1e0, 1) && memcmp(g_Tables + T_A, g_k64, sizeof(g_k64)) == 0);
    CHECK(GpuOne(0, T_A, 1, VRAM_RWX, 0x10000ull >> 12, 1) && Entry(T_A, 16) == ((PLAIN_VRAM + 0x10000ull) | VRAM_LEAF_BITS));
    CHECK(Walk(VA(2, 0x5123), &system) == 0x10f1e5123ull && system);
    CHECK(Walk(VA(2, 0x10000 + 0x234), &system) == PLAIN_VRAM + 0x10234ull && !system);
    REFUSED(GpuOne(1, T_L1, 1, VRAM_LARGE, 0x400000ull >> 12, 0));
    REFUSED(GpuOne(1, T_L1, 1, VRAM_LARGE, 0x1B0000ull >> 12, 0));      // the offset whose address is on 2 MiB here
    CHECK(vm->RefusedLarge[1] == 2 && vm->EncodedLarge[1] == 0);
    LogReset(); VidMmSummary();
    CHECK(Logged("vidmm summary: large pages not reported") && Logged("vidmm summary: level 1 large pages encoded 0, refused 2") &&
          Logged("vidmm summary: 64 KiB host pages reported") &&
          Logged("vidmm summary: 64 KiB entries: fragment 4 1, no fragment 1, refused 0"));

    // Nothing on, with LargePages 1 as the unit has it and with LargePages 0: the layout and the lines of tester.38,
    // both refusals, and the 4 KB entries of every earlier driver.
    {
        unsigned largePages;
        for (largePages = 0; largePages < 2; largePages++) {
            STARTED(largePages, 0, 0, 0);
            CHECK(g_Device.LargePageLayout == BC250_LARGE_PAGE_LAYOUT_NONE && !g_Device.LargePages && !g_Device.SystemPages64K);
            CHECK(UNIT_BASE + g_SegmentOffset == PLAIN_VRAM && g_SegmentLength == PLAIN_BYTES &&
                  UNIT_BASE + g_TableOffset == TABLE_BASE && g_TableLength == TABLE_BYTES);
            CHECK(vm->Pte.vram_base == PLAIN_VRAM && vm->Pte.vram_size == PLAIN_BYTES && vm->Pte.large_pages == 0 &&
                  vm->Pte.huge_pages_only == 0 && vm->Pte.system_64k == 0 && vm->Table64KBits == NULL);
            CHECK(Logged("vidmm: VRAM segment 1 at physical 0x453050000 + 0x1A240000, large pages off"));
            CHECK(!Logged("64 KiB host pages on") && !Logged("2 MiB base:"));
            Hierarchy();
            CHECK(GpuOne(1, T_L1, 3, TABLE_POINTER, T_B >> 12, 0) && Entry(T_L1, 3) == ((TABLE_BASE + T_B) | 1));
            CHECK(GpuOne(0, T_B, 5, VRAM_RWX, 0x1000ull >> 12, 0) && Entry(T_B, 5) == 0x0000000453051071ull);
            CHECK(GpuOne(0, T_B, 6, HOST_RW, 0x222222, 0) && Entry(T_B, 6) == 0x0000000222222067ull);
            CHECK(Walk(VA(3, 5 * 4096 + 0x10), &system) == 0x453051010ull && !system);
            REFUSED(GpuOne(1, T_L1, 1, VRAM_LARGE, 0x400000ull >> 12, 0));
            REFUSED(GpuOne(1, T_L1, 2, TABLE_POINTER | PAGE_64K, T_A >> 12, 0));
            REFUSED(GpuOne(0, T_A, 0, HOST_RW, 0x10f1e0, 1));
            LogReset(); VidMmSummary();
            CHECK(Logged("vidmm summary: large pages not reported") && Logged("vidmm summary: 64 KiB host pages not reported"));
            CHECK(Logged("vidmm summary: level 1 large pages encoded 0, refused 1") && Logged("vidmm summary: 64 KiB updates refused 1"));
        }
    }
    VidMmStop();
}

// ---- the size and the alignment of a UMD allocation -------------------------------------------------------------
static struct umd_alloc_view View(unsigned long heap, unsigned long long bytes, unsigned long long alignment)
{
    struct umd_alloc_view view;
    memset(&view, 0, sizeof(view));
    view.heap = heap; view.bytes = bytes; view.alignment = alignment; view.version = 2; view.cache_policy_valid = 1;
    return view;
}
static DXGK_ALLOCATIONINFO Admit(const BC250_DEVICE* device, BOOLEAN shared, struct umd_alloc_view view, ULONGLONG* bound)
{
    DXGK_ALLOCATIONINFO info;
    BC250_WDDM wddm;
    BC250_WDDM_OBJECT object;
    memset(&info, 0, sizeof(info)); memset(&wddm, 0, sizeof(wddm));
    wddm.SharedResidency = shared;
    object.UmdVidMmBytes = view.bytes;          // as WddmCreateAdmit sets it before these lines
    CreateAdmitPlacement(device, &wddm, view, &info, &object);
    CHECK(info.hAllocation == &object && info.SupportedReadSegmentSet == info.SupportedWriteSegmentSet);
    *bound = object.UmdVidMmBytes;
    return info;
}
#define VRAM_SET 1u
#define BOTH_SETS 3u
#define APERTURE_SET 2u
static void Sizes(void)
{
    BC250_DEVICE both, huge, sizes, none;
    DXGK_ALLOCATIONINFO info;
    ULONGLONG bound;
    memset(&both, 0, sizeof(both));
    both.LargePageLayout = BC250_LARGE_PAGE_LAYOUT_HUGE; both.LargePages = TRUE; both.SystemPages64K = TRUE; both.SystemSizes64K = TRUE;
    huge = both; huge.SystemPages64K = FALSE; huge.SystemSizes64K = FALSE;
    sizes = both; sizes.LargePageLayout = BC250_LARGE_PAGE_LAYOUT_NONE; sizes.LargePages = FALSE;
    memset(&none, 0, sizeof(none));

    // Both rules on one allocation: VRAM first, the aperture second (shared residency), 4 MiB, 256 KiB from the
    // winsys. It is placed on 2 MiB, which is a multiple of 64 KiB, and its size is a multiple of both: in VRAM each
    // whole 2 MiB of it can be one level 1 entry, in host memory VidMm can back and map it with 64 KiB pages.
    info = Admit(&both, TRUE, View(UMD_BLOB_HEAP_VRAM, 0x400000, 0x40000), &bound);
    CHECK(info.Size == 0x400000 && info.Alignment == 0x200000 && bound == 0x400000 && info.SupportedReadSegmentSet == BOTH_SETS);
    CHECK(info.PreferredSegment.SegmentId0 == BC250_WDDM_SEGMENT_VRAM && info.PreferredSegment.SegmentId1 == BC250_WDDM_SEGMENT_APERTURE);
    CHECK(info.Alignment % BC250_VIDMM_HUGE_PAGE_BYTES == 0 && info.Alignment % PAGING_HOST_PAGE_64K == 0 && info.Size % PAGING_HOST_PAGE_64K == 0);
    // 2 MiB and one page: placed on 2 MiB, grown to the next 64 KiB, and the bound of a fill is the grown size.
    info = Admit(&both, TRUE, View(UMD_BLOB_HEAP_VRAM, 0x201000, 0x1000), &bound);
    CHECK(info.Size == 0x210000 && info.Alignment == 0x200000 && bound == 0x210000);
    // Below 2 MiB: 64 KiB alignment, as with the 64 KiB sizes alone; an alignment above 64 KiB is kept.
    info = Admit(&both, TRUE, View(UMD_BLOB_HEAP_VRAM, 0x100000, 0x1000), &bound);
    CHECK(info.Size == 0x100000 && info.Alignment == 0x10000 && bound == 0x100000);
    info = Admit(&both, TRUE, View(UMD_BLOB_HEAP_VRAM, 0x111000, 0x40000), &bound);
    CHECK(info.Size == 0x120000 && info.Alignment == 0x40000 && bound == 0x120000);
    info = Admit(&both, TRUE, View(UMD_BLOB_HEAP_VRAM, 0x8000, 0x1000), &bound);
    CHECK(info.Size == 0x8000 && info.Alignment == 0x1000 && bound == 0x8000);
    // The one size range where the two rules do not add up: within 64 KiB below 2 MiB and not a multiple of 64 KiB.
    // The placement is decided on the size before the growth, so the allocation becomes 2 MiB on a 64 KiB offset and
    // gets 4 KB entries in VRAM. Not wrong (VidMm sends a 2 MiB page only for an aligned offset), and neither the
    // winsys nor the shell asks for such a VRAM size (both round to 64 KiB or more from 64 KiB on).
    info = Admit(&both, TRUE, View(UMD_BLOB_HEAP_VRAM, 0x1F1000, 0x1000), &bound);
    CHECK(info.Size == 0x200000 && info.Alignment == 0x10000 && bound == 0x200000);
    // VRAM only (no shared residency, or a scan-out, which never shares): the 2 MiB placement alone, no growth.
    info = Admit(&both, FALSE, View(UMD_BLOB_HEAP_VRAM, 0x201000, 0x1000), &bound);
    CHECK(info.Size == 0x201000 && info.Alignment == 0x200000 && bound == 0x201000 && info.SupportedReadSegmentSet == VRAM_SET);
    {
        struct umd_alloc_view scanout = View(UMD_BLOB_HEAP_VRAM, 0xE11000, 0x1000);
        scanout.scanout = 1;
        info = Admit(&both, TRUE, scanout, &bound);
        CHECK(info.Size == 0xE11000 && info.Alignment == 0x200000 && bound == 0xE11000 && info.SupportedReadSegmentSet == VRAM_SET);
    }
    // The aperture heap: the 64 KiB sizes alone. The 2 MiB placement is for VRAM.
    info = Admit(&both, TRUE, View(UMD_BLOB_HEAP_GTT, 0x401000, 0x1000), &bound);
    CHECK(info.Size == 0x410000 && info.Alignment == 0x10000 && bound == 0x410000 && info.SupportedReadSegmentSet == APERTURE_SET);

    // One feature: each is what it is on its own branch.
    info = Admit(&huge, TRUE, View(UMD_BLOB_HEAP_VRAM, 0x201000, 0x1000), &bound);
    CHECK(info.Size == 0x201000 && info.Alignment == 0x200000 && bound == 0x201000 && info.SupportedReadSegmentSet == BOTH_SETS);
    info = Admit(&huge, TRUE, View(UMD_BLOB_HEAP_GTT, 0x401000, 0x1000), &bound);
    CHECK(info.Size == 0x401000 && info.Alignment == 0x1000 && bound == 0x401000);
    info = Admit(&sizes, TRUE, View(UMD_BLOB_HEAP_VRAM, 0x201000, 0x1000), &bound);
    CHECK(info.Size == 0x210000 && info.Alignment == 0x10000 && bound == 0x210000);
    info = Admit(&sizes, TRUE, View(UMD_BLOB_HEAP_VRAM, 0x400000, 0x40000), &bound);
    CHECK(info.Size == 0x400000 && info.Alignment == 0x40000 && bound == 0x400000);
    // Nothing on: the size and the alignment of tester.38, the request rounded to pages and the winsys alignment.
    info = Admit(&none, TRUE, View(UMD_BLOB_HEAP_VRAM, 0x400000, 0x40000), &bound);
    CHECK(info.Size == 0x400000 && info.Alignment == 0x40000 && bound == 0x400000);
    info = Admit(&none, TRUE, View(UMD_BLOB_HEAP_VRAM, 0x201234, 0x1000), &bound);
    CHECK(info.Size == 0x202000 && info.Alignment == 0x1000 && bound == 0x201234);
    info = Admit(&none, TRUE, View(UMD_BLOB_HEAP_GTT, 0x401000, 0), &bound);
    CHECK(info.Size == 0x401000 && info.Alignment == 0x1000 && bound == 0x401000);
}

int main(void)
{
    BothOn();
    OtherArms();
    Sizes();
    printf("large pages combined: %u checks, %u failures\n", g_Checks, g_Failures);
    return g_Failures ? 1 : 0;
}
