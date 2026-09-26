// GPU-visible memory and the doorbell aperture for the bring-up sequences (milestone M5 second part, ADR 0007): the
// kernel side of bc250_shim_mem_alloc/free and bc250_shim_wdoorbell64 (driver/shim/include/bc250_shim.h).
//
// Two kinds of memory, as in amdgpu:
//   VRAM  from a pool in the top of the carve-out that Windows does not know exists (facts M31), by physical address
//         (facts M32):   end - 32 MB .. end - 8 MB   this pool (MQDs, the clear-state buffer, EOP buffers)
//                        end - 8 MB  .. end          psp.c and gart.c (TMR, firmware staging, GART table, PSP ring)
//   GTT   contiguous non-paged system memory, entered into the GART table that gart.c set up, so that the GPU
//         sees it at gart_start + offset in VMID 0 (rings, read and write pointer write-back slots).
//         The bus address handed to the GPU is the CPU physical address: no DMA remapping is set up for this
//         device. INFERRED until the first ring test shows the GPU's write-back arriving in such a page.
//
// The rule that matters: a GTT page goes back to Windows only when the caller of GpuMemRelease says the GPU is
// quiet (engines halted, entries pointing at the dummy page again, TLB flushed). Otherwise the pages are leaked
// on purpose and that is logged: a GPU still writing a read pointer into memory that Windows has given to somebody
// else is the one failure here that damages something outside the experiment.
//
// Everything in here runs with Device->GartLock held (the sequences' lock), at APC_LEVEL at most. Binding and
// flushing need the GART enabled and its hubs set up: gfx.c refuses a RUN without, and is the only way in.
#include "bc250kmd.h"
#include "bc250kmd_escape.h"
#include "bc250_gmc.h"
#include "bc250_gart.h"
#include "paging_window.h"
#include "bc250_shim.h"

#define BC250_GPUMEM_TAG 'mG2B'
#define BC250_GPUMEM_POOL_BELOW     BC250_VRAM_POOL_BELOW   // the VRAM pool starts 32 MB below the end of VRAM
#define BC250_GPUMEM_POOL_LENGTH    0x1800000ul         // and is 24 MB long
#define BC250_GPUMEM_TABLE_BELOW    BC250_VRAM_GART_BELOW   // gart.c: the GART table, 1 MB
#define BC250_GPUMEM_TABLE_LENGTH   0x100000ul
#define BC250_GPUMEM_GTT_FIRST      0x400000ull         // first GART offset handed out: offset 0 stays unmapped
#define BC250_GPUMEM_GTT_LIMIT      PAGING_DRIVER_GTT_LIMIT        // 64 MB of GART address space and of system memory
#define BC250_GPUMEM_MAX            64
#define BC250_DOORBELL_BAR_INDEX    2
#define BC250_DOORBELL_LENGTH       0x200000ul          // facts M14
// Dword index of the last doorbell amdgpu assigns on this family: AMDGPU_NAVI10_DOORBELL_MAX_ASSIGNMENT (0x18F,
// amdgpu_doorbell.h) counts 64-bit doorbells, and SDMA uses its index shifted left by one (sdma_v5_0.c), so 0x31F.
#define BC250_DOORBELL_LAST_INDEX   0x31Ful
#define PCI_BAR0_OFFSET 0x10

typedef struct _BC250_GPUMEM_ENTRY {
    BOOLEAN Used;
    BOOLEAN Gtt;
    BOOLEAN Bound;                  // GTT only: entered into the GART table (a PLAN's pages never are)
    BOOLEAN TranslationsRetired;    // PTE invalidation and both-hub flush completed; backing remains owned
    BOOLEAN Retired;                // GTT only: freed by the sequence, unbound if it was bound, waiting for GpuMemRelease
    const VOID* Owner;              // the sequence that allocated it: "quiet" is a statement about that owner's hardware only
    PVOID Cpu;
    ULONGLONG Mc;
    ULONGLONG GartOffset;
    ULONG Size;                     // page multiple
} BC250_GPUMEM_ENTRY;

typedef struct _BC250_GPUMEM {
    BC250_GPUMEM_ENTRY Entries[BC250_GPUMEM_MAX];
    ULONG VramNext;                 // bump pointers; they go back to zero when nothing is left
    ULONGLONG GttNext;
    ULONG VramLive, GttLive;
    PUCHAR Table;                   // CPU mapping of the GART table, from the first GTT allocation on
    volatile ULONG* Doorbell;
    PHYSICAL_ADDRESS DoorbellPhysical;
    ULONG DoorbellCount;            // doorbell writes of the running sequence
    BC250_ESCAPE_DOORBELL* Doorbells;
    ULONG MaxDoorbells;
    BOOLEAN TlbDirty;               // a table change whose TLB flush failed: a translation of a page that left the table
                                    // may still be live. The next flush that succeeds clears it (it flushes everything)
} BC250_GPUMEM;

