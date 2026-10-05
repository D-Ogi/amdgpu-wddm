// GPU_PHYSICAL updates now pass through VidMmEncodePageTable and the SDMA builder
// after the first engine RUN. The CPU routine below remains for CPU_VIRTUAL and
// the serialized pre-RUN bootstrap. Internal status propagates mapping failures.
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
#include "paging_pt_shadow.h"
#include "paging_aperture_state.h"

#define BC250_VIDMM_SHADOW_TAG 'sV2B'
// WDDM2 system paging process VA is fixed at1GiB (MS System Paging Process).
#define BC250_VIDMM_PAGING_VA_BYTES (1ull<<30)

#define BC250_VIDMM_LEVELS 4u
#define BC250_VIDMM_PTES 512u
#define BC250_VIDMM_LOG_CALLS 4             // first calls of each level that reach the log
#define BC250_VIDMM_LOG_REFUSALS 6
#define BC250_VIDMM_SEGMENT_IDS 32u         // DXGK_PTE.Segment is five bits wide

typedef struct _BC250_VIDMM {
    PAGING_APERTURE_STATE Aperture;
    PAGING_PT_SHADOW Shadow;             // logical construction state, pinned paging tables only
    EX_PUSH_LOCK CpuUpdateLock;             // serializes the embedded CPU snapshot
    ULONGLONG CpuEntries[BC250_VIDMM_PTES]; // no allocation in BuildPagingBuffer
    BOOLEAN Ready;                          // the segment is known: translation means something
    BOOLEAN Write;                          // EnableGpuVa = 1 and VRAM writes allowed
    ULONGLONG SegmentPhysical;              // system physical address of segment offset 0
    ULONGLONG SegmentLength;
    PUCHAR SegmentMapping;                  // NC mapping retained from start through CPU-reader drain
    struct bc250_pte_context Pte;
    volatile LONG Calls[BC250_VIDMM_LEVELS];
    volatile LONG CpuCalls, GpuCalls;       // by update mode: CPU_VIRTUAL (the paging process) and GPU_PHYSICAL
    volatile LONG64 Entries[BC250_VIDMM_LEVELS];
    volatile LONG64 Valid[BC250_VIDMM_LEVELS];
    volatile LONG64 Written;
    volatile LONG64 EncodedCoherentSystem, EncodedUncachedSystem, EncodedCoherencyMismatch;
    volatile LONG Refused;
    volatile LONG BadCalls;
    volatile LONG SegmentSeen[BC250_VIDMM_SEGMENT_IDS];
    volatile LONG Roots;
} BC250_VIDMM;

static BC250_VIDMM g_VidMm;

// dxgmms2 numbers the levels from the leaf: level 0 names pages, everything above names a lower table
// (bc250_pte.h question 3; the GPU virtual address page of the WDK documentation).
static enum bc250_pte_kind VidMmKind(UINT Level) { return Level == 0 ? BC250_PTE_LEAF : BC250_PTE_DIRECTORY; }

// One past the highest byte of RAM the OS owns: a host page named above that is refused, not translated. 0 (do not
// check) if the OS will not say. PASSIVE_LEVEL.
static ULONGLONG VidMmSystemLimit(void)
{
    PPHYSICAL_MEMORY_RANGE ranges = MmGetPhysicalMemoryRanges();
    ULONGLONG limit = 0;
    ULONG i;

    if (ranges == NULL) return 0;
    for (i = 0; ranges[i].BaseAddress.QuadPart != 0 || ranges[i].NumberOfBytes.QuadPart != 0; i++)
    {
        ULONGLONG end = (ULONGLONG)ranges[i].BaseAddress.QuadPart + (ULONGLONG)ranges[i].NumberOfBytes.QuadPart;
        if (end > limit) limit = end;
    }
    ExFreePool(ranges);
    return limit;
}

