#include "paging_private.h"
#define PRIVATE_MAGIC 0x36504742u

static int RangeValid(unsigned offset,PAGING_ADDRESS base,unsigned bytes)
{
    return !((offset|bytes)&3u) && bytes && offset<=65536u && bytes<=65536u-offset &&
           base<=~(PAGING_ADDRESS)0-offset-bytes;
}

int PagingPrivateHeader(unsigned* r,unsigned capacity,unsigned offset,PAGING_ADDRESS base,unsigned bytes)
{
    if (!r || !RangeValid(offset,base,bytes) || capacity<PAGING_PRIVATE_HEADER_BYTES ||
        bytes>capacity-PAGING_PRIVATE_HEADER_BYTES) return 0;
    r[0]=PRIVATE_MAGIC;r[1]=bytes;r[2]=offset;r[3]=(unsigned)base;r[4]=(unsigned)(base>>32);r[5]=0;
    return 1;
}

static int NativeValid(PAGING_ADDRESS address,unsigned bytes,PAGING_ADDRESS root,
                       unsigned ibOffset,unsigned ibDwords,unsigned csaOffset)
{
    unsigned ibBytes;
    if (!address || !root || (root&4095u) || !ibDwords || (ibDwords&7u) ||
        ibOffset>bytes || ibDwords>(bytes-ibOffset)/4u || csaOffset>bytes ||
        PAGING_PRIVATE_CSA_BYTES>bytes-csaOffset ||
        ((address+ibOffset)&31u) || ((address+csaOffset)&63u)) return 0;
    ibBytes=ibDwords*4u;
    return ibOffset>=csaOffset+PAGING_PRIVATE_CSA_BYTES || csaOffset>=ibOffset+ibBytes;
}

int PagingPrivateNativeHeader(unsigned* r,unsigned capacity,unsigned offset,
    PAGING_ADDRESS base,unsigned bytes,PAGING_ADDRESS root,
    unsigned ibOffset,unsigned ibDwords,unsigned csaOffset)
{
    if (!r || capacity<PAGING_PRIVATE_NATIVE_BYTES || !RangeValid(offset,base,bytes) || !base ||
        !NativeValid(base+offset,bytes,root,ibOffset,ibDwords,csaOffset)) return 0;
    r[0]=PRIVATE_MAGIC;r[1]=bytes;r[2]=offset;r[3]=(unsigned)base;r[4]=(unsigned)(base>>32);
    r[5]=PAGING_PRIVATE_NATIVE;r[6]=(unsigned)root;r[7]=(unsigned)(root>>32);
    r[8]=ibOffset;r[9]=ibDwords;r[10]=csaOffset;r[11]=PAGING_PRIVATE_CSA_BYTES;
    r[12]=r[13]=r[14]=r[15]=0;
    return 1;
}

unsigned PagingPrivateQueuedSize(unsigned kind,unsigned bytes)
{
    unsigned header,slots;
    if(!bytes || (bytes&3u) || bytes>65536u)return 0;
    if(kind==PAGING_PRIVATE_QUEUED_DIRECT) {
        header=PAGING_PRIVATE_HEADER_BYTES+bytes;slots=bytes/4u;
    } else if(kind==PAGING_PRIVATE_QUEUED_NATIVE) {
        header=PAGING_PRIVATE_NATIVE_BYTES;slots=1;
    } else return 0;
    return ((header+7u)&~7u)+slots*PAGING_PRIVATE_JOB_BYTES;
}

unsigned PagingPrivateQueuedDirectCapacity(unsigned capacity)
{
    unsigned words,size;
    if(capacity<PAGING_PRIVATE_HEADER_BYTES)return 0;
    words=(capacity-PAGING_PRIVATE_HEADER_BYTES)/(4u+PAGING_PRIVATE_JOB_BYTES);
    if(words>65536u/4u)words=65536u/4u;
    if(!words)return 0;
    size=PagingPrivateQueuedSize(PAGING_PRIVATE_QUEUED_DIRECT,words*4u);
    if(size>capacity)--words;
    return words*4u;
}

