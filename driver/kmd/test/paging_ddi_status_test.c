// BD-235: the BuildPagingBuffer status rules of paging_ddi_status.h. No status becomes STATUS_SUCCESS unless the
// builder returned STATUS_SUCCESS; the three statuses of the DDI pass unchanged.
#include <stdio.h>
#include <stdlib.h>
#include "../paging_ddi_status.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);exit(1);}}while(0)

static void permitted(long status)
{
    int outside = -1;
    CHECK(PagingDdiStatus(status, &outside) == status && outside == 0);
}

// A failure outside the DDI goes to the OS unchanged, and the caller counts it.
static void failure(long status)
{
    int outside = -1;
    CHECK(PagingDdiStatus(status, &outside) == status && outside == 1);
}

// A nonnegative status other than STATUS_SUCCESS is not a completed operation.
static void not_built(long status)
{
    int outside = -1;
    CHECK(PagingDdiStatus(status, &outside) == PAGING_DDI_NOT_BUILT && outside == 1);
}

int main(void)
{
    static const long others[] = {
        (long)0xC000000DL,  // STATUS_INVALID_PARAMETER, the status of the 2026-10-24 bug check
        (long)0xC00000A3L,  // STATUS_DEVICE_NOT_READY
        (long)0xC00000BBL,  // STATUS_NOT_SUPPORTED
        (long)0xC0000017L,  // STATUS_NO_MEMORY
        (long)0xC01E0000L,  // a graphics status next to INSUFFICIENT_DMA_BUFFER
        (long)0xC01E0101L,  // a graphics status next to ALLOCATION_BUSY
        (long)0x80000005L,  // a warning status (STATUS_BUFFER_OVERFLOW)
        0x00000103L,        // STATUS_PENDING
        0x00000001L,        // STATUS_WAIT_1
        0x40000000L,        // STATUS_OBJECT_NAME_EXISTS, an informational status
        0x7FFFFFFFL,
    };
    unsigned i;
    int flag = -1;

    permitted(PAGING_DDI_SUCCESS);
    permitted(PAGING_DDI_INSUFFICIENT_DMA_BUFFER);
    permitted(PAGING_DDI_ALLOCATION_BUSY);
    failure((long)0xC000000DL);
    failure((long)0xC00000A3L);
    failure((long)0xC00000BBL);
    failure((long)0xC0000017L);
    failure((long)0xC01E0000L);
    failure((long)0xC01E0101L);
    failure((long)0x80000005L);
    not_built(0x00000103L);
    not_built(0x00000001L);
    not_built(0x40000000L);
    not_built(0x7FFFFFFFL);
    // The property: only STATUS_SUCCESS gives STATUS_SUCCESS.
    for (i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        CHECK(PagingDdiStatus(others[i], &flag) != PAGING_DDI_SUCCESS && flag == 1);
    }
    // The flag is written on every call.
    CHECK(PagingDdiStatus((long)0xC000000DL, &flag) == (long)0xC000000DL && flag == 1);
    CHECK(PagingDdiStatus(PAGING_DDI_SUCCESS, &flag) == PAGING_DDI_SUCCESS && flag == 0);
    printf("paging_ddi_status_test: all checks passed\n");
    return 0;
}
