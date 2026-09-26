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
    puts("PASS: independent pending DMA buffers, exact physical/virtual ranges, nonzero offsets, zero VA, malformed/truncated/gapped/overlapping records rejected before any callback");
    return 0;
}
