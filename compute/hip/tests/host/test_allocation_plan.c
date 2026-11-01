#include <stdio.h>
#include "../../bc250hsa/allocation_plan.h"
static unsigned checks, failures;
#define CHECK(x) do {++checks; if (!(x)) {++failures; printf("FAIL line %d: %s\n",__LINE__,#x);}} while(0)
int main(void)
{
    static const uint64_t sizes[] = {1u, 0xfffc0000ull, 0xfffc0001ull, 0x100000000ull,
        6960120064ull, 11486101504ull, 0x500000001ull};
    static const uint64_t aligns[] = {0u, 4096u, 65536u, 0x40000u, 0x200000u, 0x200000000ull};
    const uint64_t lo=0x1000000000ull, hi=0x2fffffffffull;
    bc250hsa_allocation_plan p;
    uint64_t offset, bytes, base;
    unsigned s,a; uint32_t i;
    for(s=0;s<sizeof(sizes)/sizeof(sizes[0]);++s) {
        for(a=0;a<sizeof(aligns)/sizeof(aligns[0]);++a) {
            uint64_t covered=0;
            CHECK(bc250hsa_plan_allocation(sizes[s],aligns[a],lo,hi,BC250HSA_ALLOCATION_CHILD_MAX,&p));
            CHECK(p.requested==sizes[s] && p.bytes>=sizes[s] && p.bytes-sizes[s]<0x40000u);
            for(i=0;i<p.child_count;++i) {
                CHECK(bc250hsa_plan_child(&p,i,&offset,&bytes));
                CHECK(offset==covered && bytes>0u && bytes<=0xfffc0000ull && bytes%0x40000u==0u);
                covered+=bytes;
            }
            CHECK(covered==p.bytes && !bc250hsa_plan_child(&p,p.child_count,&offset,&bytes));
            CHECK(bc250hsa_plan_base(&p,lo+0x10000u,lo,hi,&base));
            CHECK(base%p.alignment==0u && base>=lo+0x10000u);
            CHECK(base+p.bytes<=lo+0x10000u+p.reservation_bytes);
            CHECK(!bc250hsa_plan_base(&p,lo+1u,lo,hi,&base));
            CHECK(!bc250hsa_plan_base(&p,hi&~0xffffull,lo,hi,&base));
        }
    }
    CHECK(!bc250hsa_plan_allocation(UINT64_MAX,0u,lo,hi,BC250HSA_ALLOCATION_CHILD_MAX,&p));
    CHECK(!bc250hsa_plan_allocation(1u,UINT64_C(1)<<63,lo,hi,BC250HSA_ALLOCATION_CHILD_MAX,&p));
    CHECK(!bc250hsa_plan_allocation(1u,3u,lo,hi,BC250HSA_ALLOCATION_CHILD_MAX,&p));
    CHECK(!bc250hsa_plan_allocation(0u,0u,lo,hi,BC250HSA_ALLOCATION_CHILD_MAX,&p));
    CHECK(!bc250hsa_plan_allocation(1u,0u,lo,hi,0xffff0000u,&p));
    CHECK(!bc250hsa_plan_allocation(1u,0u,lo,lo+0x3fffeu,BC250HSA_ALLOCATION_CHILD_MAX,&p));
    CHECK(bc250hsa_plan_allocation(0x100001u,0u,lo,hi,0x40000u,&p) && p.child_count==5u);
    CHECK(bc250hsa_plan_child(&p,4u,&offset,&bytes) && offset==0x100000u && bytes==0x40000u);
    CHECK(!bc250hsa_plan_allocation(1u,0u,UINT64_MAX,UINT64_MAX,0x40000u,&p));
    CHECK(!bc250hsa_plan_allocation(UINT64_MAX-0x3ffffu,0x8000000000000000ull,0u,UINT64_MAX,0x40000u,&p));
    printf("allocation plan: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
