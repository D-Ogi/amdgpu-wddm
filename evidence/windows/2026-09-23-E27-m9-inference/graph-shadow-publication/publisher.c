static NTSTATUS WddmPublishPagingRecordCore(_Inout_ DXGKARG_BUILDPAGINGBUFFER* Build,
                                        ULONG Written, BOOLEAN Update, ULONG Start, ULONG Next,
                                        ULONGLONG CopySource, ULONGLONG CopyDestination, ULONG CopyCount,
                                        ULONGLONG FillPhysical, ULONGLONG FillBytes, ULONG FillPattern,
                                        const BC250_PAGING_COPY_SLICE* Transfer, const PAGING_GRAPH_BATCH* Graph)
{
    ULONG* record;
    NTSTATUS status;
    if (Build==NULL || Build->pDmaBuffer==NULL || Build->pDmaBufferPrivateData==NULL ||
        Written==0 || Written>Build->DmaSize/sizeof(ULONG)) return STATUS_INVALID_PARAMETER;
    // A graph describes the complete logical effect of this record. Do not mix
    // it with another commit whose later failure could leave a partial effect.
    if(Graph && (Update || CopyCount || FillBytes || Transfer ||
       Build->Operation==DXGK_OPERATION_MAP_APERTURE_SEGMENT ||
       Build->Operation==DXGK_OPERATION_UNMAP_APERTURE_SEGMENT))return STATUS_INVALID_PARAMETER;
    record=(ULONG*)Build->pDmaBufferPrivateData;
    if (!PagingPrivateHeader((unsigned*)record,Build->DmaBufferPrivateDataSize,
            Build->DmaBufferWriteOffset,Build->DmaBufferGpuVirtualAddress,Written*4u)) return STATUS_INVALID_PARAMETER;
    if (Update) {
        if (Next<=Start) { record[0]=0; return STATUS_INVALID_PARAMETER; }
        status=VidMmCommitPagingUpdate(&Build->UpdatePageTable,Start,Next-Start);
        if (!NT_SUCCESS(status)) { record[0]=0; return status; }
    }
    if (CopyCount!=0) {
        status=VidMmCommitPagingCopy(CopySource,CopyDestination,CopyCount);
        if (!NT_SUCCESS(status)) { record[0]=0; return status; }
    }
    if (FillBytes) {
        status=VidMmCommitPagingFill(FillPhysical,FillBytes,FillPattern);
        if (!NT_SUCCESS(status)) { record[0]=0; return status; }
    }
    if (Transfer) {
        status=VidMmCommitPagingTransfer(Transfer);
        if (!NT_SUCCESS(status)) { record[0]=0;return status; }
    }
    if (Build->Operation==DXGK_OPERATION_MAP_APERTURE_SEGMENT ||
        Build->Operation==DXGK_OPERATION_UNMAP_APERTURE_SEGMENT) {
        status=VidMmCommitPagingAperture(Build,Start,Next);
        if (!NT_SUCCESS(status)) {record[0]=0;return status;}
    }
    if(Graph) {
        status=VidMmCommitPagingGraph(Graph);
        if(!NT_SUCCESS(status)){record[0]=0;return status;}
    }
    RtlCopyMemory(Build->pDmaBuffer,(PUCHAR)record+PAGING_PRIVATE_HEADER_BYTES,Written*4u);
    Build->pDmaBufferPrivateData=(PUCHAR)record+PAGING_PRIVATE_HEADER_BYTES+Written*4u;
    Build->DmaBufferPrivateDataSize-=PAGING_PRIVATE_HEADER_BYTES+Written*4u;
    if (Build->DmaBufferPrivateDataSize>=sizeof(ULONG)) *(ULONG*)Build->pDmaBufferPrivateData=0;
    Build->pDmaBuffer=(PUCHAR)Build->pDmaBuffer+Written*4u;
    Build->DmaSize-=Written*4u;
    return STATUS_SUCCESS;
}