NTSTATUS VidMmStartLayout(_In_ const BC250_DEVICE* Device, ULONGLONG SegmentOffset, ULONGLONG SegmentLength, ULONG VramSegmentId,
                          ULONGLONG TableOffset, ULONGLONG TableLength, ULONG TableSegmentId)
{
    RtlZeroMemory(&g_VidMm, sizeof(g_VidMm));
    ExInitializePushLock(&g_VidMm.CpuUpdateLock);
    if (!Device->FullWddm || !Device->VramEnabled || SegmentLength == 0) return STATUS_SUCCESS;
    if (((SegmentOffset|SegmentLength) & (PAGE_SIZE-1)) != 0 ||
        SegmentOffset > Device->VramLength || SegmentLength > Device->VramLength-SegmentOffset ||
        (ULONGLONG)(SIZE_T)SegmentLength != SegmentLength ||
        (ULONGLONG)Device->VramPhysical.QuadPart > MAXULONGLONG-SegmentOffset-SegmentLength)
        return STATUS_INVALID_PARAMETER;

    if (!TableLength || ((TableOffset|TableLength)&(PAGE_SIZE-1))!=0 ||
        TableOffset>Device->VramLength || TableLength>Device->VramLength-TableOffset ||
        (ULONGLONG)(SIZE_T)TableLength!=TableLength ||
        (ULONGLONG)Device->VramPhysical.QuadPart>MAXULONGLONG-TableOffset-TableLength ||
        VramSegmentId==0 || VramSegmentId>31 || TableSegmentId==0 || TableSegmentId>31)
        return STATUS_INVALID_PARAMETER;
    if (TableSegmentId==VramSegmentId) {
        if (TableOffset!=SegmentOffset || TableLength!=SegmentLength) return STATUS_INVALID_PARAMETER;
    } else if (TableOffset<SegmentOffset+SegmentLength && SegmentOffset<TableOffset+TableLength)
        return STATUS_INVALID_PARAMETER;
    // SegmentMapping covers table storage only. Application leaf addresses are
    // independently bounded by Pte.vram_base/vram_size below.
    g_VidMm.SegmentPhysical = (ULONGLONG)Device->VramPhysical.QuadPart + TableOffset;
    g_VidMm.SegmentLength = TableLength;
    // Settled by E18 run 001: PageAddress is a page frame number (a level 1 entry 0x1FD732 names the level 0 table at
    // segment offset 0x1FD732000), host memory is segment 0, level 0 is the leaf and level 3 the root.
    g_VidMm.Pte.units = BC250_PTE_ADDR_PAGES;
    g_VidMm.Pte.aperture = BC250_PTE_VM;
    g_VidMm.Pte.system_segment = 0;
    g_VidMm.Pte.vram_segment = VramSegmentId;
    // The walker wants physical addresses for VRAM pages and tables, not MC ones: amdgpu converts with
    // amdgpu_gmc_vram_mc2pa() (mc - vram_start + vram_base_offset) in gmc_v10_0_get_vm_pde()/get_vm_pte(), and
    // vram_base_offset is the FB offset register, which is what vram.c keeps as VramPhysical.
    g_VidMm.Pte.vram_base = (ULONGLONG)Device->VramPhysical.QuadPart + SegmentOffset;
    g_VidMm.Pte.vram_size = SegmentLength;
    if (TableSegmentId!=VramSegmentId) {
        g_VidMm.Pte.table_segment=TableSegmentId;
        g_VidMm.Pte.table_base=g_VidMm.SegmentPhysical;
        g_VidMm.Pte.table_size=TableLength;
    }
    g_VidMm.Pte.system_limit = VidMmSystemLimit();
    g_VidMm.Write = (GuardReadSetting(L"EnableGpuVa", 0) == 1) && Device->VramWriteEnabled;
    // Page-table update/walk paths must not allocate a mapping while building
    // DMA. Establish the bounded table extent once at start.
    if (g_VidMm.Write) {
        PHYSICAL_ADDRESS physical;
        unsigned tables=PagingPtShadowTableCount(BC250_VIDMM_PAGING_VA_BYTES,BC250_VIDMM_LEVELS);
        unsigned slots=tables*2u+1u; // <=50% occupancy; no growth during paging callbacks
        PAGING_PT_SHADOW_SLOT* storage;
        if (tables==0) return STATUS_INVALID_PARAMETER;
        storage=(PAGING_PT_SHADOW_SLOT*)ExAllocatePool2(POOL_FLAG_PAGED,
            (SIZE_T)slots*sizeof(*storage),BC250_VIDMM_SHADOW_TAG);
        if (storage==NULL) { g_VidMm.Write=FALSE; return STATUS_INSUFFICIENT_RESOURCES; }
        (void)PagingPtShadowInit(&g_VidMm.Shadow,storage,slots);
        GuardLog("vidmm: logical paging state %u tables, %u slots, %llu bytes reserved",tables,slots,
                 (ULONGLONG)((SIZE_T)slots*sizeof(*storage)));
        physical.QuadPart = (LONGLONG)g_VidMm.SegmentPhysical;
        g_VidMm.SegmentMapping = (PUCHAR)MmMapIoSpaceEx(physical,(SIZE_T)TableLength,PAGE_READWRITE|PAGE_NOCACHE);
        if (g_VidMm.SegmentMapping == NULL) {
            ExFreePoolWithTag(g_VidMm.Shadow.Slots,BC250_VIDMM_SHADOW_TAG);
            g_VidMm.Shadow.Slots=NULL;
            g_VidMm.Write = FALSE;
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    if (g_VidMm.Write && Device->WddmAperture.bytes) {
        unsigned pages=(unsigned)(PAGING_APERTURE_BYTES/PAGE_SIZE);
        ULONGLONG* storage=(ULONGLONG*)ExAllocatePool2(POOL_FLAG_PAGED,
            (SIZE_T)pages*sizeof(ULONGLONG),BC250_VIDMM_SHADOW_TAG);
        if (!storage) return STATUS_INSUFFICIENT_RESOURCES;
        if (!PagingApertureStateInit(&g_VidMm.Aperture,&Device->WddmAperture,storage,pages)) {
            ExFreePoolWithTag(storage,BC250_VIDMM_SHADOW_TAG);return STATUS_INVALID_PARAMETER;
        }
    }
    g_VidMm.Ready = TRUE;
    GuardLog("vidmm: table segment %u at physical 0x%llX + 0x%llX, host memory below 0x%llX, updates are %s", TableSegmentId,
             g_VidMm.SegmentPhysical, TableLength, g_VidMm.Pte.system_limit, g_VidMm.Write ? "WRITTEN" : "planned only (EnableGpuVa closed)");
    return STATUS_SUCCESS;
}

NTSTATUS VidMmStart(_In_ const BC250_DEVICE* Device, ULONGLONG SegmentOffset, ULONGLONG SegmentLength, ULONG VramSegmentId)
{
    return VidMmStartLayout(Device,SegmentOffset,SegmentLength,VramSegmentId,
                           SegmentOffset,SegmentLength,VramSegmentId);
}

void VidMmStop(void)
{
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&g_VidMm.CpuUpdateLock);
    g_VidMm.Ready = FALSE;
    g_VidMm.Write = FALSE;
    if (g_VidMm.SegmentMapping != NULL) {
        MmUnmapIoSpace(g_VidMm.SegmentMapping,(SIZE_T)g_VidMm.SegmentLength);
        g_VidMm.SegmentMapping = NULL;
    }
    if (g_VidMm.Aperture.entries!=NULL) {
        ExFreePoolWithTag(g_VidMm.Aperture.entries,BC250_VIDMM_SHADOW_TAG);
        RtlZeroMemory(&g_VidMm.Aperture,sizeof(g_VidMm.Aperture));
    }
    if (g_VidMm.Shadow.Slots!=NULL) {
        ExFreePoolWithTag(g_VidMm.Shadow.Slots,BC250_VIDMM_SHADOW_TAG);
        RtlZeroMemory(&g_VidMm.Shadow,sizeof(g_VidMm.Shadow));
    }
    ExReleasePushLockExclusive(&g_VidMm.CpuUpdateLock);
    KeLeaveCriticalRegion();
}

// Called only after DMA/private capacity has been accepted, before publication.
// Copy page identities under the same lock as other logical paging state. The
// MDL is OS-owned for this callback; no pointer is retained beyond it.
NTSTATUS VidMmCommitPagingAperture(const DXGKARG_BUILDPAGINGBUFFER* Build, ULONG Start, ULONG Next)
{
    ULONGLONG first,total,span,begin,pages[PAGING_APERTURE_BATCH_PAGES];
    ULONG count,i;
    PMDL mdl=NULL;
    BOOLEAN unmap;
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    if (!Build || Next<=Start || Next-Start>PAGING_APERTURE_BATCH_PAGES) return status;
    count=Next-Start;unmap=Build->Operation==DXGK_OPERATION_UNMAP_APERTURE_SEGMENT;
    if (unmap) {
        first=Build->UnmapApertureSegment.OffsetInPages;
        total=Build->UnmapApertureSegment.NumberOfPages;
    } else if (Build->Operation==DXGK_OPERATION_MAP_APERTURE_SEGMENT) {
        first=Build->MapApertureSegment.OffsetInPages;
        total=Build->MapApertureSegment.NumberOfPages;
        mdl=Build->MapApertureSegment.pMdl;
        if (!mdl || !MmGetMdlByteCount(mdl)) return status;
        begin=MmGetMdlByteOffset(mdl);
        if (begin>=PAGE_SIZE) return status;
        span=(begin+(ULONGLONG)MmGetMdlByteCount(mdl)+PAGE_SIZE-1)>>PAGE_SHIFT;
        if (Build->MapApertureSegment.MdlOffset>span ||
            Next>span-Build->MapApertureSegment.MdlOffset) return status;
        for (i=0;i<count;i++) {
            ULONGLONG pfn=MmGetMdlPfnArray(mdl)[Build->MapApertureSegment.MdlOffset+Start+i];
            if (pfn>(MAXULONGLONG>>PAGE_SHIFT)) return status;
            pages[i]=pfn<<PAGE_SHIFT;
        }
    } else return status;
    if (Next>total || first>=PAGING_APERTURE_BYTES/PAGE_SIZE ||
        Next>PAGING_APERTURE_BYTES/PAGE_SIZE-first) return status;
    KeEnterCriticalRegion();ExAcquirePushLockExclusive(&g_VidMm.CpuUpdateLock);
    if (!g_VidMm.Ready || !g_VidMm.Write) status=STATUS_DEVICE_NOT_READY;
    else if (unmap ? PagingApertureStateUnmap(&g_VidMm.Aperture,(unsigned)(first+Start),count) :
             PagingApertureStateMap(&g_VidMm.Aperture,(unsigned)(first+Start),count,pages,~4095ull))
        status=STATUS_SUCCESS;
    ExReleasePushLockExclusive(&g_VidMm.CpuUpdateLock);KeLeaveCriticalRegion();
    return status;
}

BOOLEAN VidMmResolveAperture(ULONGLONG Mc, ULONG Bytes, ULONGLONG* Physical)
{
    BOOLEAN result;
    *Physical=0;
    KeEnterCriticalRegion();ExAcquirePushLockShared(&g_VidMm.CpuUpdateLock);
    result=g_VidMm.Ready && g_VidMm.Write &&
        PagingApertureStateResolve(&g_VidMm.Aperture,Mc,Bytes,Physical);
    ExReleasePushLockShared(&g_VidMm.CpuUpdateLock);KeLeaveCriticalRegion();
    return result;
}

BOOLEAN VidMmApertureRangeValid(ULONGLONG Mc, ULONGLONG Bytes)
{
    ULONGLONG offset,physical;BOOLEAN result=FALSE;
    if (!Bytes || Mc>MAXULONGLONG-Bytes) return FALSE;
    KeEnterCriticalRegion();ExAcquirePushLockShared(&g_VidMm.CpuUpdateLock);
    if (!g_VidMm.Ready || !g_VidMm.Write || Mc<g_VidMm.Aperture.aperture.mc) goto Done;
    offset=Mc-g_VidMm.Aperture.aperture.mc;
    if (offset>=g_VidMm.Aperture.aperture.bytes || Bytes>g_VidMm.Aperture.aperture.bytes-offset) goto Done;
    while (Bytes) {
        ULONG count=PAGE_SIZE-(ULONG)(Mc&(PAGE_SIZE-1));
        if (count>Bytes) count=(ULONG)Bytes;
        if (!PagingApertureStateResolve(&g_VidMm.Aperture,Mc,count,&physical)) goto Done;
        Bytes-=count;Mc+=count;
    }
    result=TRUE;
Done:
    ExReleasePushLockShared(&g_VidMm.CpuUpdateLock);KeLeaveCriticalRegion();
    return result;
}

// PASSIVE_LEVEL. Immediate CPU_VIRTUAL initialization, or the serialized
// pre-RUN GPU_PHYSICAL bootstrap only. Encode a bounded snapshot before touching
// the table: a bad later entry must not leave a partly updated table. The wrapper
// serializes the embedded snapshot; no pool allocation or4KiB stack array here.
static NTSTATUS VidMmUpdatePageTableLocked(_In_ const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE* Update)
{
    BC250_VIDMM* vm = &g_VidMm;
    UINT level, i;
    BOOLEAN cpu, logCall;
    ULONGLONG tableOffset,tablePhysical=0;
    int shadowStatus=PAGING_PT_MISSING;
    ULONGLONG* entries = vm->CpuEntries;
    volatile ULONGLONG* table = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (Update == NULL) return STATUS_INVALID_PARAMETER;
    if (!vm->Ready || !vm->Write || vm->SegmentLength < PAGE_SIZE) return STATUS_DEVICE_NOT_READY;
    level = Update->PageTableLevel;
    cpu = Update->UpdateMode == DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;
    tableOffset = cpu ? 0 : Update->PageTableAddress.GpuPhysical.SegmentOffset;
    if (level >= BC250_VIDMM_LEVELS || Update->Flags.Use64KBPages ||
        Update->pPageTableEntries == NULL || Update->NumPageTableEntries == 0 ||
        Update->StartIndex >= BC250_VIDMM_PTES ||
        Update->NumPageTableEntries > BC250_VIDMM_PTES - Update->StartIndex ||
        (cpu ? (Update->PageTableAddress.CpuVirtual == NULL ||
                ((ULONG_PTR)Update->PageTableAddress.CpuVirtual & (PAGE_SIZE-1)) != 0) :
               (Update->UpdateMode != DXGK_PAGETABLEUPDATE_GPU_PHYSICAL ||
                Update->PageTableAddress.GpuPhysical.SegmentId != (vm->Pte.table_size ? vm->Pte.table_segment : vm->Pte.vram_segment) ||
                (tableOffset & (PAGE_SIZE-1)) != 0 || tableOffset > vm->SegmentLength-PAGE_SIZE))) {
        InterlockedIncrement(&vm->BadCalls);
        return STATUS_INVALID_PARAMETER;
    }
    InterlockedIncrement(&vm->Calls[level]);
    logCall = InterlockedIncrement(cpu ? &vm->CpuCalls : &vm->GpuCalls) <= BC250_VIDMM_LOG_CALLS*2;
    __try {
        for (i=0; i<Update->NumPageTableEntries; i++) {
            const DXGK_PTE* pte = Update->pPageTableEntries + (Update->Flags.Repeat ? 0 : i);
            if (bc250_pte_from_dxgk(&vm->Pte,VidMmKind(level),pte->Flags,pte->PageAddress,&entries[i]) != 0) {
                InterlockedIncrement(&vm->Refused);
                status = STATUS_INVALID_PARAMETER;
                break;
            }
            InterlockedIncrement64(&vm->Entries[level]);
            if (pte->Flags & BC250_DXGK_PTE_VALID) InterlockedIncrement64(&vm->Valid[level]);
        }
        if (NT_SUCCESS(status)) {
            if (cpu) table = (volatile ULONGLONG*)Update->PageTableAddress.CpuVirtual;
            else {
                if (vm->SegmentMapping == NULL) status = STATUS_DEVICE_NOT_READY;
                else table = (volatile ULONGLONG*)(vm->SegmentMapping+(SIZE_T)tableOffset);
            }
            if (table != NULL) {
                // CPU_VIRTUAL borrows a valid mapping of a pinned local table.
                // Obtain only its identity for the logical index, not a DMA address.
                // Do not retain the borrowed pointer after this callback.
                tablePhysical=cpu ? (ULONGLONG)MmGetPhysicalAddress((PVOID)table).QuadPart : vm->SegmentPhysical+tableOffset;
                // A borrowed CPU mapping must still name a whole local table
                // inside the configured table extent before either write occurs.
                if ((tablePhysical & (PAGE_SIZE-1))!=0 || tablePhysical<vm->SegmentPhysical ||
                    tablePhysical-vm->SegmentPhysical>vm->SegmentLength-PAGE_SIZE)
                    shadowStatus=PAGING_PT_INVALID;
                else shadowStatus=PagingPtShadowCanApply(&vm->Shadow,tablePhysical,Update->StartIndex,
                                                        Update->NumPageTableEntries,cpu);
                // CPU_VIRTUAL is paging-process initialization. An unregistered
                // GPU_PHYSICAL bootstrap table belongs to another process.
                if (shadowStatus!=PAGING_PT_OK && !(shadowStatus==PAGING_PT_MISSING && !cpu)) {
                    status=shadowStatus==PAGING_PT_FULL ? STATUS_INSUFFICIENT_RESOURCES : STATUS_INVALID_PARAMETER;
                    table=NULL; // preflight fails before either destination is modified
                }
            }
            if (table != NULL) {
                for (i=0; i<Update->NumPageTableEntries; i++) {
                    table[Update->StartIndex+i] = entries[i];
                    InterlockedIncrement64(&vm->Written);
                }
                KeMemoryBarrier();
                if (shadowStatus==PAGING_PT_OK)
                    (void)PagingPtShadowApply(&vm->Shadow,tablePhysical,Update->StartIndex,
                                             Update->NumPageTableEntries,entries,cpu);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // The OS must keep kernel pointers valid. This handles catchable faults,
        // not rollback after an invalid destination pointer faults mid-write.
        status = GetExceptionCode();
    }
    if (!NT_SUCCESS(status)) InterlockedIncrement(&vm->BadCalls);
    if (logCall || !NT_SUCCESS(status))
        GuardLog("vidmm: CPU update level %u mode %u start %u count %u status 0x%08X",level,
                 (ULONG)Update->UpdateMode,Update->StartIndex,Update->NumPageTableEntries,status);
    return status;
}

// PASSIVE_LEVEL. Lock order: optional device GfxPagingLock first, then this
// snapshot lock. No path under CpuUpdateLock acquires the device lock. Stop joins
// active CPU updates before closing gates. PnP serializes start/reinitialization.
NTSTATUS VidMmUpdatePageTable(_In_ const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE* Update)
{
    NTSTATUS status;
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&g_VidMm.CpuUpdateLock);
    status = VidMmUpdatePageTableLocked(Update);
    ExReleasePushLockExclusive(&g_VidMm.CpuUpdateLock);
    KeLeaveCriticalRegion();
    return status;
}

void VidMmSetRootPageTable(_In_ const DXGKARG_SETROOTPAGETABLE* Root)
{
    BC250_VIDMM* vm = &g_VidMm;

    if (!vm->Ready) return;
    // Stage B programs no VM context: the root is where the witness starts its walk, and stage C's first job.
    if (InterlockedIncrement(&vm->Roots) <= BC250_VIDMM_LOG_CALLS)
        GuardLog("vidmm: root page table %u:0x%llX = physical 0x%llX, %u entries (the VMID is pointed at it by the first packet)",
                 Root->Address.SegmentId, Root->Address.SegmentOffset, vm->SegmentPhysical + Root->Address.SegmentOffset,
                 Root->NumEntries);
}

// Encode a complete validated source update, retaining only this batch's entries.
// No CPU table writes: the caller queues the returned values on SDMA. Validating
// the whole source before emitting avoids accepting a malformed later entry after
// an earlier chunk has already been submitted. The OS owns source lifetime here.
BOOLEAN VidMmEncodePageTable(_In_ const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE* Update,
                            ULONG Start, ULONG Count, _Out_ ULONGLONG* Physical,
                            _Out_writes_(Count) ULONGLONG* Entries)
{
    BC250_VIDMM* vm = &g_VidMm;
    UINT i;
    ULONGLONG table;
    if (Update == NULL || Entries == NULL || Physical == NULL ||
        Update->UpdateMode != DXGK_PAGETABLEUPDATE_GPU_PHYSICAL ||
        Update->PageTableLevel >= BC250_VIDMM_LEVELS || Update->Flags.Use64KBPages ||
        Update->pPageTableEntries == NULL || Update->NumPageTableEntries == 0 ||
        Update->StartIndex >= BC250_VIDMM_PTES ||
        Update->NumPageTableEntries > BC250_VIDMM_PTES - Update->StartIndex ||
        Start > Update->NumPageTableEntries || Count == 0 || Count > Update->NumPageTableEntries - Start ||
        !VidMmRootPhysical(&Update->PageTableAddress.GpuPhysical,&table)) return FALSE;
    for (i = 0; i < Update->NumPageTableEntries; i++) {
        const DXGK_PTE* pte = Update->pPageTableEntries + (Update->Flags.Repeat ? 0 : i);
        u64 entry;
        if (bc250_pte_from_dxgk(&vm->Pte,VidMmKind(Update->PageTableLevel),
                               pte->Flags,pte->PageAddress,&entry) != 0) return FALSE;
        if (i >= Start && i - Start < Count) Entries[i - Start] = entry;
    }
    // Diagnostics count successful encoding attempts, including re-encoding at
    // logical publication. They do not imply GPU execution or page residency.
    if (Update->PageTableLevel==0) {
        LONG64 coherent=0,uncached=0,mismatch=0;
        for (i=0;i<Count;i++) {
            ULONGLONG entry=Entries[i];
            const DXGK_PTE* pte=Update->pPageTableEntries+(Update->Flags.Repeat?0:Start+i);
            if ((entry&(AMDGPU_PTE_VALID|AMDGPU_PTE_SYSTEM))!=(AMDGPU_PTE_VALID|AMDGPU_PTE_SYSTEM)) continue;
            if (pte->Flags&BC250_DXGK_PTE_CACHECOHERENT) coherent++; else uncached++;
            if (((pte->Flags&BC250_DXGK_PTE_CACHECOHERENT)!=0)!=((entry&AMDGPU_PTE_SNOOPED)!=0)) mismatch++;
        }
        if(coherent)InterlockedAdd64(&vm->EncodedCoherentSystem,coherent);
        if(uncached)InterlockedAdd64(&vm->EncodedUncachedSystem,uncached);
        if(mismatch)InterlockedAdd64(&vm->EncodedCoherencyMismatch,mismatch);
    }
    *Physical = table + ((ULONGLONG)Update->StartIndex + Start) * sizeof(ULONGLONG);
    return TRUE;
}

// Commit only a successfully constructed and capacity-accepted GPU batch.
// Source entries remain OS-owned and immutable throughout BuildPagingBuffer.
NTSTATUS VidMmCommitPagingUpdate(_In_ const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE* Update,
                                 ULONG Start, ULONG Count)
{
    BC250_VIDMM* vm=&g_VidMm;
    ULONGLONG physical;
    int result;
    NTSTATUS status=STATUS_SUCCESS;
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&vm->CpuUpdateLock);
    if (!vm->Ready || !vm->Write || vm->Shadow.Slots==NULL) status=STATUS_DEVICE_NOT_READY;
    else if (!VidMmEncodePageTable(Update,Start,Count,&physical,vm->CpuEntries)) status=STATUS_INVALID_PARAMETER;
    else {
        result=PagingPtShadowApply(&vm->Shadow,physical & ~(ULONGLONG)(PAGE_SIZE-1),
            (unsigned)((physical & (PAGE_SIZE-1))/sizeof(ULONGLONG)),Count,vm->CpuEntries,0);
        // Only pinned paging-process tables were registered during CPU initialization.
        // Ordinary process updates intentionally have no entry in this logical view.
        if (result!=PAGING_PT_OK && result!=PAGING_PT_MISSING) status=STATUS_INVALID_PARAMETER;
    }
    ExReleasePushLockExclusive(&vm->CpuUpdateLock);
    KeLeaveCriticalRegion();
    return status;
}

// Physical entry identities come from a capacity-accepted copy range. Only
// registered pinned paging tables participate; no allocation or GPU write here.
NTSTATUS VidMmCommitPagingCopy(ULONGLONG Source, ULONGLONG Destination, ULONG Count)
{
    BC250_VIDMM* vm=&g_VidMm;
    NTSTATUS status=STATUS_SUCCESS;
    int result;
    if (((Source|Destination)&7ull)!=0) return STATUS_INVALID_PARAMETER;
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&vm->CpuUpdateLock);
    if (!vm->Ready || !vm->Write || vm->Shadow.Slots==NULL) status=STATUS_DEVICE_NOT_READY;
    else {
        result=PagingPtShadowCopy(&vm->Shadow,Source & ~(ULONGLONG)(PAGE_SIZE-1),
            (unsigned)((Source & (PAGE_SIZE-1))/8u),Destination & ~(ULONGLONG)(PAGE_SIZE-1),
            (unsigned)((Destination & (PAGE_SIZE-1))/8u),Count);
        // An ordinary application's destination has no construction-state mirror.
        // An unknown source copying into a registered table clears Known bits.
        if (result!=PAGING_PT_OK && result!=PAGING_PT_MISSING) status=STATUS_INVALID_PARAMETER;
    }
    ExReleasePushLockExclusive(&vm->CpuUpdateLock);
    KeLeaveCriticalRegion();
    return status;
}