// ---- the doorbell BAR: identified the way mmio.c identifies the register BAR ----------------------------------------------

static NTSTATUS FindDoorbellBar(_In_ BC250_DEVICE* Device, _Out_ PHYSICAL_ADDRESS* Start)
{
    ULONG bar[2] = { 0, 0 }, read = 0;
    ULONGLONG address;
    PCM_RESOURCE_LIST list = Device->DeviceInfo.TranslatedResourceList;
    NTSTATUS status;
    ULONG i, j;

    Start->QuadPart = 0;
    status = Device->Dxgk.DxgkCbReadDeviceSpace(Device->Dxgk.DeviceHandle, DXGK_WHICHSPACE_CONFIG, bar,
                                                PCI_BAR0_OFFSET + 4 * BC250_DOORBELL_BAR_INDEX, sizeof(bar), &read);
    if (!NT_SUCCESS(status) || read != sizeof(bar)) return NT_SUCCESS(status) ? STATUS_DEVICE_DATA_ERROR : status;
    if ((bar[0] & 1) != 0) return STATUS_DEVICE_CONFIGURATION_ERROR;                    // I/O space
    address = bar[0] & ~0xFull;
    if ((bar[0] & 6) == 4) address |= (ULONGLONG)bar[1] << 32;                         // a 64-bit BAR
    if (address == 0 || list == NULL) return STATUS_DEVICE_CONFIGURATION_ERROR;

    for (i = 0; i < list->Count; i++)
    {
        PCM_PARTIAL_RESOURCE_LIST partial = &list->List[i].PartialResourceList;
        for (j = 0; j < partial->Count; j++)
        {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR d = &partial->PartialDescriptors[j];
            if (d->Type == CmResourceTypeMemory && d->u.Memory.Length == BC250_DOORBELL_LENGTH &&
                (ULONGLONG)d->u.Memory.Start.QuadPart == address)
            {
                *Start = d->u.Memory.Start;
                return STATUS_SUCCESS;
            }
        }
    }
    return STATUS_DEVICE_CONFIGURATION_ERROR;
}

// The pool must be above the firmware framebuffer, wherever the firmware says that is (as in gart.c and psp.c).
static BOOLEAN PoolIsAboveFramebuffer(_In_ const BC250_DEVICE* Device)
{
    ULONGLONG fb = (ULONGLONG)Device->Post.PhysicAddress.QuadPart, fbOffset;

    if (fb >= (ULONGLONG)Device->VramPhysical.QuadPart && fb < (ULONGLONG)Device->VramPhysical.QuadPart + Device->VramLength)
        fbOffset = fb - (ULONGLONG)Device->VramPhysical.QuadPart;
    else if (Device->Bar0Length != 0 && fb >= (ULONGLONG)Device->Bar0Physical.QuadPart &&
             fb < (ULONGLONG)Device->Bar0Physical.QuadPart + Device->Bar0Length)
        fbOffset = fb - (ULONGLONG)Device->Bar0Physical.QuadPart;
    else return FALSE;
    return fbOffset + Device->FramebufferLength <= Device->VramLength - BC250_GPUMEM_POOL_BELOW;
}

// ---- start, stop ------------------------------------------------------------------------------------------------------------

NTSTATUS GpuMemStart(_Inout_ BC250_DEVICE* Device)
{
    BC250_GPUMEM* mem;
    NTSTATUS status;

    Device->GpuMem = NULL;
    if (!Device->MmioGfxEnabled || Device->Gart == NULL || !Device->VramEnabled) return STATUS_SUCCESS;
    if (Device->VramLength < 2 * BC250_GPUMEM_POOL_BELOW || !PoolIsAboveFramebuffer(Device))
    {
        GuardLog("gpumem: the top 32 MB of VRAM are not clear of the firmware framebuffer, no GPU memory");
        return STATUS_SUCCESS;
    }
    mem = (BC250_GPUMEM*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*mem), BC250_GPUMEM_TAG);
    if (mem == NULL) return STATUS_SUCCESS;          // never fails the start
    mem->GttNext = BC250_GPUMEM_GTT_FIRST;

    status = FindDoorbellBar(Device, &mem->DoorbellPhysical);
    if (NT_SUCCESS(status))
        mem->Doorbell = (volatile ULONG*)MmMapIoSpaceEx(mem->DoorbellPhysical, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (mem->Doorbell == NULL)
    {
        GuardLog("gpumem: doorbell BAR not identified or not mapped (0x%08X), no GPU memory either", status);
        ExFreePoolWithTag(mem, BC250_GPUMEM_TAG);
        return STATUS_SUCCESS;
    }
    Device->GpuMem = mem;
    GuardLog("gpumem: ready, doorbells at 0x%llX", mem->DoorbellPhysical.QuadPart);
    return STATUS_SUCCESS;
}

