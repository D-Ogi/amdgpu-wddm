NTSTATUS GfxPagingBuildPageGraph(BC250_DEVICE* Device,const BC250_PAGING_ENDPOINT* Source,
    const BC250_PAGING_ENDPOINT* Destination,ULONGLONG Bytes,PVOID Buffer,
    ULONG Offset,ULONG Free,unsigned Resume,ULONG* Written,unsigned* NextResume)
{
    BC250_PHYSICAL_STREAM stream;
    BC250_GFX* gfx;
    PAGING_U64 *sources,*destinations,*physical;
    PAGING_PAGE_IDENTITY* identities;
    PAGING_PAGE_MOVE* moves;
    unsigned *sourceIndex,*destinationIndex,*readers,*writer,*queue,*forward;
    unsigned identityCount=0,bandCount=0,bandIndex=0,startOffset;
    PAGING_PAGE_BAND bands[3];
    ULONGLONG position=0;
    unsigned char* storage;
    unsigned pages,moveCapacity,moveCount=0,i,required=0,budget,maxBudget,used=0;
    unsigned first=0,last=0,cycleSize=0,nextResume=Resume;
    int batch;
    ULONGLONG src,dst,scratchPhysical;
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    int result;
    *Written=0;*NextResume=Resume;
    if(!Device || !Source || !Destination || !Buffer || !Bytes ||
       (Offset&3) || Offset>BC250_GFX_PAGING_BUFFER_BYTES || Bytes/PAGE_SIZE>0x0aaaaaa9u ||
       !(Source->Mdl || Source->Aperture) || !(Destination->Mdl || Destination->Aperture))return status;
    src=Source->Mdl?0:Source->Address;dst=Destination->Mdl?0:Destination->Address;
    startOffset=(unsigned)(src&4095);
    if(startOffset!=(dst&4095) || !PagingPageBands(startOffset,Bytes,bands,&bandCount))return status;
    if(!GfxPagingEndpointValid(Device,Source,Bytes) || !GfxPagingEndpointValid(Device,Destination,Bytes))return status;
    pages=(unsigned)((Bytes+startOffset+4095)/PAGE_SIZE);moveCapacity=pages*3;
    if(Resume && Resume!=pages && !(Resume&PAGING_PERMUTATION_RESUME))return status;
    // 128 bytes/page: captures, identity union, graph workspace and moves. All typed arrays stay aligned.
    storage=ExAllocatePool2(POOL_FLAG_PAGED,(SIZE_T)pages*128,BC250_GFX_TAG);
    if(!storage)return STATUS_INSUFFICIENT_RESOURCES;
    sources=(PAGING_U64*)storage;destinations=sources+pages;
    identities=(PAGING_PAGE_IDENTITY*)(destinations+pages);
    physical=(PAGING_U64*)(identities+pages*2);
    sourceIndex=(unsigned*)(physical+pages*2);destinationIndex=sourceIndex+pages;
    readers=destinationIndex+pages;writer=readers+pages*2;
    queue=writer+pages*2;forward=queue+pages*2;
    moves=(PAGING_PAGE_MOVE*)(forward+pages*2);
    KeEnterCriticalRegion();ExAcquirePushLockShared(&Device->GfxPagingLock);
    gfx=(BC250_GFX*)Device->Gfx;
    if(!gfx || !gfx->PagingReady || !gfx->PagingWindowReady || !gfx->PagingRing ||
       !gfx->PagingDevicePtr || gfx->PagingCopyStaging.size<PAGE_SIZE)goto Done;
    if(gfx->PagingCopyStaging.mc<Device->VramMcBase ||
       gfx->PagingCopyStaging.mc-Device->VramMcBase>MAXULONGLONG-(ULONGLONG)Device->VramPhysical.QuadPart)goto Done;
    scratchPhysical=gfx->PagingCopyStaging.mc-Device->VramMcBase+(ULONGLONG)Device->VramPhysical.QuadPart;
    RtlZeroMemory(&stream,sizeof(stream));stream.Common.Device=Device;stream.Common.Gfx=gfx;
    stream.Common.Payload=(unsigned*)Buffer;stream.Common.CommandOffset=Offset;
    stream.Source=Source;stream.Destination=Destination;
    for(i=0;i<pages;i++) {
        unsigned chunk=(unsigned)(PAGE_SIZE-((src+position)&4095));
        if(chunk>Bytes-position)chunk=(unsigned)(Bytes-position);
        if(!PagingSourceIdentity(&stream,src+position,chunk,sources+i) ||
           !PagingDestinationIdentity(&stream,dst+position,chunk,destinations+i))goto Done;
        sources[i]&=~4095ull;destinations[i]&=~4095ull;
        if(sources[i]==scratchPhysical || destinations[i]==scratchPhysical)goto Done;
        position+=chunk;
    }
    if(Resume==pages){*NextResume=pages;status=STATUS_SUCCESS;goto Done;}
    // Every action contains at least one system endpoint, using the same two-PTE
    // transaction. Ask the real packet builder for its aligned cost without writes.
    result=PagingEmit(&stream,(unsigned*)Buffer,0,sources[0]|PAGING_SYSTEM_ADDRESS,
        gfx->PagingCopyStaging.mc,PAGE_SIZE,&required);
    if(result!=BC250_SDMA_PAGING_INSUFFICIENT || !required)goto Done;
    maxBudget=PagingStreamCapacity(BC250_GFX_PAGING_BUFFER_BYTES,0,BC250_GFX_PAGING_BUFFER_BYTES,
        gfx->PagingRing->max_dw,gfx->PagingRing->funcs->align_mask,
        bc250_sdma_fence_size(gfx->PagingRing,AMDGPU_FENCE_FLAG_INT));
    if(maxBudget/required<3){status=STATUS_NOT_SUPPORTED;goto Done;}
    // Validate all bands before publishing any prefix. Separate bands touch
    // disjoint byte offsets even when the same physical pages appear in each.
    for(bandIndex=0;bandIndex<bandCount;bandIndex++) {
        PAGING_PAGE_BAND* band=&bands[bandIndex];
        if(!PagingPageGraphNormalize(sources+band->FirstPage,destinations+band->FirstPage,
             (unsigned)band->PageCount,identities,physical,sourceIndex,destinationIndex,&identityCount) ||
           !PagingPageGraphPlan(sourceIndex,destinationIndex,(unsigned)band->PageCount,identityCount,
             readers,writer,queue,forward,moves,moveCapacity,maxBudget/required,&moveCount))goto Done;
    }
    bandIndex=Resume&PAGING_PERMUTATION_RESUME ? (Resume>>PAGING_GRAPH_BAND_SHIFT)&3u : 0;
    first=Resume&PAGING_PERMUTATION_RESUME ? Resume&PAGING_GRAPH_ACTION_MASK : 0;
    if(bandIndex>=bandCount)goto Done;
    budget=PagingStreamCapacity(Free,Offset,BC250_GFX_PAGING_BUFFER_BYTES,
        gfx->PagingRing->max_dw,gfx->PagingRing->funcs->align_mask,
        bc250_sdma_fence_size(gfx->PagingRing,AMDGPU_FENCE_FLAG_INT));
    for(;bandIndex<bandCount;bandIndex++,first=0) {
        PAGING_PAGE_BAND* band=&bands[bandIndex];
        if(!PagingPageGraphNormalize(sources+band->FirstPage,destinations+band->FirstPage,
             (unsigned)band->PageCount,identities,physical,sourceIndex,destinationIndex,&identityCount) ||
           !PagingPageGraphPlan(sourceIndex,destinationIndex,(unsigned)band->PageCount,identityCount,
             readers,writer,queue,forward,moves,moveCapacity,maxBudget/required,&moveCount))goto Done;
        if(first>moveCount || (first==moveCount && first))goto Done;
        if(!moveCount)continue;
        batch=PagingPermutationBatch(moves,moveCount,first,(budget-used)/required,&last,&cycleSize);
        if(batch==PagingPermutationInvalid)goto Done;
        for(i=first;i<last;i++) {
            PAGING_U64 from=moves[i].source==PAGING_PERMUTATION_SCRATCH ? gfx->PagingCopyStaging.mc :
                (physical[moves[i].source]+band->Offset)|PAGING_SYSTEM_ADDRESS;
            PAGING_U64 to=moves[i].destination==PAGING_PERMUTATION_SCRATCH ? gfx->PagingCopyStaging.mc :
                (physical[moves[i].destination]+band->Offset)|PAGING_SYSTEM_ADDRESS;
            unsigned written=0;
            // StagingMc remains zero; each SAVE/RESTORE group owns cycle scratch.
            result=PagingEmit(&stream,(unsigned*)Buffer+used,budget-used,from,to,band->Bytes,&written);
            if(result!=BC250_SDMA_PAGING_OK || !written || written>required)goto Done;
            used+=written;
        }
        if(last<moveCount) {
            nextResume=PAGING_PERMUTATION_RESUME|(bandIndex<<PAGING_GRAPH_BAND_SHIFT)|last;
            *Written=used;*NextResume=used?nextResume:Resume;
            status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;goto Done;
        }
    }
    *Written=used;*NextResume=pages;status=STATUS_SUCCESS;
Done:
    ExReleasePushLockShared(&Device->GfxPagingLock);KeLeaveCriticalRegion();
    ExFreePoolWithTag(storage,BC250_GFX_TAG);return status;
}

