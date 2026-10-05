#include <stdio.h>
#include <string.h>
#include "paging_pt_shadow.h"
static PAGING_PT_SHADOW_SLOT slots[4], before[4];
static unsigned checks,failures;
#define CHECK(x) do {checks++;if(!(x)){failures++;printf("FAIL line %u: %s\n",(unsigned)__LINE__,#x);}} while(0)
static void test_copy(void)
{
    PAGING_PT_SHADOW s={0};PAGING_PT_U64 values[512],v;
    PAGING_PT_U64 model[16],snapshot[16];unsigned char known[16],saved[16];
    unsigned i,src,dst,count;
    for(i=0;i<512;i++)values[i]=0x123400000ull+i;
    CHECK(PagingPtShadowInit(&s,slots,4)==PAGING_PT_OK);
    CHECK(PagingPtShadowApply(&s,0,0,512,values,1)==PAGING_PT_OK);
    v=0;CHECK(PagingPtShadowApply(&s,0,123,1,&v,0)==PAGING_PT_OK);
    CHECK(PagingPtShadowApply(&s,16384,0,1,values,1)==PAGING_PT_OK);
    CHECK(PagingPtShadowCopy(&s,0,0,16384,0,512)==PAGING_PT_OK);
    for(i=0;i<512;i++)CHECK(PagingPtShadowRead(&s,16384,i,&v)==PAGING_PT_OK && v==(i==123?0:values[i]));
    // Missing source is unknown, never the stale old destination or a known zero.
    CHECK(PagingPtShadowCopy(&s,32768,0,16384,63,3)==PAGING_PT_OK);
    for(i=63;i<66;i++)CHECK(PagingPtShadowRead(&s,16384,i,&v)==PAGING_PT_MISSING && v==0);
    CHECK(PagingPtShadowRead(&s,16384,62,&v)==PAGING_PT_OK && v==values[62]);
    CHECK(PagingPtShadowRead(&s,16384,66,&v)==PAGING_PT_OK && v==values[66]);
    memcpy(before,slots,sizeof(slots));
    CHECK(PagingPtShadowCopy(&s,0,0,32768,0,1)==PAGING_PT_MISSING && s.Used==2);
    CHECK(PagingPtShadowCopy(&s,0,511,16384,0,2)==PAGING_PT_INVALID);
    CHECK(PagingPtShadowCopy(&s,0,0,16384,511,2)==PAGING_PT_INVALID);
    CHECK(PagingPtShadowCopy(&s,1,0,16384,0,1)==PAGING_PT_INVALID);
    CHECK(PagingPtShadowCopy(&s,0,0,1ull<<48,0,1)==PAGING_PT_INVALID);
    CHECK(PagingPtShadowCopy(&s,0,0,16384,0,0)==PAGING_PT_INVALID);
    CHECK(memcmp(before,slots,sizeof(slots))==0);
    // Independent snapshot oracle, both overlap directions, self-copy and
    // disjoint subranges. Base60 crosses the64-bit Known bitmap boundary.
    for(src=0;src<8;src++)for(dst=0;dst<8;dst++)for(count=1;count<=8;count++) {
        CHECK(PagingPtShadowInit(&s,slots,4)==PAGING_PT_OK);
        for(i=0;i<16;i++) {
            model[i]=snapshot[i]=(i==5?0:100+i);known[i]=saved[i]=(unsigned char)(i%3!=1);
            if(known[i])CHECK(PagingPtShadowApply(&s,0,60+i,1,model+i,1)==PAGING_PT_OK);
        }
        for(i=0;i<count;i++){model[dst+i]=snapshot[src+i];known[dst+i]=saved[src+i];}
        CHECK(PagingPtShadowCopy(&s,0,60+src,0,60+dst,count)==PAGING_PT_OK);
        for(i=0;i<16;i++) {
            int status=PagingPtShadowRead(&s,0,60+i,&v);
            CHECK(known[i]?(status==PAGING_PT_OK && v==model[i]):(status==PAGING_PT_MISSING && v==0));
        }
    }
}
int main(void)
{
    PAGING_PT_SHADOW s={0};PAGING_PT_U64 values[512],v=123;unsigned i;
    CHECK(PagingPtShadowTableCount(1ull<<30,4)==515);
    CHECK(PagingPtShadowTableCount(1ull<<30,3)==514);
    CHECK(PagingPtShadowTableCount(1ull<<30,2)==513);
    CHECK(PagingPtShadowTableCount((1ull<<30)+1,2)==0);
    CHECK(PagingPtShadowTableCount((1ull<<30)+1,4)==517);
    CHECK(PagingPtShadowTableCount(0,4)==0);
    CHECK(PagingPtShadowTableCount((1ull<<48)+1,4)==0);
    CHECK(PagingPtShadowTableCount(1,0)==0);
    CHECK(PagingPtShadowTableCount(1,5)==0);
    CHECK(PagingPtShadowInit(&s,slots,4)==PAGING_PT_OK);
    for(i=0;i<512;i++)values[i]=0x123400000ull+i;
    CHECK(PagingPtShadowRead(&s,0,0,&v)==PAGING_PT_MISSING && v==0);
    CHECK(PagingPtShadowApply(&s,0,0,1,values,0)==PAGING_PT_MISSING && !s.Used);
    CHECK(PagingPtShadowApply(&s,0,63,2,values,1)==PAGING_PT_OK && s.Used==1);
    CHECK(PagingPtShadowRead(&s,0,62,&v)==PAGING_PT_MISSING);
    CHECK(PagingPtShadowRead(&s,0,63,&v)==PAGING_PT_OK && v==values[0]);
    CHECK(PagingPtShadowRead(&s,0,64,&v)==PAGING_PT_OK && v==values[1]);
    // Split a full-table update as the current GPU builder splits512 PTEs.
    CHECK(PagingPtShadowApply(&s,0,0,480,values,0)==PAGING_PT_OK);
    CHECK(PagingPtShadowRead(&s,0,480,&v)==PAGING_PT_MISSING);
    CHECK(PagingPtShadowApply(&s,0,480,32,values+480,0)==PAGING_PT_OK);
    for(i=0;i<512;i++)CHECK(PagingPtShadowRead(&s,0,i,&v)==PAGING_PT_OK && v==values[i]);
    // Explicit invalidation is a known zero, not an absent table entry.
    v=0;CHECK(PagingPtShadowApply(&s,0,64,1,&v,0)==PAGING_PT_OK);
    CHECK(PagingPtShadowRead(&s,0,64,&v)==PAGING_PT_OK && v==0);
    // All four physical pages hash to one bucket; collisions must retain identity.
    for(i=1;i<4;i++)CHECK(PagingPtShadowApply(&s,(PAGING_PT_U64)i*16384,0,1,values+i,1)==PAGING_PT_OK);
    for(i=1;i<4;i++)CHECK(PagingPtShadowRead(&s,(PAGING_PT_U64)i*16384,0,&v)==PAGING_PT_OK && v==values[i]);
    memcpy(before,slots,sizeof(slots));
    CHECK(PagingPtShadowCanApply(&s,0,0,1,0)==PAGING_PT_OK);
    CHECK(PagingPtShadowCanApply(&s,65536,0,1,1)==PAGING_PT_FULL);
    CHECK(PagingPtShadowCanApply(&s,65536,0,1,0)==PAGING_PT_MISSING);
    CHECK(PagingPtShadowCanApply(&s,0,512,1,0)==PAGING_PT_INVALID);
    CHECK(PagingPtShadowApply(&s,65536,0,1,values,1)==PAGING_PT_FULL);
    CHECK(PagingPtShadowApply(&s,65536,0,1,values,0)==PAGING_PT_MISSING);
    CHECK(PagingPtShadowApply(&s,0,511,2,values,0)==PAGING_PT_INVALID);
    CHECK(PagingPtShadowApply(&s,1,0,1,values,0)==PAGING_PT_INVALID);
    CHECK(PagingPtShadowApply(&s,1ull<<48,0,1,values,1)==PAGING_PT_INVALID);
    CHECK(PagingPtShadowApply(&s,0,0,0,values,0)==PAGING_PT_INVALID);
    CHECK(PagingPtShadowApply(&s,0,0,1,NULL,0)==PAGING_PT_INVALID);
    CHECK(memcmp(before,slots,sizeof(slots))==0 && s.Used==4);
    // Existing tables remain writable even when registration storage is full.
    CHECK(PagingPtShadowApply(&s,0,0,1,values+7,0)==PAGING_PT_OK);
    CHECK(PagingPtShadowRead(&s,0,0,&v)==PAGING_PT_OK && v==values[7]);
    CHECK(PagingPtShadowInit(&s,slots,4)==PAGING_PT_OK && !s.Used);
    CHECK(PagingPtShadowRead(&s,0,63,&v)==PAGING_PT_MISSING);
    CHECK(PagingPtShadowApply(&s,0xfffffffff000ull,511,1,values,1)==PAGING_PT_OK);
    CHECK(PagingPtShadowRead(&s,0xfffffffff000ull,511,&v)==PAGING_PT_OK && v==values[0]);
    test_copy();
    printf("%u checks, %u failures: %s\n",checks,failures,failures?"FAIL":"PASS");
    return failures?1:0;
}
