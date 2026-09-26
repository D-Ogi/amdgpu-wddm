static NTSTATUS WddmBuildVirtualTransfer(BC250_DEVICE* Device, ULONGLONG Root,
    DXGKARG_BUILDPAGINGBUFFER* Build, ULONGLONG* Moved)
{
    ULONGLONG bytes=Build->TransferVirtual.TransferSizeInBytes;
    ULONGLONG src=Build->TransferVirtual.SourceVirtualAddress,dst=Build->TransferVirtual.DestinationVirtualAddress;
    ULONGLONG app,appLength,table,tableLength,tablePhysical,progress;
    ULONG originalOffset=Build->DmaBufferWriteOffset;
    BOOLEAN graphResume=(BOOLEAN)((Build->MultipassOffset&PAGING_PERMUTATION_RESUME)!=0);
    NTSTATUS status=STATUS_SUCCESS;
    *Moved=0;
    if (!Root || !Build->pDmaBuffer || !PagingStreamTokenDecode(FALSE,src,dst,bytes,graphResume?0:Build->MultipassOffset,&progress) ||
        !WddmMemoryLayout(Device,&app,&appLength,&table,&tableLength) ||
        table>MAXULONGLONG-(ULONGLONG)Device->VramPhysical.QuadPart) return STATUS_INVALID_PARAMETER;
    // Capture every system-page dependency before publishing a virtual prefix.
    // Local/mixed endpoints keep the table-shadow-aware path below; general
    // cross-page aliases in that path remain separate work.
    if(bytes>PAGE_SIZE && (src&(PAGE_SIZE-1))==(dst&(PAGE_SIZE-1))) {
        ULONG* record=(ULONG*)Build->pDmaBufferPrivateData;
        ULONG capacity=Build->DmaSize,written=0;
        unsigned next=Build->MultipassOffset;
        if(progress==bytes)return STATUS_SUCCESS;
        if(!record || Build->DmaBufferPrivateDataSize<=PAGING_PRIVATE_HEADER_BYTES)
            return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
        if(capacity>Build->DmaBufferPrivateDataSize-PAGING_PRIVATE_HEADER_BYTES)
            capacity=Build->DmaBufferPrivateDataSize-PAGING_PRIVATE_HEADER_BYTES;
        status=GfxPagingBuildVirtualPageGraph(Device,Root,src,dst,bytes,
            (PUCHAR)record+PAGING_PRIVATE_HEADER_BYTES,originalOffset,capacity,
            Build->MultipassOffset,&written,&next);
        if(status!=STATUS_NOT_SUPPORTED) {
            if(status!=STATUS_SUCCESS && status!=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER)return status;
            if(written) {
                NTSTATUS publish=WddmPublishPagingRecord(Build,written,FALSE,0,0);
                if(!NT_SUCCESS(publish))return publish;
            }
            Build->MultipassOffset=next;*Moved=status==STATUS_SUCCESS?bytes:0;
            return status;
        }
        status=STATUS_SUCCESS;
    }
    if(graphResume)return STATUS_INVALID_PARAMETER;
    tablePhysical=(ULONGLONG)Device->VramPhysical.QuadPart+table;
    while (progress<bytes) {
        ULONG count=(ULONG)(PAGE_SIZE-((src+progress)&(PAGE_SIZE-1)));
        ULONG destinationRoom=(ULONG)(PAGE_SIZE-((dst+progress)&(PAGE_SIZE-1)));
        ULONG written=0,capacity=Build->DmaSize;
        ULONG* record=(ULONG*)Build->pDmaBufferPrivateData;
        BC250_PAGING_COPY_SLICE slice;
        const BC250_PAGING_COPY_SLICE* commit=NULL;
        unsigned nextToken;
        if (count>destinationRoom) count=destinationRoom;
        if (count>bytes-progress) count=(ULONG)(bytes-progress);
        if (!PagingStreamTokenEncode(FALSE,src,dst,bytes,progress+count,&nextToken)) {
            status=STATUS_INVALID_PARAMETER;break;
        }
        if (!record || Build->DmaBufferPrivateDataSize<=PAGING_PRIVATE_HEADER_BYTES) {
            status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;break;
        }
        record[0]=0;
        if (capacity>Build->DmaBufferPrivateDataSize-PAGING_PRIVATE_HEADER_BYTES)
            capacity=Build->DmaBufferPrivateDataSize-PAGING_PRIVATE_HEADER_BYTES;
        // Per-slice aliases stage. Arbitrary cross-page physical alias dependencies
        // are still unresolved; VA ordering alone cannot establish physical order.
        status=GfxPagingBuildVirtualCopyPage(Device,Root,src+progress,dst+progress,count,FALSE,
            (PUCHAR)record+PAGING_PRIVATE_HEADER_BYTES,Build->DmaBufferWriteOffset,capacity,&written,&slice);
        if (!NT_SUCCESS(status)) break;
        if (!slice.DestinationSystem && slice.DestinationPhysical>=tablePhysical &&
            slice.DestinationPhysical-tablePhysical<tableLength) {
            if (count>tableLength-(slice.DestinationPhysical-tablePhysical)) {
                status=STATUS_INVALID_PARAMETER;break;
            }
            commit=&slice;
        }
        status=WddmPublishPagingRecordCore(Build,written,FALSE,0,0,0,0,0,0,0,0,commit);
        if (!NT_SUCCESS(status)) break;
        progress+=count;Build->MultipassOffset=nextToken;*Moved+=count;
        Build->DmaBufferWriteOffset+=written*4u;
    }
    Build->DmaBufferWriteOffset=originalOffset;
    return status;
}

