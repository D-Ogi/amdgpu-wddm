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


// Reserve OS aperture entries after both private GART consumers. Validate the
// complete range once; callers never obtain a partially usable advertised extent.
int PagingApertureInit(unsigned long long start, unsigned long long size,
                       unsigned long long table, unsigned long long tableBytes,
                       PAGING_APERTURE* out)
{
    const unsigned long long offset=PAGING_APERTURE_OFFSET;
    const unsigned long long pteOffset=(PAGING_APERTURE_OFFSET/4096)*8;
    const unsigned long long pteBytes=(PAGING_APERTURE_BYTES/4096)*8;
    const unsigned long long limit=0x1000000000000ull;
    if (!out) return 0;
    out->mc=out->table=out->bytes=0;
    if ((start&4095) || (table&7) || size<offset+PAGING_APERTURE_BYTES ||
        tableBytes<pteOffset+pteBytes ||
        start>limit-offset-PAGING_APERTURE_BYTES ||
        table>limit-pteOffset-pteBytes) return 0;
    out->mc=start+offset;out->table=table+pteOffset;out->bytes=PAGING_APERTURE_BYTES;
    return 1;
}

int PagingApertureRange(const PAGING_APERTURE* aperture,
                        unsigned long long firstPage, unsigned long long pageCount,
                        unsigned long long* mc, unsigned long long* table)
{
    const unsigned long long limit=0x1000000000000ull;
    unsigned long long pages;
    if (mc) *mc=0;
    if (table) *table=0;
    if (!aperture || !mc || !table || mc==table ||
        aperture->bytes!=PAGING_APERTURE_BYTES ||
        (aperture->mc&4095) || (aperture->table&7) ||
        aperture->mc>limit-PAGING_APERTURE_BYTES ||
        aperture->table>limit-(PAGING_APERTURE_BYTES/4096)*8) return 0;
    pages=aperture->bytes/4096;
    if (!pageCount || firstPage>=pages || pageCount>pages-firstPage) return 0;
    *mc=aperture->mc+firstPage*4096;*table=aperture->table+firstPage*8;
    return 1;
}
