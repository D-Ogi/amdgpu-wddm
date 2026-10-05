// Publication has accepted DMA/private capacity; apply exactly the emitted group.
// The descriptor and captured identities are borrowed only during this call.
NTSTATUS VidMmCommitPagingGraph(const PAGING_GRAPH_BATCH* Graph)
{
    BC250_VIDMM* vm=&g_VidMm;
    NTSTATUS status=STATUS_SUCCESS;
    if(!Graph)return STATUS_INVALID_PARAMETER;
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&vm->CpuUpdateLock);
    if(!vm->Ready || !vm->Write || !vm->Shadow.Slots)status=STATUS_DEVICE_NOT_READY;
    else if(PagingPtShadowApplyPageMoves(&vm->Shadow,&vm->GraphScratch,
        Graph->Pages,Graph->SystemPages,Graph->Identities,Graph->Moves,Graph->Count,
        Graph->Offset,Graph->Bytes)!=PAGING_PT_OK)status=STATUS_INVALID_PARAMETER;
    ExReleasePushLockExclusive(&vm->CpuUpdateLock);
    KeLeaveCriticalRegion();
    return status;
}

