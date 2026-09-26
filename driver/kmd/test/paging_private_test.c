#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../paging_private.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);exit(1);}}while(0)
static unsigned out[64],used;
static void visit(void* context,const unsigned* words,unsigned count) {
    (void)context; CHECK(used+count<=64);memcpy(out+used,words,count*4);used+=count;
}
static unsigned record(unsigned* b,unsigned cap,unsigned offset,PAGING_ADDRESS base,unsigned seed,unsigned count) {
    unsigned i;CHECK(PagingPrivateHeader(b,cap,offset,base,count*4));
    for(i=0;i<count;i++)b[6+i]=seed+i;
    return PAGING_PRIVATE_HEADER_BYTES+count*4;
}
static void rejected(unsigned* b,unsigned capacity,PAGING_ADDRESS start,unsigned bytes,int virt) {
    used=0;CHECK(!PagingPrivateVisit(b,capacity,start,bytes,virt,visit,0));CHECK(used==0);
}
static unsigned mixedCalls;
static PAGING_PRIVATE_SPAN spans[3];
static int mixed(void* context,const PAGING_PRIVATE_SPAN* span) {
    (void)context;CHECK(mixedCalls<3);spans[mixedCalls++]=*span;return 1;
}
static void rejectMixed(unsigned* data,unsigned size,PAGING_ADDRESS start,unsigned bytes,int virt) {
    mixedCalls=0;CHECK(!PagingPrivateVisitMixed(data,size,start,bytes,virt,mixed,0));CHECK(!mixedCalls);
}
static void nativeRecords(void) {
    unsigned data[256],bad[256],other[16],off,n,at,csa,ib,i;
    const PAGING_ADDRESS base=0x400100000ull,root=0x180020000ull;
    for(off=4;off<=128;off+=4) {
        memset(data,0,sizeof(data));
        n=record(data,sizeof(data),0,base,0x100,off/4);at=n/4;
        csa=(0u-off)&63u;ib=csa+64u;
        CHECK(PagingPrivateNativeHeader(data+at,sizeof(data)-n,off,base,192,root,ib,8,csa));
        n+=PAGING_PRIVATE_NATIVE_BYTES;
        n+=record(data+n/4,sizeof(data)-n,off+192,base,0x200,4);
        mixedCalls=0;CHECK(PagingPrivateVisitMixed(data,n,base,off+192+16,1,mixed,0));
        CHECK(mixedCalls==3 && spans[0].Kind==PAGING_PRIVATE_DIRECT && spans[0].Dwords==off/4);
        for(i=0;i<off/4;i++)CHECK(spans[0].Words[i]==0x100+i);
        CHECK(spans[1].Kind==PAGING_PRIVATE_NATIVE && !spans[1].Words && spans[1].Dwords==8);
        CHECK(spans[1].RootPhysical==root && spans[1].IbAddress==base+off+ib && spans[1].CsaAddress==base+off+csa);
        CHECK(spans[2].Kind==PAGING_PRIVATE_DIRECT && spans[2].Dwords==4 && spans[2].Words[0]==0x200);
        // Select one whole native record, keeping its address identity and root.
        mixedCalls=0;CHECK(PagingPrivateVisitMixed(data,n,base+off,192,1,mixed,0));
        CHECK(mixedCalls==1 && spans[0].RootPhysical==root);
        // A pending different buffer at the same VA must not replace its root.
        CHECK(PagingPrivateNativeHeader(other,sizeof(other),off,base,192,root+4096,ib,8,csa));
        mixedCalls=0;CHECK(PagingPrivateVisitMixed(other,sizeof(other),base+off,192,1,mixed,0));
        CHECK(mixedCalls==1 && spans[0].RootPhysical==root+4096);
        mixedCalls=0;CHECK(PagingPrivateVisitMixed(data,n,base+off,192,1,mixed,0));
        CHECK(spans[0].RootPhysical==root);
        // Whole-range rejection precedes any direct-prefix callback.
        memcpy(bad,data,n);bad[at+8]=csa;rejectMixed(bad,n,base,off+208,1);
        memcpy(bad,data,n);bad[at+7]=bad[at+6]=0;rejectMixed(bad,n,base,off+208,1);
        memcpy(bad,data,n);bad[at+9]=9;rejectMixed(bad,n,base,off+208,1);
        rejectMixed(data,n,base+off+4,188,1);rejectMixed(data,n,base+off,188,1);
        rejectMixed(data,n,0,off+208,0);
        rejectMixed(data,n-4,base,off+208,1);
        // Legacy direct-only consumers must not execute metadata as SDMA words.
        used=0;CHECK(!PagingPrivateVisit(data,n,base,off+208,1,visit,0));CHECK(!used);
    }
    puts("PASS: 32 mixed DMA layouts, direct/native/direct order, isolated roots, whole-IB boundaries and disjoint CSA backing");
}
static void queuedRecords(void) {
    __declspec(align(8)) unsigned data[2048],other[2048];
    unsigned n,k,i,cap,bytes;void* slots[37];
    PAGING_ADDRESS base=0x400000;
    memset(data,0xCC,sizeof(data));
    for(i=0;i<32;i++)data[6+i]=0x100+i;
    CHECK(PagingPrivateQueuedHeader(data,sizeof(data),0,base,128));
    n=PagingPrivateQueuedSize(PAGING_PRIVATE_QUEUED_DIRECT,128);
    CHECK(PagingPrivateQueuedNativeHeader(data+n/4,sizeof(data)-n,128,base,192,0x9000,64,8,0));
    n+=PagingPrivateQueuedSize(PAGING_PRIVATE_QUEUED_NATIVE,192);
    for(i=0;i<4;i++)data[n/4+6+i]=0x200+i;
    CHECK(PagingPrivateQueuedHeader(data+n/4,sizeof(data)-n,320,base,16));
    n+=PagingPrivateQueuedSize(PAGING_PRIVATE_QUEUED_DIRECT,16);
    // Mutate every queue slot as if independently owned pending submissions
    // populated them. None of those bytes may become GPU command words.
    for(i=0;i<37;i++) {
        unsigned start=i<32?i*4:(i==32?128:320+(i-33)*4);
        unsigned count=i==32?192:4;
        unsigned* job=(unsigned*)PagingPrivateQueueSlot(data,n,base+start,count,1);
        CHECK(job!=0 && !((PAGING_ADDRESS)job&7u));slots[i]=job;
        for(k=0;k<i;k++)CHECK(slots[k]!=job);
        for(k=0;k<PAGING_PRIVATE_JOB_BYTES/4;k++)CHECK(job[k]==0);
        for(k=0;k<PAGING_PRIVATE_JOB_BYTES/4;k++)job[k]=0xDEAD0000+i;
    }
    mixedCalls=0;CHECK(PagingPrivateVisitMixed(data,n,base,336,1,mixed,0));
    CHECK(mixedCalls==3 && spans[0].Kind==PAGING_PRIVATE_DIRECT && spans[0].Dwords==32 &&
          spans[0].Words[0]==0x100 && spans[0].Words[31]==0x11F &&
          spans[1].Kind==PAGING_PRIVATE_NATIVE && spans[1].RootPhysical==0x9000 &&
          spans[2].Words[0]==0x200);
    memcpy(other,data,n);
    CHECK(PagingPrivateQueueSlot(other,n,base,4,1)!=slots[0]);
    CHECK(PagingPrivateQueueSlot(data,n,base,336,1)==slots[0]);
    CHECK(PagingPrivateQueueSlot(data,n,base+128,192,1)==slots[32]);
    CHECK(!PagingPrivateQueueSlot(data,n,base+132,188,1));
    CHECK(!PagingPrivateQueueSlot(data,n-4,base,336,1));
    CHECK(!PagingPrivateQueueSlot(data,n,base,340,1));
    CHECK(!PagingPrivateQueueSlot(data,n,base+1,4,1));
    // Existing physical DWORD slicing retains one unique queue slot per start.
    used=0;CHECK(PagingPrivateVisit(data,n,4,12,0,visit,0));
    CHECK(used==3 && out[0]==0x101 && out[2]==0x103);
    CHECK(PagingPrivateQueueSlot(data,n,4,12,0)==slots[1]);
    CHECK(PagingPrivateQueueSlot(data,n,8,8,0)==slots[2]);
    for(cap=0;cap<=PAGING_PRIVATE_BUFFER_BYTES;cap++) {
        bytes=PagingPrivateQueuedDirectCapacity(cap);
        CHECK(!(bytes&3u));
        if(bytes)CHECK(PagingPrivateQueuedSize(PAGING_PRIVATE_QUEUED_DIRECT,bytes)<=cap);
        CHECK(PagingPrivateQueuedSize(PAGING_PRIVATE_QUEUED_DIRECT,bytes+4)>cap);
    }
    memset(other,0xAB,sizeof(other));
    CHECK(!PagingPrivateQueuedHeader(other,95,0,base,4));
    for(i=0;i<2048;i++)CHECK(other[i]==0xABABABAB);
    CHECK(!PagingPrivateQueuedHeader(other+1,sizeof(other)-4,0,base,4));
    CHECK(!PagingPrivateQueuedNativeHeader(other,127,0,base,192,0x9000,64,8,0));
    CHECK(!PagingPrivateQueuedSize(PAGING_PRIVATE_QUEUED_DIRECT,65540));
    CHECK(!PagingPrivateQueuedSize(PAGING_PRIVATE_QUEUED_NATIVE,3));
    CHECK(!PagingPrivateQueuedSize(PAGING_PRIVATE_DIRECT,128));
    CHECK(PagingPrivateHeader(other,sizeof(other),0,base,4));
    CHECK(!PagingPrivateQueueSlot(other,sizeof(other),base,4,1));
    {
        unsigned size=PagingPrivateQueuedSize(PAGING_PRIVATE_QUEUED_DIRECT,65536);
        unsigned* full=(unsigned*)malloc(size+8);
        CHECK(full!=0);memset(full,0xA5,size+8);
        CHECK(PagingPrivateQueuedHeader(full,size,0,base,65536));
        for(i=0;i<16384;i++) {
            unsigned* slot=(unsigned*)PagingPrivateQueueSlot(full,size,base+i*4,4,1);
            CHECK(slot!=0 && slot>=full && (unsigned char*)slot+64<=(unsigned char*)full+size);
            CHECK(!slot[0]);slot[0]=i+1; // catches any reused starting slot
        }
        CHECK(full[6]==0xA5A5A5A5 && full[6+16383]==0xA5A5A5A5);
        CHECK(full[size/4]==0xA5A5A5A5 && full[size/4+1]==0xA5A5A5A5);
        free(full);
    }
    puts("PASS: OS-private queued records,37 mixed slots and all16384DWORD split starts, command isolation and all131073capacity values");
}


