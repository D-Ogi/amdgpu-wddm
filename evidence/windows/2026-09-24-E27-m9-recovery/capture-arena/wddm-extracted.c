static NTSTATUS WddmReserveCaptures(PAGING_CAPTURE_OWNER* Owner)
{
    // MS system paging process uses a 1 GiB VA window. Reserve once at context
    // admission, where allocation failure is legal, not during normal paging.
    SIZE_T bytes=GfxPagingCaptureStorageSize(0,0,1ull<<30);
    Owner->Storage=ExAllocatePool2(POOL_FLAG_NON_PAGED,bytes,BC250_WDDM_TAG);
    if(!Owner->Storage)return STATUS_INSUFFICIENT_RESOURCES;
    Owner->StorageBytes=bytes;
    GuardLog("wddm: capture reservation ready %llu bytes",(ULONGLONG)bytes);
    return STATUS_SUCCESS;
}

static void WddmReleaseCaptureOwner(PAGING_CAPTURE_OWNER* Owner)
{
    PAGING_CAPTURE* capture=PagingCaptureTakeAll(Owner);
    if(Owner->ReservedCaptures || Owner->HeapCaptures)
        GuardLog("wddm: capture release reserved %u heap %u",Owner->ReservedCaptures,Owner->HeapCaptures);
    while(capture) {
        PAGING_CAPTURE* next=capture->Next;
        if(!capture->ReservationBytes)ExFreePoolWithTag(capture,capture->PoolTag);
        capture=next;
    }
    if(Owner->Storage)ExFreePoolWithTag(Owner->Storage,BC250_WDDM_TAG);
    Owner->Storage=NULL;Owner->StorageBytes=0;
}

