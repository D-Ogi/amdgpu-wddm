// SPDX-License-Identifier: MIT
// The class of an experiment switch, and the one rule that decides which build honours a name.
//
// Each application shell reads a list of switch names. The D3D12 shell does it in ddi-trace.h, and the D3D11
// shell in ddi-experiment.h of the dxvk directory. Each shell declares every name that it reads in one table.
// Each row has a class:
//   Feature   The switch turns a feature or an optimisation off, or turns an opt-in on. The driver obeys its
//             contracts with the switch set. The application runs, possibly slower or with fewer capabilities.
//             Every build honours the name, and the control application can offer it.
//   Lab       Every other switch. It turns off a protection, it starts a path that is known to fault, or it
//             changes a behaviour that ships without a switch. With such a switch set the driver can lose the
//             device, hang the GPU or stop Windows. Only a lab build honours the name. A release build drops
//             the name from the list, whichever source named it. The control application does not offer it.
//   Accepted  An old name of a behaviour that is the default now. Every build accepts it. It changes nothing.
// A name that is in no row is dropped in every build. A new switch thus does nothing until its row exists, and
// the person who adds the row must choose the class. When the class is in doubt, the class is Lab.
//
// Why the rule exists: import-residency-release-on was a switch of the release shell and of the control
// application. With it set, 3DMark Time Spy caused a GPU fault and Windows stopped with 0x116.
//
// A bisect of a protection thus needs the lab flavour of the same release. A bisect of a feature works on the
// release flavour.
#pragma once
#include <cstddef>
#include <cstring>

namespace native12 {
enum class SwitchClass : unsigned char {Feature,Lab,Accepted};
struct SwitchRow {
    const char* name;
    SwitchClass cls;
    // The name is the start of a token that carries a value, as "scanout-flip-" in "scanout-flip-1920x1200".
    bool parameter{};
};
// Whether this binary is a lab build (build-umd-d3d12.ps1 and build-umd-dxvk.ps1, -Flavour lab).
inline constexpr bool kLabBuild=
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
    true;
#else
    false;
#endif
// The row of one token, or nullptr. A row with the whole name wins over a row with a parameter.
template<size_t N>
inline const SwitchRow* find_switch(const SwitchRow (&table)[N],const char* token,size_t size) noexcept {
    if(!token || !size)return nullptr;
    for(const SwitchRow& row:table)
        if(!row.parameter && std::strlen(row.name)==size && !std::memcmp(row.name,token,size))return &row;
    for(const SwitchRow& row:table){
        const size_t length=std::strlen(row.name);
        if(row.parameter && size>length && !std::memcmp(row.name,token,length))return &row;
    }
    return nullptr;
}
struct SwitchAdmission {
    unsigned kept{};      // tokens that stay in the list
    unsigned lab{};       // Lab tokens that a release build dropped
    unsigned unknown{};   // tokens that are in no row
};
// Rewrites list in place to the tokens that this build honours, in their order, and counts the others. dropped
// gets the tokens that did not stay, separated by commas. It needs the capacity of list, or it is nullptr.
// lab_build is a parameter so that one test binary can check both flavours.
template<size_t N>
inline SwitchAdmission admit_switches(char* list,const SwitchRow (&table)[N],bool lab_build,char* dropped) noexcept {
    SwitchAdmission out{};
    if(dropped)dropped[0]=0;
    if(!list)return out;
    char* write=list;
    char* drop=dropped;
    for(const char* given=list;*given;){
        const char* end=std::strchr(given,',');
        const size_t size=end?static_cast<size_t>(end-given):std::strlen(given);
        const char* next=given+size+(end?1:0);
        if(size){
            const SwitchRow* row=find_switch(table,given,size);
            if(row && (lab_build || row->cls!=SwitchClass::Lab)){
                if(write!=list)*write++=',';
                std::memmove(write,given,size);
                write+=size;
                ++out.kept;
            } else {
                if(row)++out.lab; else ++out.unknown;
                if(drop){
                    if(drop!=dropped)*drop++=',';
                    std::memcpy(drop,given,size);
                    drop+=size;
                    *drop=0;
                }
            }
        }
        given=next;
    }
    *write=0;
    return out;
}
// Whether a row of the table has this whole name, for the tests and for a reader that needs the class.
template<size_t N>
inline const SwitchRow* switch_row(const SwitchRow (&table)[N],const char* name) noexcept {
    return name?find_switch(table,name,std::strlen(name)):nullptr;
}
}
