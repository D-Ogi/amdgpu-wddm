// VidMm's page tables (milestone M7, ADR 0008 stage B). dxgmms2 keeps a four-level page table per process in the
// local segment and asks the driver to fill it: BuildPagingBuffer operation 11 (UpdatePageTable) carries DXGK_PTEs,
// this file turns each into the entry the GPU's walker reads (driver/shim/bc250_pte.c, amdgpu's flags) and, behind
// its own gate, writes it into the table page through the CPU - the carve-out is reachable by physical address
// (facts M31), so there is no paging packet to build and the paging buffer stays empty, as in stage A.
//
//   <service key>\Parameters
//     EnableGpuVa  REG_DWORD  0 = PLAN: translate, count and log, write nothing (default).
//                             1 = write the translated entries into VidMm's page table pages. Needs EnableVram and
//                                 EnableVramWrite. No VMID is pointed at those tables in stage B: nothing walks them
//                                 but the witness (bc250rd reading VRAM), so a wrong entry costs a wrong reading.
//
// The three questions bc250_pte.h leaves open (units of PageAddress, the id of system memory, which level is the
// leaf) were answered by PLAN's log before the first write (E18 run 001, see VidMmStart): raw Flags and PageAddress of the first calls of every
// level, and the first entry of every segment id that shows up. Co nagle, to po diable (haste is the devil's work).
#include "bc250kmd.h"
#include "bc250_pte.h"

#define BC250_VIDMM_LEVELS 4u
#define BC250_VIDMM_PTES 512u
#define BC250_VIDMM_LOG_CALLS 4             // first calls of each level that reach the log
#define BC250_VIDMM_LOG_REFUSALS 6
#define BC250_VIDMM_SEGMENT_IDS 32u         // DXGK_PTE.Segment is five bits wide

typedef struct _BC250_VIDMM {
    BOOLEAN Ready;                          // the segment is known: translation means something
    BOOLEAN Write;                          // EnableGpuVa = 1 and VRAM writes allowed
    ULONGLONG SegmentPhysical;              // system physical address of segment offset 0
    ULONGLONG SegmentLength;
    struct bc250_pte_context Pte;
    volatile LONG Calls[BC250_VIDMM_LEVELS];
    volatile LONG CpuCalls, GpuCalls;       // by update mode: CPU_VIRTUAL (the paging process) and GPU_PHYSICAL
    volatile LONG64 Entries[BC250_VIDMM_LEVELS];
    volatile LONG64 Valid[BC250_VIDMM_LEVELS];
    volatile LONG64 Written;
    volatile LONG Refused;
    volatile LONG BadCalls;
    volatile LONG SegmentSeen[BC250_VIDMM_SEGMENT_IDS];
    volatile LONG Roots;
} BC250_VIDMM;

static BC250_VIDMM g_VidMm;

// dxgmms2 numbers the levels from the leaf: level 0 names pages, everything above names a lower table
// (bc250_pte.h question 3; the GPU virtual address page of the WDK documentation).
static enum bc250_pte_kind VidMmKind(UINT Level) { return Level == 0 ? BC250_PTE_LEAF : BC250_PTE_DIRECTORY; }

void VidMmStart(_In_ const BC250_DEVICE* Device, ULONGLONG SegmentOffset, ULONGLONG SegmentLength, ULONG VramSegmentId)
{
    RtlZeroMemory(&g_VidMm, sizeof(g_VidMm));
    if (!Device->FullWddm || !Device->VramEnabled || SegmentLength == 0) return;

    g_VidMm.SegmentPhysical = (ULONGLONG)Device->VramPhysical.QuadPart + SegmentOffset;
    g_VidMm.SegmentLength = SegmentLength;
    // Settled by E18 run 001: PageAddress is a page frame number (a level 1 entry 0x1FD732 names the level 0 table at
    // segment offset 0x1FD732000), host memory is segment 0, level 0 is the leaf and level 3 the root.
    g_VidMm.Pte.units = BC250_PTE_ADDR_PAGES;
    g_VidMm.Pte.aperture = BC250_PTE_VM;
    g_VidMm.Pte.system_segment = 0;
    g_VidMm.Pte.vram_segment = VramSegmentId;
    // The walker wants physical addresses for VRAM pages and tables, not MC ones: amdgpu converts with
    // amdgpu_gmc_vram_mc2pa() (mc - vram_start + vram_base_offset) in gmc_v10_0_get_vm_pde()/get_vm_pte(), and
    // vram_base_offset is the FB offset register, which is what vram.c keeps as VramPhysical.
    g_VidMm.Pte.vram_base = g_VidMm.SegmentPhysical;
    g_VidMm.Pte.vram_size = SegmentLength;
    g_VidMm.Pte.system_limit = 0;
    g_VidMm.Ready = TRUE;
    g_VidMm.Write = (GuardReadSetting(L"EnableGpuVa", 0) == 1) && Device->VramWriteEnabled;
    GuardLog("vidmm: segment %u at physical 0x%llX + 0x%llX, page table updates are %s", VramSegmentId,
             g_VidMm.SegmentPhysical, SegmentLength, g_VidMm.Write ? "WRITTEN" : "planned only (EnableGpuVa closed)");
}