// A capacity-accepted physical table fill changes logical construction state,
// not the live CPU/GPU table. No allocation and no hardware write here.
NTSTATUS VidMmCommitPagingFill(ULONGLONG Physical, ULONGLONG Bytes, ULONG Pattern)
{
    BC250_VIDMM* vm=&g_VidMm;
    NTSTATUS status=STATUS_SUCCESS;
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&vm->CpuUpdateLock);
    if (!vm->Ready || !vm->Write || !vm->Shadow.Slots) status=STATUS_DEVICE_NOT_READY;
    else if (Physical<vm->SegmentPhysical || Physical-vm->SegmentPhysical>=vm->SegmentLength ||
        Bytes>vm->SegmentLength-(Physical-vm->SegmentPhysical) ||
        PagingPtShadowFill(&vm->Shadow,Physical,Bytes,Pattern)!=PAGING_PT_OK) status=STATUS_INVALID_PARAMETER;
    ExReleasePushLockExclusive(&vm->CpuUpdateLock);
    KeLeaveCriticalRegion();
    return status;
}

NTSTATUS VidMmCommitPagingTransfer(const BC250_PAGING_COPY_SLICE* Slice)
{
    BC250_VIDMM* vm=&g_VidMm;
    NTSTATUS status=STATUS_SUCCESS;
    int result;
    if (!Slice || Slice->DestinationSystem || !Slice->Bytes) return STATUS_INVALID_PARAMETER;
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&vm->CpuUpdateLock);
    if (!vm->Ready || !vm->Write || !vm->Shadow.Slots) status=STATUS_DEVICE_NOT_READY;
    else if (Slice->DestinationPhysical<vm->SegmentPhysical ||
        Slice->DestinationPhysical-vm->SegmentPhysical>=vm->SegmentLength ||
        Slice->Bytes>vm->SegmentLength-(Slice->DestinationPhysical-vm->SegmentPhysical))
        status=STATUS_INVALID_PARAMETER;
    else {
        result=PagingPtShadowCopyBytes(&vm->Shadow,Slice->SourcePhysical,Slice->DestinationPhysical,
            Slice->Bytes,!Slice->SourceSystem);
        if (result!=PAGING_PT_OK && result!=PAGING_PT_MISSING) status=STATUS_INVALID_PARAMETER;
    }
    ExReleasePushLockExclusive(&vm->CpuUpdateLock);
    KeLeaveCriticalRegion();
    return status;
}

