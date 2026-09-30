// SPDX-License-Identifier: MIT
// Pure checks of the reset churn oracle (resetchurn-oracle.h) and its programs: no adapter, no D3D calls. build.ps1
// compiles it with the same INTERACTIVE_RESETCHURN_* values as the client it builds.
#include <windows.h>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <set>
#include "resetchurn-oracle.h"
#include "resetchurn-programs.h"

int main(){
    using namespace resetchurn;
    // Fixed seeds keep the sample deterministic; a run draws its seed from BCryptGenRandom.
    const UINT64 seed=0x0123456789ABCDEFull,other=0xFEDCBA9876543210ull;
    static_assert(slot_bytes==192 && slot_words==48,"slot layout of resetchurn.hlsl");
    static_assert(region_bytes==UINT64{draws}*256 && ring_bytes==2*region_bytes && slots_bytes==UINT64{draws}*192,"buffer sizes");
    // Root word 0 is the slot index the program writes to; the slot holds the root words, then the CBV words.
    const std::set<UINT64> batches{0,1,2,3,planned-1,99999};const std::set<UINT> sample_draws{0,1,2,draws/2,draws-1};
    for(unsigned t=0;t<threads;++t)for(const UINT64 b:batches)for(const UINT d:sample_draws){
        assert(value(seed,root_part,t,b,d,0)==d && slot_value(seed,t,b,d,0)==d);
        for(UINT i=1;i<slot_words;++i)
            assert(slot_value(seed,t,b,d,i)==(i<root_words?value(seed,root_part,t,b,d,i):value(seed,cbv_part,t,b,d,i-root_words)));
    }
    // Every word apart from the slot index is distinct over threads, batches, draws, parts and words, and depends on
    // the seed: a stale, misplaced or foreign word never passes the comparison.
    std::set<UINT32> seen;size_t words=0,moved=0;
    for(unsigned t=0;t<threads;++t)for(const UINT64 b:batches)for(const UINT d:sample_draws)
        for(unsigned part=root_part;part<=cbv_part;++part)for(UINT w=part==root_part?1u:0u;w<(part==root_part?root_words:cbv_words);++w){
            const UINT32 v=value(seed,part,t,b,d,w);
            ++words;assert(seen.insert(v).second);
            if(value(other,part,t,b,d,w)!=v)++moved;
        }
    assert(moved==words);
    // The replaced allocator goes after draw 1 at the earliest, after its own ResetCommandList, and never after the last draw.
    UINT low=draws,high=0;
    for(unsigned t=0;t<threads;++t)for(UINT64 b=0;b<512;++b){const UINT r=release_after(seed,t,b);assert(r>=1 && r<=draws);low=(std::min)(low,r);high=(std::max)(high,r);}
    assert(low<high);
    // A CBV word of batch 40 read while batch 42 is checked is traced as coming from batch 40; batch 38 is out of the
    // history and batch 43 was not written yet.
    const unsigned t=threads-1;const UINT d=draws-3;
    const Source from=identify(seed,value(seed,cbv_part,t,40,d,3),42);
    assert(from.found && from.part==cbv_part && from.thread==t && from.batch==40 && from.draw==d && from.word==3);
    const Source root=identify(seed,value(seed,root_part,0,41,7,31),42);
    assert(root.found && root.part==root_part && root.thread==0 && root.batch==41 && root.draw==7 && root.word==31);
    assert(!identify(seed,value(seed,cbv_part,t,38,d,3),42).found);
    assert(!identify(seed,value(seed,cbv_part,t,43,d,3),42).found);
    assert(identify(seed,value(seed,cbv_part,0,0,0,0),1).found && !identify(other,value(seed,cbv_part,0,0,0,0),1).found);
    // Both programs are DXBC containers.
    assert(std::memcmp(g_resetchurn_vs,"DXBC",4)==0 && std::memcmp(g_resetchurn_ps,"DXBC",4)==0);
    std::printf("PASS reset churn oracle: threads %u, draws %u, renew %u, %zu distinct seeded words, release points %u to %u, "
        "source traced within %u batches; no GPU calls\n",threads,draws,renew,words,low,high,history);
}