void VidMmStop(void)
{
    g_VidMm.Ready = FALSE;
    g_VidMm.Write = FALSE;
}

// PASSIVE_LEVEL (BuildPagingBuffer). Never fails: what goes wrong is counted and logged (ADR 0008 point 5).
//
// Two kinds of call arrive, whatever GPUMMUCAPS says (E18 run 001): the page tables of the system paging process are
// updated in DXGK_PAGETABLEUPDATE_CPU_VIRTUAL mode - VidMm has the table page mapped and hands over the pointer, 1028
// calls at the start - and everything after that comes in the declared GPU_PHYSICAL mode, segment and offset.
void VidMmUpdatePageTable(_In_ const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE* Update)
{
    BC250_VIDMM* vm = &g_VidMm;
    UINT level = Update->PageTableLevel;
    const DXGK_PTE* source = Update->pPageTableEntries;
    BOOLEAN cpu = (Update->UpdateMode == DXGK_PAGETABLEUPDATE_CPU_VIRTUAL);
    ULONGLONG tableOffset = cpu ? 0 : Update->PageTableAddress.GpuPhysical.SegmentOffset;
    ULONG tableSegment = cpu ? 0 : Update->PageTableAddress.GpuPhysical.SegmentId;
    volatile ULONGLONG* table = NULL;
    BOOLEAN logCall, bad;
    u64 firstEntry = 0;
    UINT i;

    C_ASSERT(sizeof(Update->Flags) == sizeof(UINT));    // the raw word below is read through a cast
    if (!vm->Ready || vm->SegmentLength < PAGE_SIZE) return;
    bad = level >= BC250_VIDMM_LEVELS || source == NULL || Update->NumPageTableEntries == 0 ||
          Update->StartIndex >= BC250_VIDMM_PTES || Update->NumPageTableEntries > BC250_VIDMM_PTES - Update->StartIndex;
    if (!bad && cpu)
        bad = Update->PageTableAddress.CpuVirtual == NULL || ((ULONG_PTR)Update->PageTableAddress.CpuVirtual & (PAGE_SIZE - 1)) != 0;
    else if (!bad)
        bad = Update->UpdateMode != DXGK_PAGETABLEUPDATE_GPU_PHYSICAL || tableSegment != vm->Pte.vram_segment ||
              (tableOffset & (PAGE_SIZE - 1)) != 0 || tableOffset > vm->SegmentLength - PAGE_SIZE;
    if (bad)
    {
        if (InterlockedIncrement(&vm->BadCalls) <= BC250_VIDMM_LOG_REFUSALS)
            GuardLog("vidmm: UpdatePageTable REFUSED as a call: level %u mode %u table 0x%llX / 0x%llX start %u count %u", level,
                     (ULONG)Update->UpdateMode, (ULONGLONG)(ULONG_PTR)Update->PageTableAddress.CpuVirtual,
                     Update->PageTableAddress.GpuPhysical.SegmentOffset, Update->StartIndex, Update->NumPageTableEntries);
        return;
    }

    InterlockedIncrement(&vm->Calls[level]);
    logCall = InterlockedIncrement(cpu ? &vm->CpuCalls : &vm->GpuCalls) <= BC250_VIDMM_LOG_CALLS * 2;
    if (logCall)
        GuardLog("vidmm: level %u %s 0x%llX start %u count %u flags 0x%X first raw 0x%llX / 0x%llX", level,
                 cpu ? "cpu table" : "table 1:", cpu ? (ULONGLONG)(ULONG_PTR)Update->PageTableAddress.CpuVirtual : tableOffset,
                 Update->StartIndex, Update->NumPageTableEntries, *(const UINT*)&Update->Flags, source[0].Flags,
                 source[0].PageAddress);

    if (vm->Write && cpu)
    {
        // VidMm's own mapping of the table page, valid for this call by its contract (CosKmd writes through it too).
        table = (volatile ULONGLONG*)Update->PageTableAddress.CpuVirtual;
    }
    else if (vm->Write)
    {
        PHYSICAL_ADDRESS physical;

        physical.QuadPart = (LONGLONG)(vm->SegmentPhysical + tableOffset);
        table = (volatile ULONGLONG*)MmMapIoSpaceEx(physical, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
        if (table == NULL && InterlockedIncrement(&vm->BadCalls) <= BC250_VIDMM_LOG_REFUSALS)
            GuardLog("vidmm: no mapping for the table page at 0x%llX, update dropped", physical.QuadPart);
    }

    __try
    {
        for (i = 0; i < Update->NumPageTableEntries; i++)
        {
            const DXGK_PTE* pte = Update->Flags.Repeat ? source : source + i;
            ULONG segment = (ULONG)pte->Segment;
            u64 entry = 0;
            int rc = bc250_pte_from_dxgk(&vm->Pte, VidMmKind(level), pte->Flags, pte->PageAddress, &entry);

            if (i == 0) firstEntry = entry;
            InterlockedIncrement64(&vm->Entries[level]);
            if (pte->Valid)
            {
                InterlockedIncrement64(&vm->Valid[level]);
                if (InterlockedIncrement(&vm->SegmentSeen[segment]) == 1)
                    GuardLog("vidmm: first valid entry of segment %u: level %u raw 0x%llX / 0x%llX -> 0x%llX rc %d", segment,
                             level, pte->Flags, pte->PageAddress, entry, rc);
            }
            if (rc != 0 && InterlockedIncrement(&vm->Refused) <= BC250_VIDMM_LOG_REFUSALS)
                GuardLog("vidmm: entry REFUSED (rc %d): level %u index %u raw 0x%llX / 0x%llX", rc, level,
                         Update->StartIndex + i, pte->Flags, pte->PageAddress);
            // A refused entry is written as 0: absent, which is how this hardware spells invalid (bc250_pte.c).
            if (table != NULL)
            {
                table[Update->StartIndex + i] = entry;
                InterlockedIncrement64(&vm->Written);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // Only a user-range pointer can end up here; a bad kernel address is not catchable and is VidMm's to keep valid.
        if (InterlockedIncrement(&vm->BadCalls) <= BC250_VIDMM_LOG_REFUSALS)
            GuardLog("vidmm: exception 0x%08X writing a %s table, level %u", (ULONG)GetExceptionCode(), cpu ? "cpu" : "segment", level);
    }
    if (logCall) GuardLog("vidmm:   -> first entry 0x%llX%s", firstEntry, table != NULL ? " (written)" : "");
    if (table != NULL && !cpu) MmUnmapIoSpace((PVOID)table, PAGE_SIZE);
}

void VidMmSetRootPageTable(_In_ const DXGKARG_SETROOTPAGETABLE* Root)
{
    BC250_VIDMM* vm = &g_VidMm;

    if (!vm->Ready) return;
    // Stage B programs no VM context: the root is where the witness starts its walk, and stage C's first job.
    if (InterlockedIncrement(&vm->Roots) <= BC250_VIDMM_LOG_CALLS)
        GuardLog("vidmm: root page table %u:0x%llX = physical 0x%llX, %u entries (no VMID programmed in stage B)",
                 Root->Address.SegmentId, Root->Address.SegmentOffset, vm->SegmentPhysical + Root->Address.SegmentOffset,
                 Root->NumEntries);
}

void VidMmSummary(void)
{
    BC250_VIDMM* vm = &g_VidMm;
    UINT level, segment;

    if (!vm->Ready) { GuardLog("vidmm summary: not running"); return; }
    for (level = 0; level < BC250_VIDMM_LEVELS; level++)
        GuardLog("vidmm summary: level %u: %ld calls, %lld entries, %lld valid", level, vm->Calls[level],
                 vm->Entries[level], vm->Valid[level]);
    for (segment = 0; segment < BC250_VIDMM_SEGMENT_IDS; segment++)
        if (vm->SegmentSeen[segment] != 0)
            GuardLog("vidmm summary: segment %u named by %ld valid entries", segment, vm->SegmentSeen[segment]);
    GuardLog("vidmm summary: %ld cpu-virtual calls, %ld gpu-physical calls", vm->CpuCalls, vm->GpuCalls);
    GuardLog("vidmm summary: %lld entries written (%s), %ld refused, %ld bad calls, %ld roots", vm->Written,
             vm->Write ? "EnableGpuVa open" : "plan only", vm->Refused, vm->BadCalls, vm->Roots);
}
