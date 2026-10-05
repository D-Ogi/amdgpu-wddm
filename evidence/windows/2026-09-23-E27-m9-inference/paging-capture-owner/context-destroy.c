static NTSTATUS Bc250WddmDestroyContext(_In_ const HANDLE hContext)
{
    BC250_WDDM_OBJECT* object = WddmObject(hContext, BC250_WDDM_MAGIC_CONTEXT);

    BC250_WDDM* wddm;

    if (object == NULL) return STATUS_INVALID_PARAMETER;
    if (WddmFirstCalls((BC250_WDDM*)object->Device->Wddm, WddmDdiDestroyContext)) GuardLog("wddm: DestroyContext");
    wddm=(BC250_WDDM*)object->Device->Wddm;
    // Captures are CPU-only but builders may still be reading their arrays.
    // Join that owner before detaching/freeing the context; the lock outlives it.
    if(wddm){KeEnterCriticalRegion();ExAcquirePushLockExclusive(&wddm->PagingBuildLock);}
    WddmFreeObject(object);
    if(wddm){ExReleasePushLockExclusive(&wddm->PagingBuildLock);KeLeaveCriticalRegion();}
    return STATUS_SUCCESS;
}

