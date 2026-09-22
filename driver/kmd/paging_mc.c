// See paging_mc.h.
#include "paging_mc.h"

int PagingPhysicalToMc(unsigned long long physical, unsigned long long bytes,
                       unsigned long long vramPhysical, unsigned long long vramMc,
                       unsigned long long vramLength, unsigned long long* mc)
{
    unsigned long long offset;

    *mc = 0;
    if (bytes == 0 || vramLength == 0) return 0;
    if (physical < vramPhysical) return 0;
    offset = physical - vramPhysical;
    if (offset >= vramLength) return 0;
    if (bytes > vramLength - offset) return 0;
    if (vramMc > ~offset) return 0;
    *mc = vramMc + offset;
    return 1;
}