static NTSTATUS WddmBuildCapturedVirtualTransfer(BC250_DEVICE* Device,ULONGLONG Root,
    PAGING_CAPTURE_OWNER* Owner,DXGKARG_BUILDPAGINGBUFFER* Build,ULONGLONG* Moved)
{
    PAGING_GRAPH_CAPTURE* capture;
    ULONGLONG src=Build->TransferVirtual.SourceVirtualAddress,dst=Build->TransferVirtual.DestinationVirtualAddress;
    ULONGLONG bytes=Build->TransferVirtual.TransferSizeInBytes;
    ULONG originalOffset=Build->DmaBufferWriteOffset;
    NTSTATUS status=STATUS_SUCCESS;
    *Moved=0;
    if(!Root || !Owner || !Build->pDmaBuffer)return STATUS_INVALID_PARAMETER;
    // A transfer wholly inside matching page offsets has no cross-page dependency.
    if((src&4095)==(dst&4095) && bytes<=PAGE_SIZE-(src&4095))return WddmBuildVirtualTransfer(Device,Root,Build,Moved);
    if(Build->MultipassOffset==PAGING_CAPTURE_COMPLETE)return STATUS_SUCCESS;
    if(!Build->MultipassOffset) {
        SIZE_T needed=GfxPagingCaptureStorageSize(src,dst,bytes);
        void* storage=PagingCaptureStorage(Owner,(ULONGLONG)needed);
        if(storage)
            status=GfxPagingCaptureVirtualGraphInPlace(Device,Root,src,dst,bytes,
                storage,needed,&capture);
        else
            // Oversized/exhausted arena fallback remains explicit. Independent
            // multipass plans may share available reservation spans.
            status=GfxPagingCaptureVirtualGraph(Device,Root,src,dst,bytes,&capture);
        if(!NT_SUCCESS(status))return status;
        capture->Owner.ReservationBytes=storage?((ULONGLONG)needed+7)&~7ull:0;
        capture->Owner.Allocation=Build->TransferVirtual.hAllocation;
        capture->Owner.AllocationOffset=Build->TransferVirtual.AllocationOffsetInBytes;
        if(!PagingCaptureAttach(Owner,&capture->Owner)) {
            if(!capture->Owner.ReservationBytes)ExFreePoolWithTag(capture,capture->Owner.PoolTag);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        if(capture->Owner.ReservationBytes) {
            if(Owner->ReservedCaptures<4)
                GuardLog("wddm: capture reserved token 0x%X bytes %llu pages %u identities %u linear %u",
                    capture->Owner.Token,bytes,capture->PageCount,capture->Identities,capture->Linear);
            if(Owner->ReservedCaptures!=MAXULONG)Owner->ReservedCaptures++;
            if(Device->Wddm)InterlockedIncrement64(&((BC250_WDDM*)Device->Wddm)->CaptureReservedTotal);
        } else {
            if(Owner->HeapCaptures<4)
                GuardLog("wddm: capture heap token 0x%X bytes %llu pages %u identities %u linear %u",
                    capture->Owner.Token,bytes,capture->PageCount,capture->Identities,capture->Linear);
            if(Owner->HeapCaptures!=MAXULONG)Owner->HeapCaptures++;
            if(Device->Wddm)InterlockedIncrement64(&((BC250_WDDM*)Device->Wddm)->CaptureHeapTotal);
        }
        Build->MultipassOffset=capture->Owner.Token;
    } else {
        capture=(PAGING_GRAPH_CAPTURE*)PagingCaptureFind(Owner,Build->MultipassOffset);
        if(!capture || capture->Owner.Root!=Root || capture->Owner.Source!=src || capture->Owner.Destination!=dst ||
           capture->Owner.Bytes!=bytes || capture->Owner.Allocation!=Build->TransferVirtual.hAllocation ||
           capture->Owner.AllocationOffset!=Build->TransferVirtual.AllocationOffsetInBytes)return STATUS_INVALID_PARAMETER;
    }
    if(capture->Linear) {
        ULONGLONG app,appLength,table,tableLength,tablePhysical;
        if(!WddmMemoryLayout(Device,&app,&appLength,&table,&tableLength) ||
           table>MAXULONGLONG-(ULONGLONG)Device->VramPhysical.QuadPart)return STATUS_INVALID_PARAMETER;
        tablePhysical=(ULONGLONG)Device->VramPhysical.QuadPart+table;
        while(capture->Progress<bytes) {
            ULONG* record=(ULONG*)Build->pDmaBufferPrivateData;
            ULONG capacity=Build->DmaSize,written=0;ULONGLONG next;
            BC250_PAGING_COPY_SLICE slice;const BC250_PAGING_COPY_SLICE* commit=NULL;
            if(!record || Build->DmaBufferPrivateDataSize<=PAGING_PRIVATE_HEADER_BYTES){status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;break;}
            if(capacity>Build->DmaBufferPrivateDataSize-PAGING_PRIVATE_HEADER_BYTES)capacity=Build->DmaBufferPrivateDataSize-PAGING_PRIVATE_HEADER_BYTES;
            record[0]=0;
            status=GfxPagingEmitCapturedLinear(Device,capture,(PUCHAR)record+PAGING_PRIVATE_HEADER_BYTES,
                Build->DmaBufferWriteOffset,capacity,&written,&slice,&next);
            if(!NT_SUCCESS(status))break;
            if(!slice.DestinationSystem && slice.DestinationPhysical>=tablePhysical && slice.DestinationPhysical-tablePhysical<tableLength) {
                if(slice.Bytes>tableLength-(slice.DestinationPhysical-tablePhysical)){status=STATUS_INVALID_PARAMETER;break;}
                commit=&slice;
            }
            status=WddmPublishPagingRecordCore(Build,written,FALSE,0,0,0,0,0,0,0,0,commit,NULL);
            if(!NT_SUCCESS(status))break;
            capture->Progress=next;Build->DmaBufferWriteOffset+=written*4u;
        }
    }
    while(!capture->Linear && capture->Owner.Band<capture->BandCount) {
        ULONG* record=(ULONG*)Build->pDmaBufferPrivateData;
        ULONG capacity=Build->DmaSize,written=0;
        PAGING_GRAPH_BATCH batch;unsigned nextBand,nextAction;
        if(!record || Build->DmaBufferPrivateDataSize<=PAGING_PRIVATE_HEADER_BYTES) {
            status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;break;
        }
        if(capacity>Build->DmaBufferPrivateDataSize-PAGING_PRIVATE_HEADER_BYTES)
            capacity=Build->DmaBufferPrivateDataSize-PAGING_PRIVATE_HEADER_BYTES;
        record[0]=0;
        status=GfxPagingEmitCapturedGraph(Device,capture,capture->Owner.Band,capture->Owner.Action,
            (PUCHAR)record+PAGING_PRIVATE_HEADER_BYTES,Build->DmaBufferWriteOffset,capacity,
            &written,&batch,&nextBand,&nextAction);
        if(!NT_SUCCESS(status))break;
        if(written) {
            status=WddmPublishPagingRecordCore(Build,written,FALSE,0,0,0,0,0,0,0,0,NULL,&batch);
            if(!NT_SUCCESS(status))break;
        }
        // Only an accepted exact batch advances the retained cursor. Keep filling
        // available capacity across band boundaries instead of forcing a retry.
        capture->Owner.Band=nextBand;capture->Owner.Action=nextAction;
        Build->DmaBufferWriteOffset+=written*4u;
    }
    if(NT_SUCCESS(status) && (capture->Linear?capture->Progress==bytes:capture->Owner.Band==capture->BandCount)) {
        PAGING_CAPTURE* finished=PagingCaptureDetach(Owner,capture->Owner.Token);
        *Moved=bytes;Build->MultipassOffset=PAGING_CAPTURE_COMPLETE;
        if(!finished->ReservationBytes)ExFreePoolWithTag(finished,finished->PoolTag);
    }
    Build->DmaBufferWriteOffset=originalOffset;
    return status;
}