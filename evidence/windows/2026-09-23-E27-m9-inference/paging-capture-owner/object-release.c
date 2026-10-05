static void WddmReleaseCaptures(BC250_WDDM_OBJECT* Object)
{
    PAGING_CAPTURE* capture=PagingCaptureTakeAll(&Object->Captures);
    while(capture) {
        PAGING_CAPTURE* next=capture->Next;
        ExFreePoolWithTag(capture,BC250_WDDM_TAG);
        capture=next;
    }
}

static void WddmFreeObject(_In_opt_ BC250_WDDM_OBJECT* Object)
{
    BC250_WDDM* wddm;
    KIRQL irql;

    if (Object == NULL) return;
    wddm = (Object->Device != NULL) ? (BC250_WDDM*)Object->Device->Wddm : NULL;
    if (wddm != NULL)
    {
        // Once the stop has begun, every object on the list belongs to the stop: taking one off here and freeing
        // it would race the drain and free it twice. The removal and the decision are one critical section.
        KeAcquireSpinLock(&wddm->Lock, &irql);
        if (wddm->Stopping) { KeReleaseSpinLock(&wddm->Lock, irql); return; }
        RemoveEntryList(&Object->Link);
        wddm->ObjectCount--;
        KeReleaseSpinLock(&wddm->Lock, irql);
    }
    WddmReleaseCaptures(Object);
    Object->Magic = 0;
    ExFreePoolWithTag(Object, BC250_WDDM_TAG);
}

