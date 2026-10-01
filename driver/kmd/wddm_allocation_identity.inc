// The allocation an opened object was bound to, or NULL once it is gone. The
// stored pointer is a value: it is looked up in the object index first and the
// object is read only if it is indexed, is an allocation and carries the serial
// recorded at the bind. WddmFreeObject takes an allocation out of the index under
// this lock, and a pool address reused later carries a new serial, so a binding
// to a destroyed allocation can never alias its successor. Caller holds Lock.
static BC250_WDDM_OBJECT* WddmBackingAllocationLocked(BC250_WDDM* Wddm, const BC250_WDDM_OBJECT* Opened)
{
    BC250_WDDM_OBJECT* backing;
    if (!Opened->BackingAllocation) return NULL;
    backing=(BC250_WDDM_OBJECT*)Bc250ObjectIndexFind(&Wddm->ObjectIndex,Opened->BackingAllocation);
    if (!backing || backing->Magic!=BC250_WDDM_MAGIC_ALLOCATION || backing->Serial!=Opened->BackingSerial) return NULL;
    return backing;
}

// WDK26100 Acquire/Release keeps the VidMm allocation and KMD object alive
// through lookup and publication. Match and publish, pointer and serial, under
// the same lock WddmFreeObject holds when it takes an allocation out of the
// object index. No runtime callback runs under that lock. The opened object is
// still private to OpenAllocation; its handle is published only after this
// function returns.
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
            BC250_WDDM_OBJECT* object;
            KIRQL irql;
            KeAcquireSpinLock(&wddm->Lock,&irql);
            if (!wddm->Stopping) {
                // The acquired pointer is a value until the index returns it.
                object=(BC250_WDDM_OBJECT*)Bc250ObjectIndexFind(&wddm->ObjectIndex,acquired);
                if (object && object->Magic==BC250_WDDM_MAGIC_ALLOCATION && object->Device==Device) {
                    Opened->BackingAllocation=object;
                    Opened->BackingSerial=object->Serial;
                    bound=TRUE;
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
// Both handles and both bindings are values until the object index returns them.
static BOOLEAN WddmSnapshotPresentAllocations(BC250_WDDM* Wddm, HANDLE OwnerDevice,
    const HANDLE Handles[2], BC250_WDDM_ALLOCATION_PRIVATE Allocations[2])
{
    BC250_WDDM_OBJECT* backing[2]={NULL,NULL};
    BC250_WDDM_ALLOCATION_PRIVATE snapshots[2];
    BOOLEAN valid=FALSE;
    KIRQL irql;
    UINT i;
    KeAcquireSpinLock(&Wddm->Lock,&irql);
    if (Wddm->Stopping) goto done;
    for (i=0;i<2;i++) {
        BC250_WDDM_OBJECT* opened=(BC250_WDDM_OBJECT*)Bc250ObjectIndexFind(&Wddm->ObjectIndex,Handles[i]);
        if (!opened || opened->Magic!=BC250_WDDM_MAGIC_OPENED ||
            opened->OwnerDevice!=OwnerDevice || opened->UmdAlloc || !opened->BackingAllocation) goto done;
        backing[i]=WddmBackingAllocationLocked(Wddm,opened);
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
