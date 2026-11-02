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
    /* The layout under a local budget: every size against budgets around each child edge. */
    {
        static const uint64_t lefts[] = {0u, 1u, BC250HSA_ALLOCATION_LOCAL_RESERVE,
            BC250HSA_ALLOCATION_LOCAL_RESERVE + 0x3ffffu, BC250HSA_ALLOCATION_LOCAL_RESERVE + 0x40000u,
            0xfffc0000ull, 0xfffc0000ull + BC250HSA_ALLOCATION_LOCAL_RESERVE,
            0xfffc0000ull + BC250HSA_ALLOCATION_LOCAL_RESERVE + 0x40000u,
            7482638336ull, 11200000000ull, 0x400000000ull, UINT64_MAX};
        bc250hsa_child_layout l[16];
        unsigned k;
        for(s=0;s<sizeof(sizes)/sizeof(sizes[0]);++s) {
            CHECK(bc250hsa_plan_allocation(sizes[s],0u,lo,hi,BC250HSA_ALLOCATION_CHILD_MAX,&p));
            for(k=0;k<sizeof(lefts)/sizeof(lefts[0]);++k) {
                uint64_t covered=0, sys=0, local=0, sys_sum=UINT64_MAX;
                uint64_t room = lefts[k] > BC250HSA_ALLOCATION_LOCAL_RESERVE
                    ? (lefts[k]-BC250HSA_ALLOCATION_LOCAL_RESERVE) & ~0x3ffffull : 0u;
                uint32_t n = bc250hsa_plan_layout(&p,lefts[k],l,p.child_count+1u,&sys_sum);
                unsigned cut = 0, seen_sys = 0;
                CHECK(n>=p.child_count && n<=p.child_count+1u);
                for(i=0;i<n;++i) {
                    CHECK(l[i].offset==covered && l[i].bytes>0u && l[i].bytes<=0xfffc0000ull);
                    CHECK(l[i].bytes%0x40000u==0u);
                    if(l[i].carve_out) { CHECK(!seen_sys); local+=l[i].bytes; }
                    else { seen_sys=1; sys+=l[i].bytes; }
                    if(l[i].bytes!=0xfffc0000ull && covered+l[i].bytes!=p.bytes) { ++cut; CHECK(l[i].carve_out); }
                    covered+=l[i].bytes;
                }
                CHECK(covered==p.bytes && sys==sys_sum && cut<=1u);
                /* the carve-out takes all the room it can: everything when it fits, else the room */
                CHECK(local==(room>=p.bytes ? p.bytes : room));
                if(n!=0u) { CHECK(bc250hsa_plan_layout(&p,lefts[k],l,n-1u,&sys_sum)==0u); }
            }
        }
        /* The PyTorch request of the BD-117 trace under budgets near the 12 GiB carve-out:
         * two whole children, a cut one, and a small rest for system memory. */
        CHECK(bc250hsa_plan_allocation(11484656026ull,0u,lo,hi,BC250HSA_ALLOCATION_CHILD_MAX,&p));
        {
            uint64_t sys=0;
            const uint64_t room=(11200000000ull-BC250HSA_ALLOCATION_LOCAL_RESERVE)&~0x3ffffull;
            CHECK(bc250hsa_plan_layout(&p,11200000000ull,l,16u,&sys)==4u);
            CHECK(l[0].carve_out && l[1].carve_out && l[2].carve_out && !l[3].carve_out);
            CHECK(l[2].bytes==room-2u*0xfffc0000ull && sys==p.bytes-room && l[3].bytes==sys);
            /* The same request on the 8 GiB carve-out leaves more than its 3733582234 non-local
             * bytes for system memory: the cut second child, then one rest of 4069261312. */
            CHECK(bc250hsa_plan_layout(&p,7482638336ull,l,16u,&sys)==3u);
            CHECK(sys==4069261312ull && l[1].carve_out && !l[2].carve_out);
        }
        CHECK(bc250hsa_plan_layout(NULL,0u,l,16u,&offset)==0u);
        CHECK(bc250hsa_plan_layout(&p,0u,NULL,16u,&offset)==0u);
        CHECK(bc250hsa_plan_layout(&p,0u,l,16u,NULL)==0u);
    }
    printf("allocation plan: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