static void ReleaseEntry(_Inout_ BC250_GPUMEM* Mem, _Inout_ BC250_GPUMEM_ENTRY* Entry)
{
    if (Entry->Gtt) { MmFreeContiguousMemory(Entry->Cpu); Mem->GttLive--; }
    else { MmUnmapIoSpace(Entry->Cpu, Entry->Size); Mem->VramLive--; }
    RtlZeroMemory(Entry, sizeof(*Entry));
    if (Mem->VramLive == 0) Mem->VramNext = 0;
    if (Mem->GttLive == 0) Mem->GttNext = BC250_GPUMEM_GTT_FIRST;
}

// Give back what one sequence no longer uses. GpuQuiet: the caller has seen ITS hardware stopped (gfx.c: the engines
// halted; ih.c: the IH ring disabled) and the TLB flushed after the last unbind; without it no GTT page returns to
// Windows. Another owner's retired pages are not touched: nothing here knows whether their hardware is quiet.
void GpuMemRelease(_Inout_ BC250_DEVICE* Device, _In_ const VOID* Owner, BOOLEAN GpuQuiet)
{
    BC250_GPUMEM* mem = (BC250_GPUMEM*)Device->GpuMem;
    ULONG i, leaked = 0;

    if (mem == NULL) return;
    for (i = 0; i < BC250_GPUMEM_MAX; i++)
    {
        BC250_GPUMEM_ENTRY* entry = &mem->Entries[i];
        if (!entry->Used || !entry->Gtt || !entry->Retired || entry->Owner != Owner) continue;
        // Halted engines are not enough with a stale translation about: gfx.c's next bring-up may un-halt the MEC to
        // dequeue what the last instance left (bc250_kiq_init_register), and that engine fetches through the TLB.
        if ((GpuQuiet && !mem->TlbDirty && !Device->GfxTlbBootstrap) || !entry->Bound) ReleaseEntry(mem, entry); else leaked += entry->Size;
    }
    if (leaked != 0) GuardLog("gpumem: GPU not known to be quiet%s, %u bytes of GTT memory stay allocated",
                              mem->TlbDirty ? " (a TLB flush failed)" : "", leaked);
}

// Device stop. Live allocations at this point belong to a sequence that was never torn down.
void GpuMemStop(_Inout_ BC250_DEVICE* Device, BOOLEAN GpuQuiet)
{
    BC250_GPUMEM* mem = (BC250_GPUMEM*)Device->GpuMem;
    ULONG i, leaked = 0;

    Device->GpuMem = NULL;
    if (mem == NULL) return;
    for (i = 0; i < BC250_GPUMEM_MAX; i++)
    {
        BC250_GPUMEM_ENTRY* entry = &mem->Entries[i];
        if (!entry->Used) continue;
        // On purpose, see the head of this file: a bound page goes back only unbound and with the GPU quiet.
        if (entry->Gtt && entry->Bound && (!GpuQuiet || mem->TlbDirty || Device->GfxTlbBootstrap || !entry->Retired)) { leaked += entry->Size; continue; }
        ReleaseEntry(mem, entry);
    }
    if (leaked != 0) GuardLog("gpumem: stop with the GPU not known to be quiet: %u bytes of GTT memory leaked on purpose", leaked);
    if (mem->Table != NULL) MmUnmapIoSpace(mem->Table, BC250_GPUMEM_TABLE_LENGTH);
    if (mem->Doorbell != NULL) MmUnmapIoSpace((PVOID)mem->Doorbell, PAGE_SIZE);
    ExFreePoolWithTag(mem, BC250_GPUMEM_TAG);
}

// The doorbell BAR's physical address, for the self-ring aperture (gfx.c stage 8). 0 if there is none.
ULONGLONG GpuMemDoorbellBase(_In_ const BC250_DEVICE* Device)
{
    const BC250_GPUMEM* mem = (const BC250_GPUMEM*)Device->GpuMem;

    return mem != NULL ? (ULONGLONG)mem->DoorbellPhysical.QuadPart : 0;
}