static void submissionRoots(void) {
    __declspec(align(8)) unsigned data[512]={0},before[512],bad[512];
    const PAGING_ADDRESS base=0x400100000ull,old=0x100000,newRoot=0x123456000ull;
    unsigned first,second,size;
    CHECK(PagingPrivateQueuedHeader(data,sizeof(data),0,base,16));
    first=PagingPrivateQueuedSize(PAGING_PRIVATE_QUEUED_DIRECT,16)/4;
    CHECK(PagingPrivateQueuedNativeHeader(data+first,sizeof(data)-first*4,16,base,192,old,112,8,48));
    second=first+32;
    CHECK(PagingPrivateQueuedNativeHeader(data+second,sizeof(data)-second*4,208,base,192,old+4096,112,8,48));
    size=(second+32)*4;
    memcpy(before,data,sizeof(data));
    CHECK(PagingPrivateBindRoot(data,size,base+16,192,newRoot));
    CHECK(data[first+6]==(unsigned)newRoot && data[first+7]==(unsigned)(newRoot>>32));
    CHECK(data[second+6]==before[second+6] && !memcmp(data,before,first*4));
    CHECK(PagingPrivateBindRoot(data,size,base,400,newRoot+4096));
    mixedCalls=0;CHECK(PagingPrivateVisitMixed(data,size,base,400,1,mixed,0));
    CHECK(mixedCalls==3 && spans[1].RootPhysical==newRoot+4096 && spans[2].RootPhysical==newRoot+4096);
    // Requeued DMA can acquire another current root without touching commands.
    CHECK(PagingPrivateBindRoot(data,size,base+208,192,newRoot+8192));
    CHECK(data[second+6]==(unsigned)(newRoot+8192));
    memcpy(before,data,sizeof(data));memcpy(bad,data,sizeof(data));bad[second+9]=7;
    memcpy(before,bad,sizeof(bad));
    CHECK(!PagingPrivateBindRoot(bad,size,base,400,newRoot));
    CHECK(!memcmp(before,bad,sizeof(bad)));
    memcpy(before,data,sizeof(data));
    CHECK(!PagingPrivateBindRoot(data,size,base,400,0));
    CHECK(!PagingPrivateBindRoot(data,size,base,400,newRoot+1));
    CHECK(!memcmp(before,data,sizeof(data)));
    CHECK(PagingPrivateBindRoot(data,size,base+4,8,0));
    CHECK(!memcmp(before,data,sizeof(data)));
    puts("PASS: submit-time roots, selected mixed ranges, replay rebinding, atomic refusal, unchanged direct commands");
}

