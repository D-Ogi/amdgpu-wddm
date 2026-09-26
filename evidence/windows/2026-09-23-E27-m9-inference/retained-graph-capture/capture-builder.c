// Capture once, before any table-changing copy can be accepted. Later batches
// use physical identities; neither the root nor an OS pointer is resolved again.
NTSTATUS GfxPagingCaptureVirtualGraph(BC250_DEVICE* Device,ULONGLONG Root,
    ULONGLONG Source,ULONGLONG Destination,ULONGLONG Bytes,PAGING_GRAPH_CAPTURE** Capture)
{
    PAGING_GRAPH_CAPTURE* c=NULL;BC250_PHYSICAL_STREAM stream;
    BC250_GFX* gfx;PAGING_PAGE_IDENTITY* identities;
    PAGING_U64 *sources,*destinations;unsigned char *sourceSystem,*destinationSystem;
    unsigned pages,i,j,offset=(unsigned)(Source&4095),query=0,required=0,capacity,moves=0;
    ULONGLONG position=0,scratchPhysical,probe=0;int result;
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    *Capture=NULL;
    if(!Device || !Root || !Bytes || offset!=(Destination&4095) ||
       Bytes>MAXULONGLONG-Source || Bytes>MAXULONGLONG-Destination || Bytes/PAGE_SIZE>0x0aaaaaa9u)return status;
    pages=(unsigned)((Bytes+offset+4095)/4096);
    c=ExAllocatePool2(POOL_FLAG_PAGED,sizeof(*c)+(SIZE_T)pages*136,BC250_GFX_TAG);
    if(!c)return STATUS_INSUFFICIENT_RESOURCES;
    c->Owner.PoolTag=BC250_GFX_TAG;
    c->PageCount=pages;c->Owner.Root=Root;c->Owner.Source=Source;c->Owner.Destination=Destination;c->Owner.Bytes=Bytes;
    sources=(PAGING_U64*)(c+1);destinations=sources+pages;
    identities=(PAGING_PAGE_IDENTITY*)(destinations+pages);c->Pages=(PAGING_U64*)(identities+pages*2);
    c->SourceIndex=(unsigned*)(c->Pages+pages*2);c->DestinationIndex=c->SourceIndex+pages;
    c->Readers=c->DestinationIndex+pages;c->Writer=c->Readers+pages*2;
    c->Queue=c->Writer+pages*2;c->Forward=c->Queue+pages*2;c->Moves=(PAGING_PAGE_MOVE*)(c->Forward+pages*2);
    sourceSystem=(unsigned char*)(c->Moves+pages*3);destinationSystem=sourceSystem+pages;c->SystemPages=destinationSystem+pages;
    KeEnterCriticalRegion();ExAcquirePushLockShared(&Device->GfxPagingLock);
    gfx=(BC250_GFX*)Device->Gfx;
    if(!gfx || !gfx->PagingReady || !gfx->PagingRing || !gfx->PagingDevicePtr || gfx->PagingCopyStaging.size<PAGE_SIZE ||
       gfx->PagingCopyStaging.mc<Device->VramMcBase ||
       gfx->PagingCopyStaging.mc-Device->VramMcBase>MAXULONGLONG-(ULONGLONG)Device->VramPhysical.QuadPart)goto Done;
    scratchPhysical=gfx->PagingCopyStaging.mc-Device->VramMcBase+(ULONGLONG)Device->VramPhysical.QuadPart;
    RtlZeroMemory(&stream,sizeof(stream));stream.Common.Device=Device;stream.Common.Gfx=gfx;stream.Common.Root=Root;
    stream.Common.Payload=&query;
    for(i=0;i<pages;i++) {
        unsigned chunk=(unsigned)(4096-((Source+position)&4095));
        if(chunk>Bytes-position)chunk=(unsigned)(Bytes-position);
        if(!PagingResolve(&stream.Common,Source+position,chunk,sources+i) ||
           !PagingResolve(&stream.Common,Destination+position,chunk,destinations+i))goto Done;
        sourceSystem[i]=(unsigned char)((sources[i]&PAGING_SYSTEM_ADDRESS)!=0);
        destinationSystem[i]=(unsigned char)((destinations[i]&PAGING_SYSTEM_ADDRESS)!=0);
        if(sourceSystem[i])probe=sources[i]&~4095ull;
        if(destinationSystem[i])probe=destinations[i]&~4095ull;
        sources[i]=sourceSystem[i]?sources[i]&~PAGING_SYSTEM_ADDRESS:
            sources[i]-Device->VramMcBase+(ULONGLONG)Device->VramPhysical.QuadPart;
        destinations[i]=destinationSystem[i]?destinations[i]&~PAGING_SYSTEM_ADDRESS:
            destinations[i]-Device->VramMcBase+(ULONGLONG)Device->VramPhysical.QuadPart;
        sources[i]&=~4095ull;destinations[i]&=~4095ull;
        if(sources[i]==scratchPhysical || destinations[i]==scratchPhysical)goto Done;
        position+=chunk;
    }
    if(!PagingPageGraphNormalize(sources,destinations,pages,identities,c->Pages,c->SourceIndex,c->DestinationIndex,&c->Identities) ||
       !PagingPageBands(offset,Bytes,c->Bands,&c->BandCount))goto Done;
    for(i=0;i<c->Identities;i++)c->SystemPages[i]=2;
    for(i=0;i<pages;i++)for(j=0;j<2;j++) {
        unsigned at=j?c->DestinationIndex[i]:c->SourceIndex[i];
        unsigned char system=j?destinationSystem[i]:sourceSystem[i];
        // A single physical identity must have a consistent access/cache domain.
        // Supporting conflicting local/system views needs a proven alias policy.
        if(c->SystemPages[at]!=2 && c->SystemPages[at]!=system){status=STATUS_NOT_SUPPORTED;goto Done;}
        c->SystemPages[at]=system;
    }
    if(!probe)probe=sources[0]-(ULONGLONG)Device->VramPhysical.QuadPart+Device->VramMcBase;
    result=PagingEmit(&stream,&query,0,probe,gfx->PagingCopyStaging.mc,PAGE_SIZE,&required);
    if(result!=BC250_SDMA_PAGING_INSUFFICIENT || !required)goto Done;
    capacity=PagingStreamCapacity(BC250_GFX_PAGING_BUFFER_BYTES,0,BC250_GFX_PAGING_BUFFER_BYTES,
        gfx->PagingRing->max_dw,gfx->PagingRing->funcs->align_mask,
        bc250_sdma_fence_size(gfx->PagingRing,AMDGPU_FENCE_FLAG_INT));
    c->RequiredDwords=required;c->MaxAtomicMoves=capacity/required;
    if(c->MaxAtomicMoves<3){status=STATUS_NOT_SUPPORTED;goto Done;}
    for(i=0;i<c->BandCount;i++) {
        PAGING_PAGE_BAND* band=c->Bands+i;
        if(!PagingPageGraphPlan(c->SourceIndex+band->FirstPage,c->DestinationIndex+band->FirstPage,
            (unsigned)band->PageCount,c->Identities,c->Readers,c->Writer,c->Queue,c->Forward,
            c->Moves,pages*3,c->MaxAtomicMoves,&moves))goto Done;
    }
    *Capture=c;c=NULL;status=STATUS_SUCCESS;
Done:
    ExReleasePushLockShared(&Device->GfxPagingLock);KeLeaveCriticalRegion();
    if(c)ExFreePoolWithTag(c,BC250_GFX_TAG);
    return status;
}

