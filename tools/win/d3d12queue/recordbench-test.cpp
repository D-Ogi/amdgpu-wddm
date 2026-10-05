// SPDX-License-Identifier: MIT
// Pure checks of the record bench oracle (recordbench-oracle.h) and its programs: no adapter, no D3D calls. build.ps1
// compiles it with the same INTERACTIVE_RECORDBENCH_* values as the client it builds.
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <set>
#include "recordbench-oracle.h"
#include "recordbench-programs.h"

int main(){
    using namespace recordbench;
    // Fixed seeds keep the sample deterministic; a run draws its seed from BCryptGenRandom.
    const UINT64 seed=0x0123456789ABCDEFull,other=0xFEDCBA9876543210ull;
    static_assert(slot_bytes==128 && slot_words==32,"slot layout of recordbench.hlsl");
    static_assert(ring_bytes==UINT64{draws}*256 && slots_bytes==UINT64{draws}*128,"buffer sizes");
    static_assert(target_offset%512==0 && target_offset>=slots_bytes && target_offset<slots_bytes+512,"target placement");
    static_assert(readback_bytes==target_offset+1024,"readback size");
    // The state each draw sees: the pipeline alternates every 4 draws, the table every 8 over 4, the bindings every 16.
    assert(pipeline_of(0)==0 && pipeline_of(3)==0 && pipeline_of(4)==1 && pipeline_of(8)==0);
    assert(table_of(7)==0 && table_of(8)==1 && table_of(31)==3 && table_of(32)==0);
    assert(binding_of(15)==0 && binding_of(16)==1 && binding_of(32)==0);
    // The last draw of the last group that bound each target: the defaults' 512 draws end with group 31 on target 1.
    // The target keeps that draw's root word 1, which moves with the frame.
    for(UINT t=0;t<2;++t){
        const UINT last=last_draw(t);
        assert(last<draws && binding_of(last)==t && (last+1)%group==0);
        for(UINT d=last+1;d<draws;++d)assert(binding_of(d)!=t);
        assert(target_value(seed,burst,0,5,t)==value(seed,root_part,burst,0,5,last,1));
        assert(target_value(seed,burst,0,5,t)!=target_value(seed,burst,0,6,t));
    }
    assert(last_draw(0)!=last_draw(1) && (last_draw(0)==draws-1 || last_draw(1)==draws-1));
    // Root word 0 is the slot index the program writes to; the mark carries it with the pipeline's byte; the cross word
    // combines root word 1, CBV word 0, table word 0 and the tag.
    const std::set<UINT64> frames{0,1,2,3,1000,0xFFFFF};const std::set<UINT> sample_draws{0,1,4,8,15,16,draws/2,draws-1};
    for(unsigned p=0;p<phase_count;++p)for(unsigned l=0;l<lists;++l)for(const UINT64 f:frames)for(const UINT d:sample_draws){
        assert(slot_value(seed,p,l,f,d,0)==d);
        for(UINT i=1;i<root_words;++i)assert(slot_value(seed,p,l,f,d,i)==value(seed,root_part,p,l,f,d,i));
        for(UINT i=0;i<cbv_words;++i)assert(slot_value(seed,p,l,f,d,root_words+i)==value(seed,cbv_part,p,l,f,d,i));
        for(UINT i=0;i<table_words;++i)assert(slot_value(seed,p,l,f,d,root_words+cbv_words+i)==table_value(seed,table_of(d),i));
        assert(slot_value(seed,p,l,f,d,28)==(d^((pipeline_of(d)?0x5Au:0xA5u)<<24)));
        assert(slot_value(seed,p,l,f,d,29)==tags[binding_of(d)]);
        assert(slot_value(seed,p,l,f,d,30)==(value(seed,root_part,p,l,f,d,1)^value(seed,cbv_part,p,l,f,d,0)^
                                             table_value(seed,table_of(d),0)^tags[binding_of(d)]));
        assert(slot_value(seed,p,l,f,d,31)==seal);
    }
    // Every root and CBV word apart from the slot index is distinct over phases, lists, frames, draws, parts and
    // words, and depends on the seed: a stale, misplaced or foreign word never passes the comparison. The sample is
    // small enough (about 1700 words) that a chance collision of the 32-bit truncation stays below 1 in 2000.
    std::set<UINT32> seen;size_t words=0,moved=0;
    const std::set<unsigned> sample_lists{0,lists-1};const std::set<UINT64> few_frames{0,1,0xFFFFF};
    const std::set<UINT> few_draws{0,1,draws/2,draws-1};
    for(unsigned p=0;p<phase_count;++p)for(const unsigned l:sample_lists)for(const UINT64 f:few_frames)for(const UINT d:few_draws)
        for(unsigned part=root_part;part<=cbv_part;++part)for(UINT w=part==root_part?1u:0u;w<(part==root_part?root_words:cbv_words);++w){
            const UINT32 v=value(seed,part,p,l,f,d,w);
            ++words;assert(seen.insert(v).second);
            if(value(other,part,p,l,f,d,w)!=v)++moved;
        }
    assert(moved==words);
    for(UINT t=0;t<tables;++t)for(UINT w=0;w<table_words;++w)assert(seen.insert(table_value(seed,t,w)).second);
    // A root word of the frame before is traced as such; one two frames back, or of another phase, is not found.
    const unsigned l=lists-1;const UINT d=draws-3;
    const Source late=identify(seed,value(seed,root_part,interleaved,l,41,d,5),interleaved,42);
    assert(late.found && late.part==root_part && late.list==l && late.frame==41 && late.draw==d && late.word==5);
    const Source cbv=identify(seed,value(seed,cbv_part,interleaved,0,42,7,15),interleaved,42);
    assert(cbv.found && cbv.part==cbv_part && cbv.list==0 && cbv.frame==42 && cbv.draw==7 && cbv.word==15);
    assert(!identify(seed,value(seed,root_part,interleaved,l,40,d,5),interleaved,42).found);
    assert(!identify(seed,value(seed,root_part,burst,l,42,d,5),interleaved,42).found);
    const Source table=identify(seed,table_value(seed,2,3),burst,0);
    assert(table.found && table.part==table_part && table.draw==2 && table.word==3);
    // The three programs are DXBC containers.
    assert(std::memcmp(g_recordbench_vs,"DXBC",4)==0 && std::memcmp(g_recordbench_ps_a,"DXBC",4)==0 &&
           std::memcmp(g_recordbench_ps_b,"DXBC",4)==0);
    std::printf("PASS record bench oracle: lists %u, draws %u, executes %u, work %u us per group, %zu distinct seeded "
        "words, targets end at draws %u and %u, sources traced within 2 frames; no GPU calls\n",lists,draws,executes,
        work_us,words,last_draw(0),last_draw(1));
}
