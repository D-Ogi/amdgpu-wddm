// SPDX-License-Identifier: MIT
// Build parameters and the value oracle of the record bench variant (interactive-recordbench.h), without D3D, so that
// recordbench-test.cpp checks them on their own. Needs the integer types of <windows.h> and includes nothing itself,
// because interactive.h includes it inside its namespace.
#pragma once

namespace recordbench {
#ifndef INTERACTIVE_RECORDBENCH_LISTS
#define INTERACTIVE_RECORDBENCH_LISTS 4
#endif
#ifndef INTERACTIVE_RECORDBENCH_DRAWS
#define INTERACTIVE_RECORDBENCH_DRAWS 512
#endif
#ifndef INTERACTIVE_RECORDBENCH_EXECUTES
#define INTERACTIVE_RECORDBENCH_EXECUTES 2
#endif
// Stand-in application work after each group of draws in the interleaved and threaded phases, in microseconds: 64 us
// per 16 draws puts about 8 ms of it into a frame of the defaults, against the engine's recording of about the same
// order, the proportion of Witcher 3's main thread at LOW (session 246: 6.5 ms of command-list calls in 21.7 ms).
#ifndef INTERACTIVE_RECORDBENCH_WORK_US
#define INTERACTIVE_RECORDBENCH_WORK_US 64
#endif
// The lab runner's copy command arrives about 17 s after the client starts, with a 70 s client deadline and a 65 s
// Drive (resetchurn-oracle.h): 20 s of phases leave the status and exit round trips inside the Drive.
#ifndef INTERACTIVE_RECORDBENCH_SECONDS
#define INTERACTIVE_RECORDBENCH_SECONDS 20
#endif
constexpr unsigned lists=INTERACTIVE_RECORDBENCH_LISTS;
constexpr UINT draws=INTERACTIVE_RECORDBENCH_DRAWS;
constexpr unsigned executes=INTERACTIVE_RECORDBENCH_EXECUTES;
constexpr UINT work_us=INTERACTIVE_RECORDBENCH_WORK_US;
constexpr ULONGLONG budget_ms=INTERACTIVE_RECORDBENCH_SECONDS*1000ull;
static_assert(lists>=1 && lists<=8,"INTERACTIVE_RECORDBENCH_LISTS: 1 to 8");
static_assert(draws>=64 && draws<=4096 && draws%16==0,"INTERACTIVE_RECORDBENCH_DRAWS: 64 to 4096, a multiple of 16");
static_assert(executes>=1 && executes<=lists,"INTERACTIVE_RECORDBENCH_EXECUTES: 1 to LISTS");
static_assert(work_us<=1000,"INTERACTIVE_RECORDBENCH_WORK_US: 0 to 1000");
static_assert(INTERACTIVE_RECORDBENCH_SECONDS>=3 && INTERACTIVE_RECORDBENCH_SECONDS<=140,"INTERACTIVE_RECORDBENCH_SECONDS: 3 to 140");

// Every group of 16 draws binds its render target, viewport, scissor and vertex buffer (two of each, in turn); the
// pipeline alternates every 4 draws and the descriptor table moves every 8 over 4 tables; even draws are indexed.
constexpr UINT group=16,pipeline_every=4,table_every=8,tables=4;
// Layout shared with recordbench.hlsl: 8 root constants (b0), 16 words of root CBV (b1), 4 words of table CBV (b2),
// 32-word slots (u1). CBVs sit at the D3D12 placement alignment of 256 bytes; each 1x1 target is copied into the
// readback buffer at the texture placement alignment of 512 bytes after the slots.
constexpr UINT root_words=8,cbv_words=16,table_words=4,slot_words=32,slot_bytes=slot_words*4;
constexpr UINT64 cbv_stride=256,target_stride=512;
constexpr UINT64 ring_bytes=UINT64{draws}*cbv_stride,slots_bytes=UINT64{draws}*slot_bytes;
constexpr UINT64 target_offset=(slots_bytes+target_stride-1)/target_stride*target_stride;
constexpr UINT64 readback_bytes=target_offset+2*target_stride;
constexpr UINT mark_a=0xA5,mark_b=0x5A;
constexpr UINT32 tags[2]={0x7A600001u,0x7A600002u},seal=0x600DF00Du;
// Frames of a phase before its counted ones: pipeline compilation, allocator and ring growth.
constexpr unsigned warmup=16;
constexpr ULONGLONG margin_ms=10000,fence_ms=5000,record_ms=10000,join_ms=10000;
constexpr unsigned report_limit=16;
enum Phase : unsigned {burst,interleaved,threaded,phase_count};
inline const char* phase_name(unsigned phase){
    return phase==burst?"burst":phase==interleaved?"interleaved":phase==threaded?"threaded":"invalid";
}
enum Part : unsigned {root_part,cbv_part,table_part};
static_assert(slot_bytes==128,"recordbench.hlsl addresses slots by 128 bytes");
static_assert(root_words+cbv_words+table_words+4==slot_words,"slot: root, cbv, table, mark, tag, cross, seal");
static_assert(lists<=8 && draws<=(1u<<12) && slot_words<=32,"value key fields");

inline UINT pipeline_of(UINT draw){return (draw/pipeline_every)%2;}
inline UINT table_of(UINT draw){return (draw/table_every)%tables;}
inline UINT binding_of(UINT draw){return (draw/group)%2;}           // render target and vertex buffer

inline UINT64 mix(UINT64 z){
    z=(z^(z>>30))*0xBF58476D1CE4E5B9ull;z=(z^(z>>27))*0x94D049BB133111EBull;return z^(z>>31);
}
// Word 'word' of a part of one draw. The key is injective over part (2 bits), phase (2), list (3), frame (20, the
// frame number modulo 2^20), draw (12) and word (5), and mix is a bijection, so values differ up to collisions of the
// 32-bit truncation. Root word 0 is the draw's slot index instead: the program addresses its slot with it.
inline UINT32 value(UINT64 seed,unsigned part,unsigned phase,unsigned list,UINT64 frame,UINT draw,UINT word){
    if(part==root_part && !word)return draw;
    const UINT64 key=(((((UINT64{part}<<2|phase)<<3|list)<<20|(frame&0xFFFFF))<<12|draw)<<5)|word;
    return static_cast<UINT32>(mix(seed+key*0x9E3779B97F4A7C15ull));
}
// The descriptor tables' CBVs, written once at setup.
inline UINT32 table_value(UINT64 seed,UINT table,UINT word){return value(seed,table_part,0,0,0,table,word);}
inline UINT32 slot_value(UINT64 seed,unsigned phase,unsigned list,UINT64 frame,UINT draw,UINT index){
    if(index<root_words)return value(seed,root_part,phase,list,frame,draw,index);
    if(index<root_words+cbv_words)return value(seed,cbv_part,phase,list,frame,draw,index-root_words);
    if(index<root_words+cbv_words+table_words)return table_value(seed,table_of(draw),index-root_words-cbv_words);
    const UINT32 tag=tags[binding_of(draw)];
    switch(index-root_words-cbv_words-table_words){
    case 0:return draw^((pipeline_of(draw)?mark_b:mark_a)<<24);
    case 1:return tag;
    case 2:return value(seed,root_part,phase,list,frame,draw,1)^value(seed,cbv_part,phase,list,frame,draw,0)^
                  table_value(seed,table_of(draw),0)^tag;
    default:return seal;
    }
}
// The last draw that renders to a 1x1 target in a list: the last draw of the last group that binds it.
inline UINT last_draw(UINT target){
    const UINT groups=draws/group,last=(groups-1)%2==target?groups-1:groups-2;
    return (last+1)*group-1;
}
// The value the target holds after the list: that draw's root word 1, which the program returns.
inline UINT32 target_value(UINT64 seed,unsigned phase,unsigned list,UINT64 frame,UINT target){
    return value(seed,root_part,phase,list,frame,last_draw(target),1);
}
struct Source {bool found{};unsigned part{},list{};UINT64 frame{};UINT draw{},word{};};
// Where an unexpected value came from, if this phase wrote it in the checked frame or the one before it (a call
// replayed out of order or late would leave one of those), or if it is a table word. Slot indices are small numbers
// shared by every frame and are not searched.
inline Source identify(UINT64 seed,UINT32 actual,unsigned phase,UINT64 frame){
    for(UINT64 back=0;back<2 && back<=frame;++back)
        for(unsigned l=0;l<lists;++l)for(UINT d=0;d<draws;++d)for(unsigned part=root_part;part<=cbv_part;++part)
            for(UINT w=part==root_part?1u:0u;w<(part==root_part?root_words:cbv_words);++w)
                if(value(seed,part,phase,l,frame-back,d,w)==actual)return {true,part,l,frame-back,d,w};
    for(UINT t=0;t<tables;++t)for(UINT w=0;w<table_words;++w)
        if(table_value(seed,t,w)==actual)return {true,table_part,0,0,t,w};
    return {};
}
}  // namespace recordbench
