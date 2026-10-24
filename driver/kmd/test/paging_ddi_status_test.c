// BD-235: only the three statuses of the DxgkDdiBuildPagingBuffer contract reach the OS.
#include <stdio.h>
#include <stdlib.h>
#include "../paging_ddi_status.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);exit(1);}}while(0)

static void passes(long status)
{
    int refused = -1;
    CHECK(PagingDdiStatus(status, &refused) == status && refused == 0);
}

static void refused(long status)
{
    int refused = -1;
    CHECK(PagingDdiStatus(status, &refused) == PAGING_DDI_SUCCESS && refused == 1);
}

int main(void)
{
    int flag = -1;

    // The permitted statuses pass unchanged and are not refusals.
    passes(PAGING_DDI_SUCCESS);
    passes(PAGING_DDI_INSUFFICIENT_DMA_BUFFER);
    passes(PAGING_DDI_ALLOCATION_BUSY);
    // An informational status is a success.
    CHECK(PagingDdiStatus(0x00000103L, &flag) == PAGING_DDI_SUCCESS && flag == 0);   // STATUS_PENDING
    // The statuses that the builders return on a refusal. 0xC000000D is the status of the 2026-10-24 bug check.
    refused((long)0xC000000DL);     // STATUS_INVALID_PARAMETER
    refused((long)0xC00000A3L);     // STATUS_DEVICE_NOT_READY
    refused((long)0xC00000BBL);     // STATUS_NOT_SUPPORTED
    refused((long)0xC0000017L);     // STATUS_NO_MEMORY
    refused((long)0xC01E0000L);     // a graphics status next to INSUFFICIENT_DMA_BUFFER
    refused((long)0xC01E0101L);     // a graphics status next to ALLOCATION_BUSY
    refused((long)0x80000005L);     // a warning status (STATUS_BUFFER_OVERFLOW)
    // The flag is written on every call, also after a refusal.
    CHECK(PagingDdiStatus((long)0xC000000DL, &flag) == PAGING_DDI_SUCCESS && flag == 1);
    CHECK(PagingDdiStatus(PAGING_DDI_SUCCESS, &flag) == PAGING_DDI_SUCCESS && flag == 0);
    printf("paging_ddi_status_test: all checks passed\n");
    return 0;
}