static void ClearQueuedStorage(unsigned* r,unsigned prefix,unsigned total)
{
    unsigned i;
    for(i=prefix/4u;i<total/4u;i++)r[i]=0;
}

int PagingPrivateQueuedHeader(unsigned* r,unsigned capacity,unsigned offset,
    PAGING_ADDRESS base,unsigned bytes)
{
    unsigned size=PagingPrivateQueuedSize(PAGING_PRIVATE_QUEUED_DIRECT,bytes);
    if(!r || ((PAGING_ADDRESS)r&7u) || !size || capacity<size ||
       !PagingPrivateHeader(r,capacity,offset,base,bytes))return 0;
    ClearQueuedStorage(r,PAGING_PRIVATE_HEADER_BYTES+bytes,size);
    r[5]=PAGING_PRIVATE_QUEUED_DIRECT;
    return 1;
}

int PagingPrivateQueuedNativeHeader(unsigned* r,unsigned capacity,unsigned offset,
    PAGING_ADDRESS base,unsigned bytes,PAGING_ADDRESS root,
    unsigned ibOffset,unsigned ibDwords,unsigned csaOffset)
{
    unsigned size=PagingPrivateQueuedSize(PAGING_PRIVATE_QUEUED_NATIVE,bytes);
    if(!r || ((PAGING_ADDRESS)r&7u) || !size || capacity<size ||
       !PagingPrivateNativeHeader(r,capacity,offset,base,bytes,root,ibOffset,ibDwords,csaOffset))return 0;
    ClearQueuedStorage(r,PAGING_PRIVATE_NATIVE_BYTES,size);
    r[5]=PAGING_PRIVATE_QUEUED_NATIVE;
    return 1;
}

static int Walk(const unsigned* data,unsigned capacity,PAGING_ADDRESS start,unsigned bytes,
                int virtualAddress,int nativeAllowed,PAGING_PRIVATE_MIXED_VISITOR visit,void* context)
{
    unsigned cursor=0,left=bytes;
    PAGING_ADDRESS expected=start;
    int matched=0;
    while(cursor<capacity && left) {
        const unsigned* r;
        PAGING_ADDRESS base,address,end;
        unsigned offset,count,skip,take,privateBytes,queued;
        PAGING_PRIVATE_SPAN span={0};
        if(capacity-cursor<PAGING_PRIVATE_HEADER_BYTES)return 0;
        r=data+cursor/4;count=r[1];offset=r[2];
        base=((PAGING_ADDRESS)r[4]<<32)|r[3];
        if(r[0]!=PRIVATE_MAGIC || !RangeValid(offset,base,count))return 0;
        address=(virtualAddress?base:0)+offset;end=address+count;
        if(r[5]>PAGING_PRIVATE_QUEUED_NATIVE)return 0;
        queued=r[5]>=PAGING_PRIVATE_QUEUED_DIRECT;
        span.Kind=r[5]&1u;
        if(span.Kind==PAGING_PRIVATE_DIRECT) {
            if(count>capacity-cursor-PAGING_PRIVATE_HEADER_BYTES)return 0;
            privateBytes=PAGING_PRIVATE_HEADER_BYTES+count;
        } else if(span.Kind==PAGING_PRIVATE_NATIVE) {
            if(!nativeAllowed || !virtualAddress || !base || capacity-cursor<PAGING_PRIVATE_NATIVE_BYTES)return 0;
            span.RootPhysical=((PAGING_ADDRESS)r[7]<<32)|r[6];
            if(r[11]!=PAGING_PRIVATE_CSA_BYTES || r[12] || r[13] || r[14] || r[15] ||
               !NativeValid(address,count,span.RootPhysical,r[8],r[9],r[10]))return 0;
            privateBytes=PAGING_PRIVATE_NATIVE_BYTES;
            span.IbAddress=address+r[8];span.CsaAddress=address+r[10];span.Dwords=r[9];
        } else return 0;
        if(queued) {
            if((PAGING_ADDRESS)r&7u)return 0;
            privateBytes=PagingPrivateQueuedSize(r[5],count);
            if(!privateBytes || privateBytes>capacity-cursor)return 0;
        }
        if(!matched && expected>=end) {cursor+=privateBytes;continue;}
        if(expected<address || expected>=end || (matched && expected!=address))return 0;
        skip=(unsigned)(expected-address);take=count-skip;if(take>left)take=left;
        if(span.Kind==PAGING_PRIVATE_NATIVE) {
            if(skip || take!=count)return 0;
        } else {
            span.Words=r+PAGING_PRIVATE_HEADER_BYTES/4+skip/4;span.Dwords=take/4;
        }
        if(visit && !visit(context,&span))return 0;
        expected+=take;left-=take;matched=1;cursor+=privateBytes;
    }
    return left==0;
}

