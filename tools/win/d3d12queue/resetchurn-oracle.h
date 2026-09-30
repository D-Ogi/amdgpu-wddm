// SPDX-License-Identifier: MIT
// Build parameters and the value oracle of the reset churn variant (interactive-resetchurn.h), without D3D, so that
// resetchurn-test.cpp checks them on their own. Needs the integer types of <windows.h> and includes nothing itself,
// because interactive.h includes it inside its namespace.
#pragma once

namespace resetchurn {
#ifndef INTERACTIVE_RESETCHURN_THREADS
#define INTERACTIVE_RESETCHURN_THREADS 4
#endif
#ifndef INTERACTIVE_RESETCHURN_BATCHES
#define INTERACTIVE_RESETCHURN_BATCHES 2000
#endif
// The lab runner starts the client with a 70 s deadline and drives it for 65 s from about 10 s before the client
// starts; the copy command arrives about 17 s in (176). 20 s of batches leave the status and exit round trips
// inside the Drive. A longer run needs a longer client deadline and Drive first.
#ifndef INTERACTIVE_RESETCHURN_SECONDS
#define INTERACTIVE_RESETCHURN_SECONDS 20
#endif
#ifndef INTERACTIVE_RESETCHURN_DRAWS
#define INTERACTIVE_RESETCHURN_DRAWS 1024
#endif
#ifndef INTERACTIVE_RESETCHURN_RENEW
#define INTERACTIVE_RESETCHURN_RENEW 2
#endif
constexpr unsigned threads=INTERACTIVE_RESETCHURN_THREADS;
constexpr UINT64 planned=INTERACTIVE_RESETCHURN_BATCHES;
constexpr ULONGLONG budget_ms=INTERACTIVE_RESETCHURN_SECONDS*1000ull;
constexpr UINT draws=INTERACTIVE_RESETCHURN_DRAWS;
constexpr unsigned renew=INTERACTIVE_RESETCHURN_RENEW;
static_assert(threads>=1 && threads<=8,"INTERACTIVE_RESETCHURN_THREADS: 1 to 8");
static_assert(planned>=1 && planned<=100000,"INTERACTIVE_RESETCHURN_BATCHES: 1 to 100000");
static_assert(INTERACTIVE_RESETCHURN_SECONDS>=1 && INTERACTIVE_RESETCHURN_SECONDS<=140,"INTERACTIVE_RESETCHURN_SECONDS: 1 to 140");
static_assert(draws>=16 && draws<=4096,"INTERACTIVE_RESETCHURN_DRAWS: 16 to 4096");
static_assert(renew<=64,"INTERACTIVE_RESETCHURN_RENEW: 0 (never) to 64");
// Layout shared with resetchurn.hlsl: 32 root constants (b0), 16 words of root CBV (b1), 48-word slots (u1). CBVs
// sit at the D3D12 placement alignment of 256 bytes, checked against the header in interactive-resetchurn.h.
constexpr UINT root_words=32,cbv_words=16,slot_words=root_words+cbv_words,slot_bytes=slot_words*4;
constexpr UINT64 cbv_stride=256;
constexpr UINT regions=2;
constexpr UINT64 region_bytes=draws*cbv_stride,ring_bytes=region_bytes*regions,slots_bytes=UINT64{draws}*slot_bytes;
constexpr ULONGLONG margin_ms=10000,fence_ms=5000,record_ms=10000,join_ms=10000;
constexpr unsigned report_limit=16,progress_every=50,history=4;
enum Part : unsigned {root_part,cbv_part};
static_assert(slot_bytes==192,"resetchurn.hlsl addresses slots by 192 bytes");
static_assert(threads<=8 && planned<=(1u<<17) && draws<=(1u<<12) && root_words<=64 && cbv_words<=64,"value key fields");

inline UINT64 mix(UINT64 z){
    z=(z^(z>>30))*0xBF58476D1CE4E5B9ull;z=(z^(z>>27))*0x94D049BB133111EBull;return z^(z>>31);
}
// Word 'word' of a part of one draw. The key is injective over part, thread (3 bits), batch (17), draw (12) and word
// (6), and mix is a bijection, so values differ up to collisions of the 32-bit truncation. Root word 0 is the draw's
// slot index instead: the program addresses its slot with it.
inline UINT32 value(UINT64 seed,unsigned part,unsigned thread,UINT64 batch,UINT draw,UINT word){
    if(part==root_part && !word)return draw;
    const UINT64 key=(((((UINT64{part}<<3|thread)<<17|batch)<<12|draw)<<6)|word);
    return static_cast<UINT32>(mix(seed+key*0x9E3779B97F4A7C15ull));
}
inline UINT32 slot_value(UINT64 seed,unsigned thread,UINT64 batch,UINT draw,UINT index){
    return index<root_words?value(seed,root_part,thread,batch,draw,index):value(seed,cbv_part,thread,batch,draw,index-root_words);
}
// The draw after which a thread releases the allocator it replaced: 1 to draws, so always after its own
// ResetCommandList onto the new one (a closed list keeps a pointer to its last allocator until that reset).
inline UINT release_after(UINT64 seed,unsigned thread,UINT64 batch){
    return 1+static_cast<UINT>(mix(seed^0xA24BAED4963EE407ull^(UINT64{thread}<<32)^batch)%draws);
}
struct Source {bool found{};unsigned part{},thread{};UINT64 batch{};UINT draw{},word{};};
// Where an unexpected value came from, if this run wrote it in the checked batch or the three before it. Slot
// indices are small numbers shared by every batch and are not searched.
inline Source identify(UINT64 seed,UINT32 actual,UINT64 batch){
    for(UINT64 back=0;back<history && back<=batch;++back)
        for(unsigned t=0;t<threads;++t)for(UINT d=0;d<draws;++d)for(unsigned part=root_part;part<=cbv_part;++part)
            for(UINT w=part==root_part?1u:0u;w<(part==root_part?root_words:cbv_words);++w)
                if(value(seed,part,t,batch-back,d,w)==actual)return {true,part,t,batch-back,d,w};
    return {};
}
}  // namespace resetchurn