// ADR 0008 stage D (docs/design/paging-node.md section 4): node 1's own doorbell ring, reached directly from
// Device with no BC250_SEQUENCE and no adev involved, so it may be called at DISPATCH_LEVEL, where MemOf()'s
// adev->backend read is not safe (backend is only ever set up and torn down under GartLock, at APC_LEVEL).
// Same store bc250_shim_wdoorbell64 makes below and the same bounds; DoorbellCount/Doorbells is a sequence's
// own escape-replay bookkeeping and is left alone, this write is not part of any sequence's log.
void GpuMemDoorbellWrite(_In_ const BC250_DEVICE* Device, ULONG Index, ULONGLONG Value)
{
    const BC250_GPUMEM* mem = (const BC250_GPUMEM*)Device->GpuMem;

    if (mem == NULL || mem->Doorbell == NULL) return;
    if (Index > BC250_DOORBELL_LAST_INDEX - 1 || (Index & 1) != 0 || ((SIZE_T)Index + 2) * sizeof(ULONG) > PAGE_SIZE) return;
    WRITE_REGISTER_ULONG64((volatile ULONG64*)&mem->Doorbell[Index], Value);
}

// The doorbell writes of one sequence run are listed for the caller, like the register writes (sequence.c).
void GpuMemBeginSequence(_Inout_ BC250_DEVICE* Device, _Out_writes_opt_(MaxDoorbells) BC250_ESCAPE_DOORBELL* Doorbells, ULONG MaxDoorbells)
{
    BC250_GPUMEM* mem = (BC250_GPUMEM*)Device->GpuMem;

    if (mem == NULL) return;
    mem->DoorbellCount = 0;
    mem->Doorbells = Doorbells;
    mem->MaxDoorbells = (Doorbells != NULL) ? MaxDoorbells : 0;
}

ULONG GpuMemEndSequence(_Inout_ BC250_DEVICE* Device, _Out_ ULONG* VramBytes, _Out_ ULONG* GttBytes)
{
    BC250_GPUMEM* mem = (BC250_GPUMEM*)Device->GpuMem;
    ULONG i;

    *VramBytes = 0;
    *GttBytes = 0;
    if (mem == NULL) return 0;
    for (i = 0; i < BC250_GPUMEM_MAX; i++)
    {
        if (!mem->Entries[i].Used) continue;
        if (mem->Entries[i].Gtt) *GttBytes += mem->Entries[i].Size; else *VramBytes += mem->Entries[i].Size;
    }
    mem->Doorbells = NULL;
    mem->MaxDoorbells = 0;
    return mem->DoorbellCount;
}

// ---- the shim's functions ------------------------------------------------------------------------------------------------------

static BC250_GPUMEM* MemOf(_In_ struct amdgpu_device* adev, _Outptr_ BC250_SEQUENCE** Sequence)
{
    *Sequence = (BC250_SEQUENCE*)adev->backend;
    return (*Sequence != NULL && (*Sequence)->Device != NULL) ? (BC250_GPUMEM*)(*Sequence)->Device->GpuMem : NULL;
}

// amdgpu_gart_invalidate_tlb() (amdgpu_gart.c): after every change of the table, VMID 0 of every hub, as amdgpu did on
// unit A after each of its 22 binds (E03 trace, 0.2495 to 0.2528 s). A faulted sequence writes nothing, so the
// acknowledge could only time out: not tried then, and said.
static void ObserveRetirementTlb(struct amdgpu_device* adev, const char* phase, u32 value)
{
    BC250_SEQUENCE* sequence = (BC250_SEQUENCE*)adev->backend;
    GuardLog("gpumem: GFXHUB %s sample 0x%08X fault 0x%08X",phase,value,sequence->Fault);
    // Observe only values already obtained by the flush. Keep unrelated RLC/GRBM
    // reads out of the request -> dummy read -> ACK sequence from Linux v6.18
    // gmc_v10_0_flush_gpu_tlb. M396 stopped in the old observer interval; this
    // removes its extra MMIO, not any required invalidation operation.
    if (sequence->TraceBootstrapTlb) GuardLogKeep();
    if (sequence->TraceBootstrapTlb) {
        GuardLog("startup: visibility observer returned %s",phase);
        GuardLogKeep();
    }
}

static int FlushTlb(struct amdgpu_device* adev, const BC250_SEQUENCE* sequence)
{
    int gfx, mm;

    if (!NT_SUCCESS(sequence->Fault))
    {
        GuardLog("gpumem: the sequence has stopped, no TLB flush after the table change");
        return -5;
    }
    if (sequence->Device->GfxTlbBootstrap && !sequence->Plan) {
        BC250_GPUMEM* mem=(BC250_GPUMEM*)sequence->Device->GpuMem;
        mem->TlbDirty=TRUE;
        mm=bc250_gmc_flush_gpu_tlb(adev,0,AMDGPU_MMHUB0(0),0);
        if (!NT_SUCCESS(sequence->Fault)) return -5;
        return mm; // GFX remains explicitly pending, not globally clean.
    }
    gfx = bc250_gmc_flush_gpu_tlb_observed(adev, 0, AMDGPU_GFXHUB(0), 0,
        sequence->TraceRlcRetirement ? ObserveRetirementTlb : NULL);
    if (sequence->TraceRlcRetirement) GuardLog("gpumem: retirement GFXHUB flush result %d fault 0x%08X",gfx,sequence->Fault);
    if (sequence->TraceRlcRetirement) GfxTraceRlcState(sequence->Device,"after-retire-gfxhub-flush");
    mm = bc250_gmc_flush_gpu_tlb(adev, 0, AMDGPU_MMHUB0(0), 0);
    if (sequence->TraceRlcRetirement) GfxTraceRlcState(sequence->Device,"after-retire-mmhub-flush");
    // A sequence that stopped inside the flush answers every read with all ones, which both of the flush's polls take
    // for "done": the return codes say nothing then. (The semaphore still goes back: gfx.c's PassesFault.)
    if (!NT_SUCCESS(sequence->Fault)) { gfx = -5; mm = -5; }
    if (gfx != 0 || mm != 0) GuardLog("gpumem: TLB flush failed, GFX hub %d, MM hub %d", gfx, mm);
    return gfx != 0 ? gfx : mm;
}

