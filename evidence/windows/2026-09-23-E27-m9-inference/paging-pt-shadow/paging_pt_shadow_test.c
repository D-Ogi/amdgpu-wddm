#include <stdio.h>
#include <string.h>
#include "paging_pt_shadow.h"
static PAGING_PT_SHADOW_SLOT slots[4], before[4];
static unsigned checks,failures;
#define CHECK(x) do {checks++;if(!(x)){failures++;printf("FAIL line %u: %s\n",(unsigned)__LINE__,#x);}} while(0)
int main(void)
{
    PAGING_PT_SHADOW s={0};PAGING_PT_U64 values[512],v=123;unsigned i;
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
    printf("%u checks, %u failures: %s\n",checks,failures,failures?"FAIL":"PASS");
    return failures?1:0;
}