// A leaf may name application storage or a table mapped as paging-process data.
static BOOLEAN VidMmLocalPageAllowed(const BC250_VIDMM* Vm, ULONGLONG Physical)
{
    return (Vm->Pte.vram_size>=PAGE_SIZE && Physical>=Vm->Pte.vram_base &&
            Physical-Vm->Pte.vram_base<=Vm->Pte.vram_size-PAGE_SIZE) ||
           (Vm->Pte.table_size>=PAGE_SIZE && Physical>=Vm->Pte.table_base &&
            Physical-Vm->Pte.table_base<=Vm->Pte.table_size-PAGE_SIZE);
}

// A root page table address as VidMm gives it, as the physical address the VMID's base register wants (without
// amdgpu's VALID bit: gfx.c's shim call adds it). FALSE for anything that is not a page inside the segment.
BOOLEAN VidMmRootPhysical(_In_ const D3DGPU_PHYSICAL_ADDRESS* Address, _Out_ ULONGLONG* Physical)
{
    const BC250_VIDMM* vm = &g_VidMm;

    *Physical = 0;
    // Plan-only mode (EnableGpuVa closed) has no root: tables nobody wrote are not tables to point a GPU at.
    if (!vm->Ready || !vm->Write || vm->SegmentLength < PAGE_SIZE || Address->SegmentId != (vm->Pte.table_size ? vm->Pte.table_segment : vm->Pte.vram_segment) ||
        (Address->SegmentOffset & (PAGE_SIZE - 1)) != 0 || Address->SegmentOffset > vm->SegmentLength - PAGE_SIZE)
        return FALSE;
    *Physical = vm->SegmentPhysical + Address->SegmentOffset;
    return TRUE;
}