// Caller holds GartLock and has just completed RLC stage5, before CP or
// publication. Keep pending ownership on any failure, including sequence faults.
int GpuMemCompleteGfxBootstrap(struct amdgpu_device* adev)
{
    BC250_SEQUENCE* sequence;
    BC250_GPUMEM* mem=MemOf(adev,&sequence);
    int gfx,mm;
    if (!mem || sequence->Plan || !sequence->Device->GfxTlbBootstrap ||
        !NT_SUCCESS(sequence->Fault)) return -5;
    mem->TlbDirty=TRUE;
    gfx=bc250_gmc_flush_gpu_tlb_observed(adev,0,AMDGPU_GFXHUB(0),0,ObserveRetirementTlb);
    if (sequence->TraceBootstrapTlb) {
        GuardLog("startup: visibility GFX returned %d, entering MMHUB",gfx);
        GuardLogKeep();
    }
    mm=bc250_gmc_flush_gpu_tlb(adev,0,AMDGPU_MMHUB0(0),0);
    if (sequence->TraceBootstrapTlb) {
        GuardLog("startup: visibility MMHUB returned %d",mm);
        GuardLogKeep();
    }
    if (!NT_SUCCESS(sequence->Fault)) return -5;
    if (gfx || mm) return gfx ? gfx : mm;
    mem->TlbDirty=FALSE;
    sequence->Device->GfxTlbBootstrap=FALSE;
    GuardLog("gpumem: startup GFX translations committed after RLC, before CP");
    if (sequence->TraceBootstrapTlb) GuardLogKeep();
    GfxTraceRlcState(sequence->Device,"after-bootstrap-translation-commit");
    if (sequence->TraceBootstrapTlb) {
        GuardLog("startup: visibility publication observer returned");
        GuardLogKeep();
    }
    return 0;
}

// Retained-resume table reconstruction, not ordinary bind/unbind. The power
// coordinator holds GartLock and has halted ALL GTT consumers, including IH.
// Rebuild only the driver's reserved aperture prefix; Windows owns the rest.
// No owner, allocation, physical backing or OS mapping is created/destroyed.
// Caller configures hubs and commits visibility after RLC; this does not flush
// a sleeping GFX hub or claim that stale OS page tables survived power loss.
int GpuMemRebuildRetainedGtt(struct amdgpu_device* adev)
{
    BC250_SEQUENCE* sequence;
    BC250_GPUMEM* mem=MemOf(adev,&sequence);
    ULONGLONG physical[BC250_GPUMEM_MAX];
    ULONG i,page;
    int result;
    if (!mem || !mem->Table || sequence->Plan || !NT_SUCCESS(sequence->Fault)) return -5;
    if (adev->gmc.gart_size<BC250_GPUMEM_GTT_LIMIT ||
        BC250_GPUMEM_GTT_LIMIT/PAGE_SIZE>BC250_GPUMEM_TABLE_LENGTH/sizeof(ULONGLONG)) return -22;
    // Same retained contiguous nonpaged allocations and identity-DMA contract
    // as bc250_shim_mem_alloc. This is not support for IOMMU remapping.
    for (i=0;i<BC250_GPUMEM_MAX;i++) {
        const BC250_GPUMEM_ENTRY* entry=&mem->Entries[i];
        physical[i]=0;
        if (!entry->Used || !entry->Gtt || !entry->Bound ||
            entry->Retired || entry->TranslationsRetired) continue;
        if (!entry->Cpu || !entry->Size || (entry->Size&(PAGE_SIZE-1)) ||
            entry->GartOffset<BC250_GPUMEM_GTT_FIRST ||
            (entry->GartOffset&(PAGE_SIZE-1)) || entry->GartOffset>BC250_GPUMEM_GTT_LIMIT ||
            entry->Size>BC250_GPUMEM_GTT_LIMIT-entry->GartOffset ||
            entry->Mc!=adev->gmc.gart_start+entry->GartOffset) return -22;
        physical[i]=(ULONGLONG)MmGetPhysicalAddress(entry->Cpu).QuadPart;
    }
    mem->TlbDirty=TRUE;
    result=bc250_gart_unbind(adev,0,(unsigned)(BC250_GPUMEM_GTT_LIMIT/PAGE_SIZE),mem->Table);
    if (result) return result;
    for (i=0;i<BC250_GPUMEM_MAX;i++) {
        const BC250_GPUMEM_ENTRY* entry=&mem->Entries[i];
        if (!entry->Used || !entry->Gtt || !entry->Bound ||
            entry->Retired || entry->TranslationsRetired) continue;
        for (page=0;page<entry->Size/PAGE_SIZE;page++) {
            ULONGLONG dma=physical[i]+(ULONGLONG)page*PAGE_SIZE;
            result=bc250_gart_bind(adev,entry->GartOffset+(ULONGLONG)page*PAGE_SIZE,1,&dma,mem->Table);
            if (result) return result;
        }
    }
    KeMemoryBarrier();
    return 0; // TlbDirty stays set until the coordinator commits both hubs.
}

