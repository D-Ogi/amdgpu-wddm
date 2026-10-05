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
static void page_lists(void)
{
    PAGING_U64 pages[17],address;
    unsigned first,index,offset,checks=0;
    for(index=0;index<17;index++)pages[index]=0x10000+((index*7)%17)*3;
    for(first=0;first<17;first++)for(index=0;index<17-first;index++)for(offset=0;offset<4096;offset+=127) {
        unsigned bytes=4096-offset;
        CHECK(PagingPageListAddress(pages,17,0,0,first,(PAGING_U64)index*4096+offset,bytes,&address));
        CHECK(address==(pages[first+index]<<12)+offset);
        CHECK(PagingPageListAddress(NULL,17,0x20000,1,first,(PAGING_U64)index*4096+offset,bytes,&address));
        CHECK(address==((0x20000ull+first+index)<<12)+offset);
        checks+=4;
    }
    address=123;CHECK(!PagingPageListAddress(pages,17,0,0,16,4096,1,&address) && !address);
    CHECK(!PagingPageListAddress(pages,17,0,0,17,0,1,&address));
    CHECK(!PagingPageListAddress(pages,17,0,0,0,4095,2,&address));
    CHECK(!PagingPageListAddress(pages,17,0,0,0,0,0,&address));
    CHECK(!PagingPageListAddress(NULL,17,0,0,0,0,1,&address));
    CHECK(!PagingPageListAddress(pages,0,0,0,0,0,1,&address));
    CHECK(!PagingPageListAddress(pages,17,0,2,0,0,1,&address));
    CHECK(!PagingPageListAddress(pages,17,0,0,0,~0ull,1,&address));
    CHECK(!PagingPageListAddress(NULL,17,~0ull,1,1,0,1,&address));
    CHECK(!PagingPageListAddress(NULL,17,(~0ull>>12),1,1,0,1,&address));
    CHECK(PagingPageListAddress(NULL,1,(~0ull>>12),1,0,4095,1,&address) && address==~0ull);
    pages[0]=1ull<<52;CHECK(!PagingPageListAddress(pages,17,0,0,0,0,1,&address));
    CHECK(!PagingPageListAddress(pages,17,0,0,0,0,1,NULL));
    printf("page-list resolution: %u address checks plus13 boundary controls\n",checks);
}

static int linear_fail(void* c,PAGING_U64 va,unsigned bytes,PAGING_U64* mc) {
    if (va>=*(PAGING_U64*)c) return 0;
    return linear(NULL,va,bytes,mc);
}
static void wide_progress(void) {
    const PAGING_U64 boundary=1ull<<32;
    unsigned b[257],written,i,mode;
    PAGING_U64 next,start,total,source,dest,covered,limit;
    int rc;
    for(mode=0;mode<3;mode++) {
        int fill=mode==2;
        source=boundary+13;dest=2*boundary+(fill?28:29);
        total=boundary+(fill?8192:8193);
        start=mode?boundary-8192:0;
        do {
            covered=start;b[256]=0xDEADBEEF;
            rc=PagingStreamBuild64(NULL,linear,emit,fill,source,dest,total,start,b,256,&written,&next);
            CHECK(rc==PagingStreamDone || rc==PagingStreamMore);
            CHECK(written<=256 && written%7==0 && next>start && next<=total);
            CHECK(b[256]==0xDEADBEEF);
            for(i=0;i<written;i+=7) {
                PAGING_U64 src=((PAGING_U64)b[i+2]<<32)|b[i+1];
                PAGING_U64 dst=((PAGING_U64)b[i+4]<<32)|b[i+3];
                CHECK(src==(fill?0:source+covered) && dst==dest+covered);
                CHECK(b[i+5]>0 && b[i+5]<=total-covered);
                CHECK((dst&4095)+b[i+5]<=4096);
                CHECK(fill?!(b[i+5]&3):(src&4095)+b[i+5]<=4096);
                covered+=b[i+5];
            }
            CHECK(next==covered && (rc==PagingStreamDone)==(next==total));
            start=next;
        } while(rc==PagingStreamMore);
        CHECK(start==total);
    }
    start=boundary+4096;total=start+8192;
    CHECK(PagingStreamBuild64(NULL,linear,emit,0,0,0,total,start,b,6,&written,&next)==PagingStreamMore);
    CHECK(written==0 && next==start);
    limit=start+4096;
    CHECK(PagingStreamBuild64(&limit,linear_fail,emit,0,0,0,total,start,b,256,&written,&next)==PagingStreamAddress);
    CHECK(written==0 && next==start); // Tentative first packet is not published.
    CHECK(PagingStreamBuild64(NULL,linear,emit,0,~0ull-3,0,8,0,b,256,&written,&next)==PagingStreamInvalid);
    CHECK(PagingStreamBuild64(NULL,linear,emit,0,0,~0ull-3,8,0,b,256,&written,&next)==PagingStreamInvalid);
    CHECK(PagingStreamBuild64(NULL,linear,emit,0,0,0,total,total+1,b,256,&written,&next)==PagingStreamInvalid);
    CHECK(PagingStreamBuild64(NULL,linear,emit,1,0,0,total,start+1,b,256,&written,&next)==PagingStreamInvalid);
    CHECK(PagingStreamBuild64(NULL,linear,emit,0,0,0,total,total,b,256,&written,&next)==PagingStreamDone);
    CHECK(written==0 && next==total);
    puts("PASS: >4GiB exact byte coverage, unaligned copy/fill carry, wide resume, rollback and overflow");
}

