// WDK26100 Acquire/Release keeps the VidMm allocation and KMD object alive
// through lookup and publication. Match and publish under the same lock used
// by WddmFreeObject to clear snapshots before an allocation address is reused.
// No runtime callback runs under that lock. The opened object is still private
// to OpenAllocation; its handle is published only after this function returns.
static void WddmBindHandleIdentity(BC250_DEVICE* Device, DXGK_OPENALLOCATIONINFO* Info,
    BC250_WDDM_OBJECT* Opened, ULONG OpenFlags)
{
    BC250_WDDM* wddm=(BC250_WDDM*)Device->Wddm;
    DXGKARGCB_GETHANDLEDATA query={0};
    DXGKARGCB_RELEASEHANDLEDATA release={0};
    void* acquired=NULL;
    void* legacy=NULL;
    BOOLEAN attempted=FALSE,bound=FALSE,released=FALSE,diagnostic=FALSE;
    ULONG kind=0;
    if (!wddm || !Opened || (!wddm->GpuPresentGate && !wddm->HandleIdentityProbe)) return;
    if (wddm->HandleIdentityProbe) {
        kind=Opened->UmdAlloc?1u:0u;
        diagnostic=InterlockedIncrement(&wddm->HandleIdentityProbeCalls[kind])<=16;
    }
    query.hObject=Info->hAllocation;
    query.Type=DXGK_HANDLE_ALLOCATION;
    release.Type=DXGK_HANDLE_ALLOCATION;
    if (KeGetCurrentIrql()<=APC_LEVEL &&
        Device->Dxgk.Size>=FIELD_OFFSET(DXGKRNL_INTERFACE,DxgkCbReleaseHandleData)+sizeof(Device->Dxgk.DxgkCbReleaseHandleData) &&
        Device->Dxgk.DxgkCbAcquireHandleData && Device->Dxgk.DxgkCbReleaseHandleData) {
        // Legacy comparison is bounded diagnostics, never a binding fallback.
        if (diagnostic && KeGetCurrentIrql()==PASSIVE_LEVEL && Device->Dxgk.DxgkCbGetHandleData)
            legacy=Device->Dxgk.DxgkCbGetHandleData(&query);
        attempted=TRUE;
        acquired=Device->Dxgk.DxgkCbAcquireHandleData(&query,&release.ReleaseHandle);
        if (acquired) {
            const LIST_ENTRY* entry;
            KIRQL irql;
            KeAcquireSpinLock(&wddm->Lock,&irql);
            if (!wddm->Stopping) {
                for (entry=wddm->Objects.Flink;entry!=&wddm->Objects;entry=entry->Flink) {
                    BC250_WDDM_OBJECT* object=CONTAINING_RECORD(entry,BC250_WDDM_OBJECT,Link);
                    if ((void*)object==acquired) {
                        if (object->Magic==BC250_WDDM_MAGIC_ALLOCATION && object->Device==Device) {
                            Opened->BackingAllocation=object;
                            bound=TRUE;
                        }
                        break;
                    }
                }
            }
            KeReleaseSpinLock(&wddm->Lock,irql);
        }
        // Release a nonzero token even if private data was NULL. Also retain
        // the paired release for a successful pointer with a zero token.
        if (acquired || release.ReleaseHandle) {
            Device->Dxgk.DxgkCbReleaseHandleData(release);
            released=TRUE;
        }
    }
    // Boolean witnesses fit in a GuardLog record. No pointer is dereferenced
    // after release; bound records publication, not indefinite ownership.
    if (diagnostic)
        GuardLog("wddm: identity bind kind %u flags %x attempted %u get %u acquire %u token %u bound %u released %u",
            kind,OpenFlags,attempted,legacy!=NULL,acquired!=NULL,release.ReleaseHandle!=NULL,bound,released);
}

// Copy both validated descriptors under the object-list lock. No opened or
// backing object pointer escapes; destroy/close may run after this returns
// without invalidating metadata consumed by the command builder. VidMm still
// owns residency and in-flight command lifetime, independently of this snapshot.
static BOOLEAN WddmSnapshotPresentAllocations(BC250_WDDM* Wddm, HANDLE OwnerDevice,
    const HANDLE Handles[2], BC250_WDDM_ALLOCATION_PRIVATE Allocations[2])
{
    BC250_WDDM_OBJECT* backing[2]={NULL,NULL};
    BC250_WDDM_ALLOCATION_PRIVATE snapshots[2];
    const LIST_ENTRY* entry;
    BOOLEAN valid=FALSE;
    KIRQL irql;
    UINT i;
    KeAcquireSpinLock(&Wddm->Lock,&irql);
    if (Wddm->Stopping) goto done;
    for (i=0;i<2;i++) {
        BC250_WDDM_OBJECT* opened=NULL;
        for (entry=Wddm->Objects.Flink;entry!=&Wddm->Objects;entry=entry->Flink) {
            BC250_WDDM_OBJECT* object=CONTAINING_RECORD(entry,BC250_WDDM_OBJECT,Link);
            if ((HANDLE)object==Handles[i] && object->Magic==BC250_WDDM_MAGIC_OPENED) { opened=object; break; }
        }
        if (!opened || opened->OwnerDevice!=OwnerDevice || opened->UmdAlloc || !opened->BackingAllocation) goto done;
        for (entry=Wddm->Objects.Flink;entry!=&Wddm->Objects;entry=entry->Flink) {
            BC250_WDDM_OBJECT* object=CONTAINING_RECORD(entry,BC250_WDDM_OBJECT,Link);
            if ((HANDLE)object==opened->BackingAllocation && object->Magic==BC250_WDDM_MAGIC_ALLOCATION) { backing[i]=object; break; }
        }
        if (!backing[i] || backing[i]->UmdAlloc ||
            RtlCompareMemory(&backing[i]->Allocation,&opened->Allocation,sizeof(opened->Allocation))!=sizeof(opened->Allocation)) goto done;
        snapshots[i]=opened->Allocation;
    }
    // Two imports of one allocation are not a supported non-overlapping blit.
    if (backing[0]==backing[1] || snapshots[0].Format!=snapshots[1].Format) goto done;
    Allocations[0]=snapshots[0];Allocations[1]=snapshots[1];
    valid=TRUE;
done:
    KeReleaseSpinLock(&Wddm->Lock,irql);
    return valid;
}