// Caller holds GartLock and has drained/halted this owner's CP/SDMA consumers.
// RLC/CSB and firmware may remain live. This operation frees no memory and
// changes no allocation ownership; Bound remains the historical exposure flag.
int GpuMemRetireGttMappings(struct amdgpu_device* adev)
{
    BC250_SEQUENCE* sequence;
    BC250_GPUMEM* mem = MemOf(adev, &sequence);
    ULONG i, count = 0;
    int result;
    if (mem == NULL || sequence->Plan || sequence->Device->GfxTlbBootstrap || !NT_SUCCESS(sequence->Fault)) return -5;
    for (i = 0; i < BC250_GPUMEM_MAX; i++) {
        BC250_GPUMEM_ENTRY* entry = &mem->Entries[i];
        if (!entry->Used || !entry->Gtt || !entry->Bound || entry->TranslationsRetired || entry->Owner != sequence) continue;
        mem->TlbDirty = TRUE;
        result = bc250_gart_unbind(adev,entry->GartOffset,entry->Size/PAGE_SIZE,mem->Table);
        if (result != 0) return result;
        count++;
    }
    if (count == 0) return mem->TlbDirty ? -62 : 0;
    result = FlushTlb(adev,sequence);
    if (result != 0) return result;
    mem->TlbDirty = FALSE;
    for (i = 0; i < BC250_GPUMEM_MAX; i++) {
        BC250_GPUMEM_ENTRY* entry = &mem->Entries[i];
        if (entry->Used && entry->Gtt && entry->Bound && entry->Owner == sequence)
            entry->TranslationsRetired = TRUE;
    }
    GuardLog("gpumem: retired %lu owner GTT mappings; backing retained",count);
    return 0;
}

int bc250_shim_mem_alloc(struct amdgpu_device* adev, enum bc250_mem_domain domain, unsigned int size, unsigned int align,
                         struct bc250_mem* out)
{
    BC250_SEQUENCE* sequence;
    BC250_GPUMEM* mem = MemOf(adev, &sequence);
    BC250_GPUMEM_ENTRY* entry = NULL;
    const BC250_DEVICE* device;
    ULONG i, rounded;

    RtlZeroMemory(out, sizeof(*out));
    if (mem == NULL || size == 0 || size > BC250_GPUMEM_POOL_LENGTH) return -12;
    device = sequence->Device;
    if (align < PAGE_SIZE) align = PAGE_SIZE;
    if ((align & (align - 1)) != 0 || align > 0x200000) return -22;
    rounded = (size + PAGE_SIZE - 1) & ~(ULONG)(PAGE_SIZE - 1);
    for (i = 0; i < BC250_GPUMEM_MAX && entry == NULL; i++) if (!mem->Entries[i].Used) entry = &mem->Entries[i];
    if (entry == NULL) return -12;

