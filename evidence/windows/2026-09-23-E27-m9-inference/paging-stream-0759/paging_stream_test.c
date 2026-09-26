#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../paging_stream.h"
#define P 4096u
#define N 17u
#define BASE 0x400000ull
static unsigned char mem[2*N*P];
static unsigned calls;
static int failPage=-1, filling=0;
static int resolve(void* c,PAGING_U64 va,unsigned bytes,PAGING_U64* mc) {
    unsigned page,off=(unsigned)(va&(P-1)); int dst=va>=0x200000;
    PAGING_U64 relative=va-(dst?0x200000:0x100000);
    (void)c; page=(unsigned)(relative/P);
    if(page>=N || off+bytes>P || (dst && (int)page==failPage))return 0;
    *mc=BASE+((page*(dst?7:5))%N+(dst?N:0))*P+off;
    return 1;
}
static int emit(void* c,unsigned* b,unsigned cap,PAGING_U64 src,PAGING_U64 dst,unsigned bytes,unsigned* dw) {
    (void)c; if(cap<7)return 1;
    b[0]=0x1234;b[1]=(unsigned)src;b[2]=(unsigned)(src>>32);
    b[3]=(unsigned)dst;b[4]=(unsigned)(dst>>32);b[5]=bytes;b[6]=0;
    *dw=7;calls++;return 0;
}
static void execute(unsigned* b,unsigned n) {
    unsigned at;
    for(at=0;at<n;at+=7) {
        PAGING_U64 src=((PAGING_U64)b[at+2]<<32)|b[at+1];
        PAGING_U64 dst=((PAGING_U64)b[at+4]<<32)|b[at+3];
        if(b[at]!=0x1234 || dst<BASE || dst-BASE+b[at+5]>sizeof(mem))abort();
        if(filling)memset(mem+(size_t)(dst-BASE),0x5a,b[at+5]);
        else {
            if(src<BASE || src-BASE+b[at+5]>sizeof(mem))abort();
            memcpy(mem+(size_t)(dst-BASE),mem+(size_t)(src-BASE),b[at+5]);
        }
    }
}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void transfer(unsigned cap,int fill) {
    unsigned i,start=0,next=0,written=0,buf[128],iterations=0;
    unsigned sourceOffset=13,destOffset=fill?28:29,total=fill?N*P-64:N*P-41;
    memset(mem,0xcc,sizeof(mem));filling=fill;failPage=-1;
    for(i=0;i<N*P;i++) {PAGING_U64 mc;CHECK(resolve(NULL,0x100000+i,1,&mc));mem[(size_t)(mc-BASE)]=(unsigned char)(i*31u+i/251u);}
    do {
        int rc;
        for(i=0;i<128;i++)buf[i]=0xDEADBEEF;
        rc=PagingStreamBuild(NULL,resolve,emit,fill,0x100000+sourceOffset,0x200000+destOffset,total,start,buf,cap,&written,&next);
        CHECK(rc==PagingStreamDone || rc==PagingStreamMore);
        CHECK(written<=cap && next>start && next<=total);
        CHECK(buf[cap]==0xDEADBEEF);
        execute(buf,written);start=next;iterations++;
        CHECK(iterations<1000);
        CHECK((rc==PagingStreamDone)==(next==total));
    }while(start<total);
    for(i=0;i<N*P;i++) {
        PAGING_U64 mc;unsigned char expected=0xcc;
        CHECK(resolve(NULL,0x200000+i,1,&mc));
        if(i>=destOffset && i-destOffset<total) {unsigned source=i-destOffset+sourceOffset;expected=fill?0x5a:(unsigned char)(source*31u+source/251u);}
        CHECK(mem[(size_t)(mc-BASE)]==expected);
    }
}
static int linear(void* c,PAGING_U64 va,unsigned bytes,PAGING_U64* mc) {
    (void)c;CHECK((va&4095)+bytes<=4096);*mc=va;return 1;
}
int main(void) {
    unsigned b[256],written,next,start=0,i;int rc;
    CHECK(PagingStreamCapacity(65536,0,65536,1024,15,10)==1014);
    CHECK(PagingStreamCapacity(65536-4000,4000,65536,1024,15,10)==14);
    CHECK(PagingStreamCapacity(65536-4056,4056,65536,1024,15,10)==0);
    CHECK(PagingStreamCapacity(12,0,65536,1024,15,10)==3);
    CHECK(PagingStreamCapacity(65536,0,8,1024,15,10)==2);
    CHECK(PagingStreamCapacity(65536,1,65536,1024,15,10)==0);
    CHECK(PagingStreamCapacity(65536,0,65536,8,15,10)==0);
    transfer(7,0);transfer(14,0);transfer(127,0);transfer(7,1);transfer(127,1);
    rc=PagingStreamBuild(NULL,resolve,emit,0,0x100000,0x200000,8192,0,b,6,&written,&next);
    CHECK(rc==PagingStreamMore && written==0 && next==0);
    failPage=1;
    rc=PagingStreamBuild(NULL,resolve,emit,0,0x100000,0x200000,8192,0,b,256,&written,&next);
    CHECK(rc==PagingStreamAddress && written==0 && next==0);failPage=-1;
    CHECK(PagingStreamBuild(NULL,resolve,emit,0,~0ull-3,0,8,0,b,7,&written,&next)==PagingStreamInvalid);
    CHECK(PagingStreamBuild(NULL,resolve,emit,1,0,0x200001,8,0,b,7,&written,&next)==PagingStreamInvalid);
    CHECK(PagingStreamBuild(NULL,resolve,emit,0,0,0,8,9,b,7,&written,&next)==PagingStreamInvalid);
    calls=0;
    do {
        rc=PagingStreamBuild(NULL,linear,emit,0,0x100000000ull,0x200000000ull,1u<<30,start,b,256,&written,&next);
        CHECK(rc==PagingStreamDone || rc==PagingStreamMore);CHECK(next>start);
        for(i=0;i<written;i+=7) { CHECK(b[i+1]==start && b[i+2]==1); CHECK(b[i+3]==start && b[i+4]==2); CHECK(b[i+5]==P);start+=P; }
        CHECK(start==next);
    }while(rc==PagingStreamMore);
    CHECK(start==(1u<<30) && calls==(1u<<18));
    puts("PASS: fragmented copy/fill byte equality, unaligned copy, canaries, tiny buffers, fault rollback, overflow, live-ring reservation limits, 1GiB resume exactly once");
    return 0;
}
