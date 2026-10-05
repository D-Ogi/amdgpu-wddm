// Emit only from retained identities. Batch and proposed cursor become usable
// together on success; publication, not construction, advances the owner cursor.
NTSTATUS GfxPagingEmitCapturedGraph(BC250_DEVICE* Device,PAGING_GRAPH_CAPTURE* Capture,
    unsigned Band,unsigned Action,PVOID Buffer,ULONG Offset,ULONG Free,ULONG* Written,
    PAGING_GRAPH_BATCH* Batch,unsigned* NextBand,unsigned* NextAction)
{
    BC250_PHYSICAL_STREAM stream;BC250_GFX* gfx;
    PAGING_GRAPH_BATCH batch;unsigned nextBand=Band,nextAction=Action;
    unsigned budget,fresh,i,used=0;int result;
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    *Written=0;*NextBand=Band;*NextAction=Action;RtlZeroMemory(Batch,sizeof(*Batch));
    if(!Device || !Capture || !Buffer || (Offset&3) || Offset>BC250_GFX_PAGING_BUFFER_BYTES || !Capture->RequiredDwords)return status;
    KeEnterCriticalRegion();ExAcquirePushLockShared(&Device->GfxPagingLock);
    gfx=(BC250_GFX*)Device->Gfx;
    if(!gfx || !gfx->PagingReady || !gfx->PagingRing || !gfx->PagingDevicePtr ||
       gfx->PagingCopyStaging.size<PAGE_SIZE || gfx->PagingCopyStaging.mc!=Capture->ScratchMc ||
       Device->VramMcBase!=Capture->VramMcBase || (ULONGLONG)Device->VramPhysical.QuadPart!=Capture->VramPhysical)goto Done;
    fresh=PagingStreamCapacity(BC250_GFX_PAGING_BUFFER_BYTES,0,BC250_GFX_PAGING_BUFFER_BYTES,
        gfx->PagingRing->max_dw,gfx->PagingRing->funcs->align_mask,
        bc250_sdma_fence_size(gfx->PagingRing,AMDGPU_FENCE_FLAG_INT));
    if(Capture->MaxAtomicMoves>fresh/Capture->RequiredDwords)goto Done;
    budget=PagingStreamCapacity(Free,Offset,BC250_GFX_PAGING_BUFFER_BYTES,
        gfx->PagingRing->max_dw,gfx->PagingRing->funcs->align_mask,
        bc250_sdma_fence_size(gfx->PagingRing,AMDGPU_FENCE_FLAG_INT));
    result=GfxPagingPlanCapturedGraph(Capture,Band,Action,budget/Capture->RequiredDwords,&batch,&nextBand,&nextAction);
    if(result==PagingPermutationNeedCycle){status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;goto Done;}
    if(result!=PagingPermutationDone && result!=PagingPermutationMore)goto Done;
    RtlZeroMemory(&stream,sizeof(stream));stream.Common.Device=Device;stream.Common.Gfx=gfx;
    stream.Common.Payload=(unsigned*)Buffer;stream.Common.CommandOffset=Offset;
    for(i=0;i<batch.Count;i++) {
        unsigned ids[2]={batch.Moves[i].source,batch.Moves[i].destination};
        PAGING_U64 mc[2];unsigned side,written=0;
        for(side=0;side<2;side++) {
            if(ids[side]==PAGING_PERMUTATION_SCRATCH)mc[side]=gfx->PagingCopyStaging.mc;
            else if(batch.SystemPages[ids[side]]) {
                if(!gfx->PagingWindowReady)goto Done;
                mc[side]=(batch.Pages[ids[side]]+batch.Offset)|PAGING_SYSTEM_ADDRESS;
            } else if(!PagingPhysicalToMc(batch.Pages[ids[side]]+batch.Offset,batch.Bytes,
                Capture->VramPhysical,Capture->VramMcBase,Device->VramLength,&mc[side]))goto Done;
        }
        result=PagingEmit(&stream,(unsigned*)Buffer+used,budget-used,mc[0],mc[1],batch.Bytes,&written);
        if(result!=BC250_SDMA_PAGING_OK || !written || written>Capture->RequiredDwords)goto Done;
        used+=written;
    }
    *Written=used;*Batch=batch;*NextBand=nextBand;*NextAction=nextAction;status=STATUS_SUCCESS;
Done:
    ExReleasePushLockShared(&Device->GfxPagingLock);KeLeaveCriticalRegion();
    return status;
}