// PASSIVE_LEVEL page-table walk through the retained NC segment mapping.
// The public shared lock protects mapping lifetime and excludes CPU updates.
// Ordering against GPU PTE writes still follows OS paging dependencies.
static BOOLEAN VidMmTranslateViewLocked(ULONGLONG RootPhysical, ULONGLONG Va, _Out_ ULONGLONG* Physical, _Out_ BOOLEAN* System, BOOLEAN Logical)
{
    const BC250_VIDMM* vm = &g_VidMm;
    ULONGLONG table = RootPhysical;
    int level;

    *Physical = 0;
    *System = FALSE;
    if (!vm->Ready || !vm->Write || (Logical ? vm->Shadow.Slots==NULL : vm->SegmentMapping==NULL) || vm->SegmentLength < PAGE_SIZE || RootPhysical == 0 ||
        (Va >> (PAGE_SHIFT + 9 * BC250_VIDMM_LEVELS)) != 0 || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return FALSE;
    for (level = BC250_VIDMM_LEVELS - 1; level >= 0; level--)
    {
        struct bc250_pte_fields fields;
        volatile ULONGLONG* page;
        ULONGLONG entry;
        UINT index = (UINT)((Va >> (PAGE_SHIFT + 9 * level)) & (BC250_VIDMM_PTES - 1));

        if ((table & (PAGE_SIZE - 1)) != 0 || table < vm->SegmentPhysical || table > vm->SegmentPhysical + vm->SegmentLength - PAGE_SIZE)
            return FALSE;
        if (Logical) {
            if (PagingPtShadowRead(&vm->Shadow,table,index,&entry)!=PAGING_PT_OK) return FALSE;
        } else {
            page = (volatile ULONGLONG*)(vm->SegmentMapping+(SIZE_T)(table-vm->SegmentPhysical));
            entry = page[index];
        }
        bc250_pte_decode(entry, level == 0 ? BC250_PTE_LEAF : BC250_PTE_DIRECTORY, &fields);
        // A directory entry that is itself a page (PDE-as-PTE, a large mapping) is nothing this file writes; refuse
        // rather than misread it.
        if (!fields.valid || (level != 0 && fields.pde_pte)) return FALSE;
        table = fields.address;
        if (level == 0) *System = (fields.system != 0);
    }
    // Local leaf addresses may name either application or table storage.
    // Directory traversal above remains confined to the table extent.
    if (!*System && !VidMmLocalPageAllowed(vm,table))
        return FALSE;
    *Physical = table + (Va & (PAGE_SIZE - 1));
    return TRUE;
}

static BOOLEAN VidMmTranslateLocked(ULONGLONG RootPhysical, ULONGLONG Va,
                                      _Out_ ULONGLONG* Physical, _Out_ BOOLEAN* System)
{
    return VidMmTranslateViewLocked(RootPhysical,Va,Physical,System,FALSE);
}

// Construction view includes accepted, possibly not-yet-executed PTE updates.
// A missing logical entry is a refusal, never a fallback to stale GPU memory.
BOOLEAN VidMmTranslatePaging(ULONGLONG RootPhysical, ULONGLONG Va,
                             _Out_ ULONGLONG* Physical, _Out_ BOOLEAN* System)
{
    BOOLEAN result;
    if (KeGetCurrentIrql()!=PASSIVE_LEVEL) { *Physical=0; *System=FALSE; return FALSE; }
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&g_VidMm.CpuUpdateLock);
    result=VidMmTranslateViewLocked(RootPhysical,Va,Physical,System,TRUE);
    ExReleasePushLockShared(&g_VidMm.CpuUpdateLock);
    KeLeaveCriticalRegion();
    return result;
}

