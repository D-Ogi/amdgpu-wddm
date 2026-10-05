#include "paging_stream.h"

int PagingStreamBuild(void* context, PAGING_RESOLVE resolve, PAGING_EMIT emit,
                      int fill, PAGING_U64 src, PAGING_U64 dst, unsigned total, unsigned start,
                      unsigned* buffer, unsigned capacity, unsigned* written, unsigned* next)
{
    unsigned done=start, used=0;
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
        if (total-done<bytes) bytes=total-done;
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