int main(void) {
    unsigned a[64]={0},b[64]={0},bad[64],n,m,i;
    n=record(a,sizeof(a),0,0x100000000ull,10,4);
    n+=record(a+n/4,sizeof(a)-n,16,0x100000000ull,20,4);
    m=record(b,sizeof(b),0,0x200000000ull,100,8);
    // Build B after A, then submit A: no global buffer identity can select B.
    used=0;CHECK(PagingPrivateVisit(a,sizeof(a),0x100000000ull,32,1,visit,0));
    CHECK(used==8);for(i=0;i<4;i++){CHECK(out[i]==10+i);CHECK(out[4+i]==20+i);}
    used=0;CHECK(PagingPrivateVisit(b,sizeof(b),0x200000000ull,32,1,visit,0));CHECK(used==8 && out[0]==100);
    // Nonzero physical command offset and physical private-record start offset.
    used=0;CHECK(PagingPrivateVisit(a+10,n-40,16,16,0,visit,0));CHECK(used==4 && out[0]==20);
    used=0;CHECK(PagingPrivateVisit(a,n,8,16,0,visit,0));CHECK(used==4 && out[0]==12 && out[2]==20);
    // Whole requested range validates BEFORE the first visitor call.
    memcpy(bad,a,sizeof(a));bad[10]=0;rejected(bad,n,0,32,0);
    memcpy(bad,a,sizeof(a));bad[12]=20;rejected(bad,n,0,32,0); // gap
    memcpy(bad,a,sizeof(a));bad[12]=12;rejected(bad,n,0,32,0); // overlap
    memcpy(bad,a,sizeof(a));bad[11]=65536;rejected(bad,n,0,32,0); // oversized record
    memcpy(bad,a,sizeof(a));bad[15]=1;rejected(bad,n,0,32,0); // reserved field
    rejected(a,n-4,0,32,0);rejected(a,n,0,36,0); // truncation, no clamping
    rejected(a,n,0x200000000ull,16,1); // wrong virtual identity
    rejected(a,n,1,4,0);rejected(a,n,~0ull-3,8,1); // alignment/overflow
    // Zero base (observed paging convention), empty capacity, no stale tail record.
    memset(bad,0,sizeof(bad));m=record(bad,sizeof(bad),4,0,77,2);
    used=0;CHECK(PagingPrivateVisit(bad,m,4,8,1,visit,0));CHECK(used==2 && out[0]==77);
    rejected(bad,sizeof(bad),4,12,1);rejected(bad,0,4,8,1);
    CHECK(!PagingPrivateHeader(bad,24,0,0,4));
    CHECK(!PagingPrivateHeader(bad,sizeof(bad),65536,0,4));
    CHECK(!PagingPrivateHeader(bad,sizeof(bad),0,~0ull-3,4));
    submissionRoots();
    nativeRecords();
    queuedRecords();
    puts("PASS: independent pending DMA buffers, exact physical/virtual ranges, nonzero offsets, zero VA, malformed/truncated/gapped/overlapping records rejected before any callback");
    return 0;
}
