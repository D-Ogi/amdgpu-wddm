#include "paging_stream.h"

int PagingStreamBuildEndpoints64(void* context, PAGING_RESOLVE sourceResolve,
                      PAGING_RESOLVE destinationResolve, PAGING_EMIT emit,
                      int fill, PAGING_U64 src, PAGING_U64 dst, PAGING_U64 total, PAGING_U64 start,
                      unsigned* buffer, unsigned capacity, unsigned* written, PAGING_U64* next)
{
    PAGING_U64 done=start;
    unsigned used=0;
    if (!written || !next) return PagingStreamInvalid;
    *written=0; *next=start;
    if ((!fill && !sourceResolve) || !destinationResolve || !emit || !buffer || !total || start>total ||
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
        if (!destinationResolve(context,dst+done,bytes,&destination) ||
            (!fill && !sourceResolve(context,src+done,bytes,&source))) return PagingStreamAddress;
        rc=emit(context,buffer+used,capacity-used,source,destination,bytes,&dw);
        if (rc==1) { *written=used; *next=done; return PagingStreamMore; }
        if (rc!=0 || !dw || dw>capacity-used) return PagingStreamInvalid;
        used+=dw; done+=bytes;
    }
    *written=used; *next=done;
    return PagingStreamDone;
}

int PagingStreamBuild64(void* context, PAGING_RESOLVE resolve, PAGING_EMIT emit,
                      int fill, PAGING_U64 src, PAGING_U64 dst, PAGING_U64 total, PAGING_U64 start,
                      unsigned* buffer, unsigned capacity, unsigned* written, PAGING_U64* next)
{
    if (!resolve) {
        if (written && next) { *written=0; *next=start; }
        return PagingStreamInvalid;
    }
    return PagingStreamBuildEndpoints64(context,resolve,resolve,emit,fill,src,dst,
                                       total,start,buffer,capacity,written,next);
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

static int TokenGeometry(int Fill, PAGING_U64 Source, PAGING_U64 Destination,
    PAGING_U64 Total, unsigned* Low, unsigned* High, unsigned* Count)
{
    unsigned a=4096u-(unsigned)(Destination&4095u);
    unsigned b=Fill ? a : 4096u-(unsigned)(Source&4095u);
    PAGING_U64 count=1;
    if (!Total || Destination>~(PAGING_U64)0-Total ||
        (!Fill && Source>~(PAGING_U64)0-Total) ||
        (Fill && ((Destination|Total)&3))) return 0;
    *Low=a<b?a:b;*High=a<b?b:a;
    // Count boundaries strictly before the end, then add the terminal slice.
    if(Total>*Low)count+=(Total-1-*Low)/4096+1;
    if(*High!=*Low && Total>*High)count+=(Total-1-*High)/4096+1;
    if(count>0xffffffffull)return 0;
    *Count=(unsigned)count;return 1;
}

int PagingStreamTokenDecode(int Fill, PAGING_U64 Source, PAGING_U64 Destination,
    PAGING_U64 Total, unsigned Token, PAGING_U64* Progress)
{
    unsigned low,high,count;
    PAGING_U64 n;
    if(!Progress)return 0;
    *Progress=0;
    if(!TokenGeometry(Fill,Source,Destination,Total,&low,&high,&count) || Token>count)return 0;
    if(!Token)return 1;
    if(Token==count){*Progress=Total;return 1;}
    n=(PAGING_U64)Token-1;
    *Progress=low==high ? low+n*4096 : (n/2)*4096+(n&1?high:low);
    return 1;
}

int PagingStreamTokenEncode(int Fill, PAGING_U64 Source, PAGING_U64 Destination,
    PAGING_U64 Total, PAGING_U64 Progress, unsigned* Token)
{
    unsigned low,high,count;
    PAGING_U64 token,phase;
    if(!Token)return 0;
    *Token=0;
    if(!TokenGeometry(Fill,Source,Destination,Total,&low,&high,&count) || Progress>Total)return 0;
    if(!Progress)return 1;
    if(Progress==Total){*Token=count;return 1;}
    if(low==high) {
        if(Progress<low || (Progress-low)%4096)return 0;
        token=1+(Progress-low)/4096;
    } else {
        phase=(Progress-1)%4096+1;
        if(phase!=low && phase!=high)return 0;
        token=2*((Progress-1)/4096)+(phase==low?1:2);
    }
    if(token>=count)return 0;
    *Token=(unsigned)token;return 1;
}