int GfxPagingPlanCapturedGraph(PAGING_GRAPH_CAPTURE* Capture,unsigned Band,unsigned Action,
    unsigned MaxMoves,PAGING_GRAPH_BATCH* Batch,unsigned* NextBand,unsigned* NextAction)
{
    unsigned count=0,next=0,required=0;int result;
    RtlZeroMemory(Batch,sizeof(*Batch));*NextBand=Band;*NextAction=Action;
    if(!Capture || Band>Capture->BandCount || (Band==Capture->BandCount && Action))return PagingPermutationInvalid;
    for(;Band<Capture->BandCount;Band++,Action=0) {
        PAGING_PAGE_BAND* band=Capture->Bands+Band;
        if(!PagingPageGraphPlan(Capture->SourceIndex+band->FirstPage,Capture->DestinationIndex+band->FirstPage,
            (unsigned)band->PageCount,Capture->Identities,Capture->Readers,Capture->Writer,Capture->Queue,Capture->Forward,
            Capture->Moves,Capture->PageCount*3,Capture->MaxAtomicMoves,&count) || Action>count)return PagingPermutationInvalid;
        if(!count)continue;
        result=PagingPermutationBatch(Capture->Moves,count,Action,MaxMoves,&next,&required);
        if(result==PagingPermutationInvalid || result==PagingPermutationNeedCycle)return result;
        Batch->Pages=Capture->Pages;Batch->SystemPages=Capture->SystemPages;Batch->Identities=Capture->Identities;
        Batch->Moves=Capture->Moves+Action;Batch->Count=next-Action;Batch->Offset=band->Offset;Batch->Bytes=band->Bytes;
        *NextBand=next==count?Band+1:Band;*NextAction=next==count?0:next;
        return *NextBand==Capture->BandCount?PagingPermutationDone:PagingPermutationMore;
    }
    *NextBand=Band;*NextAction=0;return PagingPermutationDone;
}

