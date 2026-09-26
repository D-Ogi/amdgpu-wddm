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
    nativeRecords();
    puts("PASS: independent pending DMA buffers, exact physical/virtual ranges, nonzero offsets, zero VA, malformed/truncated/gapped/overlapping records rejected before any callback");
    return 0;
}