    if (domain == BC250_MEM_VRAM)
    {
        ULONG at = (mem->VramNext + align - 1) & ~(align - 1);      // the pool starts on a 2 MB boundary of MC space
        PHYSICAL_ADDRESS physical;

        if (at > BC250_GPUMEM_POOL_LENGTH || rounded > BC250_GPUMEM_POOL_LENGTH - at) return -12;
        physical.QuadPart = device->VramPhysical.QuadPart + (LONGLONG)(device->VramLength - BC250_GPUMEM_POOL_BELOW + at);
        entry->Cpu = MmMapIoSpaceEx(physical, rounded, PAGE_READWRITE | PAGE_NOCACHE);
        if (entry->Cpu == NULL) return -12;
        entry->Mc = device->VramMcBase + device->VramLength - BC250_GPUMEM_POOL_BELOW + at;
        mem->VramNext = at + rounded;
        mem->VramLive++;
    }
    else if (domain == BC250_MEM_GTT)
    {
        ULONGLONG at = (mem->GttNext + align - 1) & ~((ULONGLONG)align - 1);
        PHYSICAL_ADDRESS low, high, boundary, physical;
        ULONGLONG dma[16];
        ULONG page, pages = rounded / PAGE_SIZE, chunk;
        int failed = 0;

        if (at + rounded > BC250_GPUMEM_GTT_LIMIT) return -12;
        if (mem->Table == NULL)
        {
            physical.QuadPart = device->VramPhysical.QuadPart + (LONGLONG)(device->VramLength - BC250_GPUMEM_TABLE_BELOW);
            mem->Table = (PUCHAR)MmMapIoSpaceEx(physical, BC250_GPUMEM_TABLE_LENGTH, PAGE_READWRITE | PAGE_NOCACHE);
            if (mem->Table == NULL) return -12;
        }
        low.QuadPart = 0;
        high.QuadPart = 0xFFFFFFFFFFFll;            // 44 bits: what the PTE's address field and the hubs' system aperture take
        boundary.QuadPart = 0;
        entry->Cpu = MmAllocateContiguousMemorySpecifyCache(rounded, low, high, boundary, MmCached);
        if (entry->Cpu == NULL) return -12;
        physical = MmGetPhysicalAddress(entry->Cpu);
        // A PLAN gets memory to build its rings and MQDs in, but the GART table is live hardware state: not written.
        for (page = 0; page < pages && !sequence->Plan; page += chunk)
        {
            chunk = min(pages - page, RTL_NUMBER_OF(dma));
            for (i = 0; i < chunk; i++) dma[i] = (ULONGLONG)physical.QuadPart + (ULONGLONG)(page + i) * PAGE_SIZE;
            if (bc250_gart_bind(adev, at + (ULONGLONG)page * PAGE_SIZE, chunk, dma, mem->Table) != 0)
            {
                failed = -22;
                break;
            }
        }
        if (failed == 0 && !sequence->Plan)
        {
            failed = FlushTlb(adev, sequence) != 0 ? -62 : 0;
            mem->TlbDirty = (failed != 0 || device->GfxTlbBootstrap) ? mem->TlbDirty : FALSE;
        }
        if (failed != 0)
        {
            // No engine was given the address. Still: pages that were in the table without a flush after their removal
            // go the way of every bound page (head of this file), retired until GpuMemRelease hears of a quiet GPU.
            bc250_gart_unbind(adev, at, pages, mem->Table);
            if (!sequence->Plan) mem->TlbDirty = (FlushTlb(adev, sequence) != 0) || sequence->Device->GfxTlbBootstrap;
            entry->Used = TRUE;
            entry->Gtt = TRUE;
            entry->Bound = TRUE;
            entry->Retired = TRUE;
            entry->Owner = sequence;
            entry->GartOffset = at;
            entry->Size = rounded;
            mem->GttNext = at + rounded;
            mem->GttLive++;
            return failed;
        }
        entry->Gtt = TRUE;
        entry->Bound = !sequence->Plan;
        entry->GartOffset = at;
        entry->Mc = adev->gmc.gart_start + at;
        mem->GttNext = at + rounded;
        mem->GttLive++;
    }
    else return -22;

    entry->Used = TRUE;
    entry->Owner = sequence;
    entry->Size = rounded;
    RtlZeroMemory(entry->Cpu, rounded);
    out->cpu = entry->Cpu;
    out->mc = entry->Mc;
    out->size = size;
    return 0;
}

