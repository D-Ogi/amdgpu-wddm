#include "paging_private.h"
#define PRIVATE_MAGIC 0x36504742u

int PagingPrivateHeader(unsigned* r,unsigned capacity,unsigned offset,PAGING_ADDRESS base,unsigned bytes)
{
    if (!r || ((offset|bytes)&3) || !bytes || capacity<PAGING_PRIVATE_HEADER_BYTES ||
        bytes>capacity-PAGING_PRIVATE_HEADER_BYTES || offset>65536u || bytes>65536u-offset ||
        base>~(PAGING_ADDRESS)0-offset-bytes) return 0;
    r[0]=PRIVATE_MAGIC;r[1]=bytes;r[2]=offset;r[3]=(unsigned)base;r[4]=(unsigned)(base>>32);r[5]=0;
    return 1;
}

static int Walk(const unsigned* data,unsigned capacity,PAGING_ADDRESS start,unsigned bytes,
                int virtualAddress,PAGING_PRIVATE_VISITOR visit,void* context)
{
    unsigned cursor=0,left=bytes;
    PAGING_ADDRESS expected=start;
    int matched=0;
    while(cursor<capacity && left) {
        const unsigned* r;
        PAGING_ADDRESS base,address,end;
        unsigned offset,count,skip,take;
        if(capacity-cursor<PAGING_PRIVATE_HEADER_BYTES)return 0;
        r=data+cursor/4;count=r[1];offset=r[2];
        if(r[0]!=PRIVATE_MAGIC || r[5] || !count || ((count|offset)&3) ||
           count>capacity-cursor-PAGING_PRIVATE_HEADER_BYTES || offset>65536u || count>65536u-offset)return 0;
        base=((PAGING_ADDRESS)r[4]<<32)|r[3];
        if(base>~(PAGING_ADDRESS)0-offset-count)return 0;
        address=(virtualAddress?base:0)+offset;end=address+count;
        if(!matched && expected>=end) { cursor+=PAGING_PRIVATE_HEADER_BYTES+count;continue; }
        if(expected<address || expected>=end || (matched && expected!=address))return 0;
        skip=(unsigned)(expected-address);take=count-skip;if(take>left)take=left;
        if(visit)visit(context,r+PAGING_PRIVATE_HEADER_BYTES/4+skip/4,take/4);
        expected+=take;left-=take;matched=1;cursor+=PAGING_PRIVATE_HEADER_BYTES+count;
    }
    return left==0;
}

int PagingPrivateVisit(const void* data,unsigned capacity,PAGING_ADDRESS start,unsigned bytes,
                       int virtualAddress,PAGING_PRIVATE_VISITOR visit,void* context)
{
    if(!data || !bytes || ((start|bytes|capacity)&3) || start>~(PAGING_ADDRESS)0-bytes)return 0;
    if(!Walk((const unsigned*)data,capacity,start,bytes,virtualAddress,0,0))return 0;
    return !visit || Walk((const unsigned*)data,capacity,start,bytes,virtualAddress,visit,context);
}
