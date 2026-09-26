#include "paging_stream.h"

int PagingStreamBuild64(void* context, PAGING_RESOLVE resolve, PAGING_EMIT emit,
                      int fill, PAGING_U64 src, PAGING_U64 dst, PAGING_U64 total, PAGING_U64 start,
                      unsigned* buffer, unsigned capacity, unsigned* written, PAGING_U64* next)
{
    PAGING_U64 done=start;
    unsigned used=0;
    if (!written || !next) return PagingStreamInvalid;
    *written=0; *next=start;
    if (!resolve || !emit || !buffer || !total || start>total ||
        dst>~(PAGING_U64)0-total || (!fill && src>~(PAGING_U64)0-total) ||
        (fill && ((dst | total | start) & 3))) return PagingStreamInvalid;
    while (done<total) {
        unsigned bytes=4096u-(unsigned)((dst+done)&4095u), dw=0;
        PAGING_U64 source=0,destination=0;
        int rc;
        if (!fill) {
            unsigned sourceBytes=4096u-(unsigned)((src+done)&4095u);
            if (sourceBytes<bytes) bytes=sourceBytes;
        }
        if (total-done<bytes) bytes=(unsigned)(total-done);
        if (!resolve(context,dst+done,bytes,&destination) ||
            (!fill && !resolve(context,src+done,bytes,&source))) return PagingStreamAddress;
        rc=emit(context,buffer+used,capacity-used,source,destination,bytes,&dw);
        if (rc==1) { *written=used; *next=done; return PagingStreamMore; }
        if (rc!=0 || !dw || dw>capacity-used) return PagingStreamInvalid;
        used+=dw; done+=bytes;
    }
    *written=used; *next=done;
    return PagingStreamDone;
}

int PagingStreamBuild(void* context, PAGING_RESOLVE resolve, PAGING_EMIT emit,
                      int fill, PAGING_U64 src, PAGING_U64 dst, unsigned total, unsigned start,
                      unsigned* buffer, unsigned capacity, unsigned* written, unsigned* next)
{
    PAGING_U64 progress=start;
    int rc;
    if (!written || !next) return PagingStreamInvalid;
    rc=PagingStreamBuild64(context,resolve,emit,fill,src,dst,total,start,
                         buffer,capacity,written,&progress);
    // Progress cannot exceed total or the original start, both unsigned.
    *next=(unsigned)progress;
    return rc;
}

unsigned PagingStreamCapacity(unsigned remaining, unsigned offset, unsigned shadow,
                              unsigned ringMax, unsigned alignMask, unsigned fence)
{
    unsigned limit=ringMax & ~alignMask, room;
    if ((offset & 3) || fence>limit) return 0;
    limit-=fence;
    if (limit>shadow/4) limit=shadow/4;
    if (offset/4>limit) return 0;
    room=limit-offset/4;
    return remaining/4<room ? remaining/4 : room;
}

int PagingPageListAddress(const PAGING_U64* Pages, unsigned PageCount,
                          PAGING_U64 BasePage, int Contiguous, unsigned FirstPage,
                          PAGING_U64 ByteOffset, unsigned Bytes, PAGING_U64* Address)
{
    PAGING_U64 relative=ByteOffset>>12,page,index;
    unsigned within=(unsigned)(ByteOffset&4095u);
    if (!Address) return 0;
    *Address=0;
    if ((Contiguous!=0 && Contiguous!=1) || !Bytes || Bytes>4096u-within ||
        FirstPage>=PageCount || relative>=(PAGING_U64)(PageCount-FirstPage) ||
        (!Contiguous && !Pages)) return 0;
    index=(PAGING_U64)FirstPage+relative;
    if (Contiguous) {
        if (BasePage>~(PAGING_U64)0-index) return 0;
        page=BasePage+index;
    } else page=Pages[(unsigned)index];
    if (page>(~(PAGING_U64)0>>12)) return 0;
    *Address=(page<<12)+within;
    return 1;
}