BOOLEAN VidMmTranslate(ULONGLONG RootPhysical, ULONGLONG Va, _Out_ ULONGLONG* Physical, _Out_ BOOLEAN* System)
{
    BOOLEAN result;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) { *Physical=0; *System=FALSE; return FALSE; }
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&g_VidMm.CpuUpdateLock);
    result = VidMmTranslateLocked(RootPhysical,Va,Physical,System);
    ExReleasePushLockShared(&g_VidMm.CpuUpdateLock);
    KeLeaveCriticalRegion();
    return result;
}

// Same walk as VidMmTranslate, and then the dwords at Va, not at the base of the page. A CPU mapping
// of the BO can show PM4 while this page does not: the PTE would then name the wrong frame, or the
// write would still be sitting in a write-combine buffer. The first four dwords of M125 matched the
// IB only because that VA was page-aligned. PASSIVE_LEVEL.
static BOOLEAN VidMmProbeIbLocked(ULONGLONG RootPhysical, ULONGLONG Va, _Out_ ULONGLONG* Leaf, _Out_ ULONGLONG* Physical,
                     _Out_ BOOLEAN* System, _Out_writes_(BC250_IB_PROBE_DWORDS) ULONG* Dwords)
{
    const BC250_VIDMM* vm = &g_VidMm;
    ULONGLONG table = RootPhysical;
    ULONGLONG leaf = 0;
    ULONG i;
    int level;

    *Leaf = 0;
    *Physical = 0;
    *System = FALSE;
    for (i = 0; i < BC250_IB_PROBE_DWORDS; i++) Dwords[i] = 0;
    if (!vm->Ready || !vm->Write || vm->SegmentMapping==NULL || vm->SegmentLength < PAGE_SIZE || RootPhysical == 0 ||
        (Va >> (PAGE_SHIFT + 9 * BC250_VIDMM_LEVELS)) != 0 || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return FALSE;
    for (level = BC250_VIDMM_LEVELS - 1; level >= 0; level--)
    {
        struct bc250_pte_fields fields;
        volatile ULONGLONG* page;
        ULONGLONG entry;
        UINT index = (UINT)((Va >> (PAGE_SHIFT + 9 * level)) & (BC250_VIDMM_PTES - 1));

        if ((table & (PAGE_SIZE - 1)) != 0 || table < vm->SegmentPhysical ||
            table > vm->SegmentPhysical + vm->SegmentLength - PAGE_SIZE)
            return FALSE;
        page = (volatile ULONGLONG*)(vm->SegmentMapping+(SIZE_T)(table-vm->SegmentPhysical));
        entry = page[index];
        if (level == 0) leaf = entry;
        bc250_pte_decode(entry, level == 0 ? BC250_PTE_LEAF : BC250_PTE_DIRECTORY, &fields);
        if (!fields.valid || (level != 0 && fields.pde_pte)) return FALSE;
        table = fields.address;
        if (level == 0) *System = (fields.system != 0);
    }
    *Leaf = leaf;
    if (!*System && !VidMmLocalPageAllowed(vm,table))
        return FALSE;
    *Physical = table + (Va & (PAGE_SIZE - 1));
    if (*System) {
        MM_COPY_ADDRESS source;
        SIZE_T copied=0,bytes;
        ULONG off=(ULONG)(Va & (PAGE_SIZE-1));
        NTSTATUS status;
        if (!vm->Pte.system_limit || table>=vm->Pte.system_limit ||
            PAGE_SIZE>vm->Pte.system_limit-table || (off&3u)!=0 ||
            (table & (PAGE_SIZE-1))!=0 || VidMmLocalPageAllowed(vm,table)) return FALSE;
        bytes=PAGE_SIZE-off;
        if (bytes>BC250_IB_PROBE_DWORDS*sizeof(ULONG)) bytes=BC250_IB_PROBE_DWORDS*sizeof(ULONG);
        source.PhysicalAddress.QuadPart=(LONGLONG)(table+off);
        // Regular OS RAM only. Do not create an NC alias of an allocation whose
        // existing cache attributes are OS-owned. Caller supplies nonpaged output.
        status=MmCopyMemory(Dwords,source,bytes,MM_COPY_MEMORY_PHYSICAL,&copied);
        if (!NT_SUCCESS(status) || copied!=bytes) return FALSE;
    }
    return TRUE;
}

// CPU table-map lifetime and PTE CPU writers share the same lock as other walks.
// This is a diagnostic snapshot, not GPU synchronization or proof of pinning.
BOOLEAN VidMmProbeIb(ULONGLONG RootPhysical, ULONGLONG Va, _Out_ ULONGLONG* Leaf, _Out_ ULONGLONG* Physical,
                     _Out_ BOOLEAN* System, _Out_writes_(BC250_IB_PROBE_DWORDS) ULONG* Dwords)
{
    BOOLEAN result;
    *Leaf=0;*Physical=0;*System=FALSE;
    RtlZeroMemory(Dwords,BC250_IB_PROBE_DWORDS*sizeof(ULONG));
    if (KeGetCurrentIrql()!=PASSIVE_LEVEL) return FALSE;
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&g_VidMm.CpuUpdateLock);
    result=VidMmProbeIbLocked(RootPhysical,Va,Leaf,Physical,System,Dwords);
    ExReleasePushLockShared(&g_VidMm.CpuUpdateLock);
    KeLeaveCriticalRegion();
    if (!result) {
        *Leaf=0;*Physical=0;*System=FALSE;
        RtlZeroMemory(Dwords,BC250_IB_PROBE_DWORDS*sizeof(ULONG));
    }
    return result;
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
    GuardLog("vidmm summary: system leaf encoding coherent %lld noncoherent %lld snoop mismatches %lld",
             vm->EncodedCoherentSystem,vm->EncodedUncachedSystem,vm->EncodedCoherencyMismatch);
    GuardLog("vidmm summary: %ld cpu-virtual calls, %ld gpu-physical calls", vm->CpuCalls, vm->GpuCalls);
    GuardLog("vidmm summary: %lld entries written (%s), %ld refused, %ld bad calls, %ld roots", vm->Written,
             vm->Write ? "EnableGpuVa open" : "plan only", vm->Refused, vm->BadCalls, vm->Roots);
}
