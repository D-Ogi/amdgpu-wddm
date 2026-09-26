#include <stddef.h>
#include <stdio.h>
typedef unsigned long ULONG;
typedef size_t SIZE_T;
typedef void* PVOID;
typedef unsigned char* PUCHAR;
struct args { void* pDmaBuffer; ULONG DmaSize; ULONG DmaBufferWriteOffset; };
static int check(unsigned offset, unsigned left, unsigned dwords) {
    unsigned char buffer[64];
    struct args arg = { buffer + offset, left, offset };
    struct args* pBuildPagingBuffer = &arg;
    ULONG written = dwords;
    ULONG dmaFree = pBuildPagingBuffer->DmaSize;
    if (dmaFree != left || written * 4 > dmaFree) return 1;
    pBuildPagingBuffer->pDmaBuffer = (PVOID)((PUCHAR)pBuildPagingBuffer->pDmaBuffer + (SIZE_T)written * 4u);
    pBuildPagingBuffer->DmaSize -= written * 4u;
    if (arg.pDmaBuffer != buffer + offset + dwords * 4) return 1;
    if (arg.DmaSize != left - dwords * 4) return 1;
    if (arg.DmaBufferWriteOffset != offset) return 1;
    if ((unsigned char*)arg.pDmaBuffer + arg.DmaSize != buffer + 64) return 1;
    return 0;
}
int main(void) {
    int failures = check(0,64,4) + check(16,48,6) + check(40,24,6) + check(64,0,0);
    printf("paging budget: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
