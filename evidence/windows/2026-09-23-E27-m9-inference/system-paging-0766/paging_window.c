#include "paging_window.h"
int PagingWindowInit(unsigned long long start, unsigned long long size,
                     unsigned long long table, unsigned long long tableBytes,
                     PAGING_WINDOW* out)
{
    const unsigned long long offset=PAGING_DRIVER_GTT_LIMIT;
    const unsigned long long pteOffset=(PAGING_DRIVER_GTT_LIMIT/4096)*8;
    if (!out) return 0;
    out->mc=out->table=0;
    if ((start & 4095) || (table & 7) || size<offset+PAGING_WINDOW_BYTES ||
        tableBytes<pteOffset+16 || start>0xffffffffffffull-offset-PAGING_WINDOW_BYTES+1 ||
        table>0xffffffffffffull-pteOffset-15) return 0;
    out->mc=start+offset; out->table=table+pteOffset;
    return 1;
}