void bc250_shim_mem_free(struct amdgpu_device* adev, struct bc250_mem* m)
{
    BC250_SEQUENCE* sequence;
    BC250_GPUMEM* mem = MemOf(adev, &sequence);
    ULONG i;

    // A zeroed struct (nothing was ever allocated into it) is a safe no-op, as before. An allocation
    // whose cpu is NULL - bc250_shim.h: the owner could not map it - still exists here and is looked
    // up by its MC address instead, because there is no cpu pointer to look it up by (D-02; neither
    // MmMapIoSpaceEx nor MmAllocateContiguousMemorySpecifyCache above ever returns success with a
    // NULL pointer, so this arm is not reached on this backend today - see sdma_faults_report.md).
    if (mem == NULL || m == NULL || (m->cpu == NULL && m->mc == 0 && m->size == 0)) return;
    for (i = 0; i < BC250_GPUMEM_MAX; i++)
    {
        BC250_GPUMEM_ENTRY* entry = &mem->Entries[i];
        if (!entry->Used || entry->Retired) continue;
        if (m->cpu != NULL ? entry->Cpu != m->cpu : entry->Mc != m->mc) continue;
        if (entry->Gtt)
        {
            // The GPU loses the page now; Windows gets it back in GpuMemRelease, once the GPU is known to be quiet.
            if (entry->Bound && !entry->TranslationsRetired)
            {
                if (sequence->TraceRlcRetirement) {
                    GuardLog("gpumem: retire GTT entry %lu bytes %llu",i,entry->Size);
                    GfxTraceRlcState(sequence->Device,"before-retire-unbind");
                }
                bc250_gart_unbind(adev, entry->GartOffset, entry->Size / PAGE_SIZE, mem->Table);
                if (sequence->TraceRlcRetirement) GfxTraceRlcState(sequence->Device,"after-retire-unbind");
                // Failed: logged, and the page then waits for more than a quiet GPU (TlbDirty, GpuMemRelease).
                mem->TlbDirty = (FlushTlb(adev, sequence) != 0) || sequence->Device->GfxTlbBootstrap;
            }
            entry->Retired = TRUE;
        }
        else ReleaseEntry(mem, entry);
        break;
    }
    RtlZeroMemory(m, sizeof(*m));
}

void bc250_shim_wdoorbell64(struct amdgpu_device* adev, unsigned int index, unsigned long long value)
{
    BC250_SEQUENCE* sequence;
    BC250_GPUMEM* mem = MemOf(adev, &sequence);

    if (mem == NULL || !NT_SUCCESS(sequence->Fault)) return;
    // The index alone first: index + 1 wraps for 0xFFFFFFFF. Even indices only: amdgpu's doorbells are 64-bit, and an
    // odd dword index would make the store below unaligned, which the CPU may split.
    if (index > BC250_DOORBELL_LAST_INDEX - 1 || (index & 1) != 0 || ((SIZE_T)index + 2) * sizeof(ULONG) > PAGE_SIZE)
    {
        sequence->Fault = STATUS_ACCESS_DENIED;         // stops every further register write as well (sequence.c)
        sequence->FaultOffset = 0xD0000000ul | (index & 0xFFFFul);     // 0xD marks a doorbell, the full index is in the log
        if (!sequence->Dpc) GuardLog("%s: doorbell index 0x%X refused, sequence stopped", sequence->Name, index);
        return;
    }
    if (!sequence->Plan)
    {
        // amdgpu_mm_wdoorbell64(): one 64-bit store (atomic64_set on a u32 pointer).
        WRITE_REGISTER_ULONG64((volatile ULONG64*)&mem->Doorbell[index], value);
    }
    if (mem->DoorbellCount < mem->MaxDoorbells)
    {
        mem->Doorbells[mem->DoorbellCount].Index = index;
        mem->Doorbells[mem->DoorbellCount].AfterWrite = sequence->WriteCount;
        mem->Doorbells[mem->DoorbellCount].Value = value;
    }
    mem->DoorbellCount++;
}

// amdgpu's WDOORBELL32 (amdgpu_mm_wdoorbell): the IH ring's read pointer is the one 32-bit doorbell of this part
// (navi10_ih_set_rptr). Two callers: a sequence under GartLock, recorded like the 64-bit ones, and ih.c's DPC, whose
// sequence says so: nothing recorded, nothing logged, nothing shared. The mapping outlives the DPC: ih.c's stop drains
// the DPCs before gfx.c's stop unmaps it.
void bc250_shim_wdoorbell32(struct amdgpu_device* adev, unsigned int index, unsigned int value)
{
    BC250_SEQUENCE* sequence;
    BC250_GPUMEM* mem = MemOf(adev, &sequence);

    if (mem == NULL || !NT_SUCCESS(sequence->Fault)) return;
    if (index > BC250_DOORBELL_LAST_INDEX || ((SIZE_T)index + 1) * sizeof(ULONG) > PAGE_SIZE)
    {
        sequence->Fault = STATUS_ACCESS_DENIED;
        sequence->FaultOffset = 0xD0000000ul | (index & 0xFFFFul);
        if (!sequence->Dpc) GuardLog("%s: doorbell index 0x%X refused, sequence stopped", sequence->Name, index);
        return;
    }
    if (!sequence->Plan) WRITE_REGISTER_ULONG((volatile ULONG*)&mem->Doorbell[index], value);
    if (sequence->Dpc) return;
    if (mem->DoorbellCount < mem->MaxDoorbells)
    {
        mem->Doorbells[mem->DoorbellCount].Index = index;
        mem->Doorbells[mem->DoorbellCount].AfterWrite = sequence->WriteCount;
        mem->Doorbells[mem->DoorbellCount].Value = value;
    }
    mem->DoorbellCount++;
}