static int Visit(const void* data,unsigned capacity,PAGING_ADDRESS start,unsigned bytes,
                 int virtualAddress,int nativeAllowed,PAGING_PRIVATE_MIXED_VISITOR visit,void* context)
{
    if(!data || !bytes || ((start|bytes|capacity)&3) || start>~(PAGING_ADDRESS)0-bytes)return 0;
    if(!Walk((const unsigned*)data,capacity,start,bytes,virtualAddress,nativeAllowed,0,0))return 0;
    return !visit || Walk((const unsigned*)data,capacity,start,bytes,virtualAddress,nativeAllowed,visit,context);
}

int PagingPrivateVisitMixed(const void* data,unsigned capacity,PAGING_ADDRESS start,
    unsigned bytes,int virtualAddress,PAGING_PRIVATE_MIXED_VISITOR visit,void* context)
{
    return Visit(data,capacity,start,bytes,virtualAddress,1,visit,context);
}

typedef struct LEGACY_VISITOR {PAGING_PRIVATE_VISITOR Visit;void* Context;} LEGACY_VISITOR;
static int LegacyVisit(void* context,const PAGING_PRIVATE_SPAN* span)
{
    LEGACY_VISITOR* legacy=(LEGACY_VISITOR*)context;
    legacy->Visit(legacy->Context,span->Words,span->Dwords);
    return 1;
}

int PagingPrivateVisit(const void* data,unsigned capacity,PAGING_ADDRESS start,unsigned bytes,
                       int virtualAddress,PAGING_PRIVATE_VISITOR visit,void* context)
{
    LEGACY_VISITOR legacy={visit,context};
    return Visit(data,capacity,start,bytes,virtualAddress,0,visit?LegacyVisit:0,&legacy);
}

void* PagingPrivateQueueSlot(void* data,unsigned capacity,PAGING_ADDRESS start,
    unsigned bytes,int virtualAddress)
{
    unsigned cursor=0;
    if(!PagingPrivateVisitMixed(data,capacity,start,bytes,virtualAddress,0,0))return 0;
    // The validated walk guarantees every record size/range used here.
    while(cursor<capacity) {
        unsigned* r=(unsigned*)data+cursor/4u;
        PAGING_ADDRESS base=((PAGING_ADDRESS)r[4]<<32)|r[3];
        PAGING_ADDRESS address=(virtualAddress?base:0)+r[2];
        unsigned size=r[5]>=PAGING_PRIVATE_QUEUED_DIRECT ? PagingPrivateQueuedSize(r[5],r[1]) :
            (r[5]==PAGING_PRIVATE_NATIVE ? PAGING_PRIVATE_NATIVE_BYTES : PAGING_PRIVATE_HEADER_BYTES+r[1]);
        if(start>=address && start<address+r[1]) {
            unsigned prefix,index;
            if(r[5]<PAGING_PRIVATE_QUEUED_DIRECT)return 0;
            prefix=r[5]==PAGING_PRIVATE_QUEUED_DIRECT ? PAGING_PRIVATE_HEADER_BYTES+r[1] : PAGING_PRIVATE_NATIVE_BYTES;
            index=r[5]==PAGING_PRIVATE_QUEUED_DIRECT ? (unsigned)(start-address)/4u : 0;
            return (unsigned char*)r+((prefix+7u)&~7u)+index*PAGING_PRIVATE_JOB_BYTES;
        }
        cursor+=size;
    }
    return 0;
}
