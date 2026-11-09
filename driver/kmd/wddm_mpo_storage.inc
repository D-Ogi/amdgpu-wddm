// Check3 receives adapter allocation handles, not per-device opened handles.
// Copy both descriptions while the object index/removal lock protects them.
// Handles are values until the index resolves them. No allocation pointer
// escapes this function. Serial is an identity witness, not a residency pin;
// SetAddress must resolve/revalidate it and its physical placement again.
static BOOLEAN WddmSnapshotMpoStorage(BC250_DEVICE* Device,BC250_WDDM* Wddm,
    const HANDLE Handles[2],BC250_MPO_STORAGE Out[2])
{
    BC250_MPO_STORAGE copy[2];KIRQL irql;BOOLEAN valid=FALSE;UINT i;
    RtlZeroMemory(copy,sizeof(copy));RtlZeroMemory(Out,2*sizeof(*Out));
    KeAcquireSpinLock(&Wddm->Lock,&irql);
    if (Wddm->Stopping || Handles[0]==Handles[1]) goto done;
    for (i=0;i<2;i++) {
        BC250_WDDM_OBJECT* a=(BC250_WDDM_OBJECT*)Bc250ObjectIndexFind(&Wddm->ObjectIndex,Handles[i]);
        if (!a || a->Magic!=BC250_WDDM_MAGIC_ALLOCATION || a->Device!=Device || !a->Serial) goto done;
        copy[i].Serial=a->Serial;copy[i].UmdAlloc=a->UmdAlloc?1u:0u;
        copy[i].ScanoutRequested=a->ScanoutRequested?1u:0u;
        if (a->UmdAlloc) {
            copy[i].Surface.Width=a->ScanoutWidth;copy[i].Surface.Height=a->ScanoutHeight;
            copy[i].Surface.Pitch=a->ScanoutPitch;copy[i].Surface.Format=a->ScanoutFormat;
            copy[i].Surface.Size=a->UmdBytes;
        } else {
            copy[i].Surface=a->Allocation;
            if (a->Allocation.Version==BC250_WDDM_NV12_VERSION) {
                if (!Bc250Nv12PrivateValid(&a->Nv12,sizeof(a->Nv12)) ||
                    RtlCompareMemory(&a->Nv12.Surface,&a->Allocation,sizeof(a->Allocation))!=sizeof(a->Allocation)) goto done;
                copy[i].Nv12=a->Nv12;
            }
        }
    }
    valid=TRUE;
done:
    KeReleaseSpinLock(&Wddm->Lock,irql);
    if (valid) {Out[0]=copy[0];Out[1]=copy[1];}
    return valid;
}