static void progress_tokens(void)
{
    unsigned source,dest,fill,token,encoded,b[8],written,cases=0;
    PAGING_U64 progress,next,total,decoded;
    int rc;
    for(fill=0;fill<2;fill++)for(source=0;source<4096;source+=127)
    for(dest=0;dest<4096;dest+=124) {
        total=fill?3*4096+28:3*4096+29;token=0;progress=0;
        do {
            CHECK(PagingStreamTokenDecode((int)fill,source,dest,total,token,&decoded) && decoded==progress);
            rc=PagingStreamBuild64(NULL,linear,emit,(int)fill,source,dest,total,decoded,b,7,&written,&next);
            CHECK((rc==PagingStreamDone || rc==PagingStreamMore) && written==7 && next>progress);
            CHECK(PagingStreamTokenEncode((int)fill,source,dest,total,next,&encoded) && encoded==token+1);
            CHECK(PagingStreamTokenDecode((int)fill,source,dest,total,encoded,&decoded) && decoded==next);
            token=encoded;progress=next;cases++;
        }while(rc==PagingStreamMore);
        CHECK(progress==total);
    }
    // Decode/encode next to the4GiB carry, validated by actual emitted progress.
    total=(1ull<<32)+8192+29;
    for(token=(1u<<21)-3;token<(1u<<21)+3;token++) {
        CHECK(PagingStreamTokenDecode(0,0,29,total,token,&progress));
        rc=PagingStreamBuild64(NULL,linear,emit,0,0,29,total,progress,b,7,&written,&next);
        CHECK(rc==PagingStreamMore && written==7);
        CHECK(PagingStreamTokenEncode(0,0,29,total,next,&encoded) && encoded==token+1);
    }
    total=0xffffffffull*4096;
    CHECK(PagingStreamTokenEncode(0,0,0,total,total,&encoded) && encoded==0xffffffffu);
    CHECK(PagingStreamTokenDecode(0,0,0,total,0xffffffffu,&decoded) && decoded==total);
    CHECK(!PagingStreamTokenDecode(0,0,0,total+1,0,&decoded) && !decoded);
    CHECK(!PagingStreamTokenEncode(0,0,0,total+1,0,&encoded) && !encoded);
    CHECK(!PagingStreamTokenEncode(0,0,1,8192,1,&encoded) && !encoded);
    CHECK(!PagingStreamTokenDecode(0,0,1,8192,5,&decoded) && !decoded);
    CHECK(!PagingStreamTokenEncode(0,0,0,8,9,&encoded) && !encoded);
    CHECK(!PagingStreamTokenDecode(0,~0ull-3,0,8,0,&decoded));
    CHECK(!PagingStreamTokenDecode(0,0,~0ull-3,8,0,&decoded));
    CHECK(!PagingStreamTokenDecode(1,0,1,8,0,&decoded));
    CHECK(!PagingStreamTokenDecode(1,0,0,7,0,&decoded));
    CHECK(!PagingStreamTokenDecode(0,0,0,0,0,&decoded));
    CHECK(!PagingStreamTokenEncode(0,0,0,8,0,NULL));
    CHECK(!PagingStreamTokenDecode(0,0,0,8,0,NULL));
    printf("PASS: UINT slice tokens match %u actual stream boundaries, wide carry and capacity controls\n",cases);
}

int main(void) {
    unsigned b[256],written,next,start=0,i;int rc;
    page_lists();
    wide_progress();
    progress_tokens();
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
