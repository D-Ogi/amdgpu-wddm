// SPDX-License-Identifier: MIT
// The run-time inputs of the -Flip client (interactive-flip.h), read from the environment with no window, no
// adapter and no D3D call.
#define INTERACTIVE_FLIP
#include "interactive.h"
#include <cassert>
#include <cstdio>
int main(){
    using namespace interactive;
    const char* name="AMDGPU_WDDM_D3D12_FLIP_CHAIN_TEST";
    UINT w=0,h=0;
    SetEnvironmentVariableA(name,nullptr);
    assert(!flip_chain_from(name,1920,1200,w,h) && w==1920 && h==1200);
    SetEnvironmentVariableA(name,"1280x720");
    assert(flip_chain_from(name,1920,1200,w,h) && w==1280 && h==720);
    SetEnvironmentVariableA(name,"1920x1200");
    assert(flip_chain_from(name,1920,1200,w,h) && w==1920 && h==1200);
    SetEnvironmentVariableA(name,"64x64");
    assert(flip_chain_from(name,1920,1200,w,h) && w==64 && h==64);
    // Each of these keeps the output's size: a side below 64 or above the output, a missing side, text that is not
    // "<width>x<height>", and a number too long for the buffer.
    const char* refused[]={"","x","1280","1280x","x720","1280X720","1280x720x1","63x720","1280x63","1921x1200",
        "1920x1201","-1280x720","1280 x720","12a0x720","0x0","99999999999x720","1280x720 "};
    for(const auto text:refused){
        SetEnvironmentVariableA(name,text);
        w=h=0;
        assert(!flip_chain_from(name,1920,1200,w,h) && w==1920 && h==1200);
    }
    SetEnvironmentVariableA(name,nullptr);
    std::puts("PASS flip chain size: the output's size unless a valid smaller <width>x<height> is named; no GPU calls");
    // BD-224: the present period. Absent, 0, or anything that is not a number from 0 to 2000 gives 0.
    const char* period="AMDGPU_WDDM_D3D12_FLIP_PERIOD_MS_TEST";
    SetEnvironmentVariableA(period,nullptr);
    assert(flip_period_from(period)==0);
    const struct { const char* text; UINT ms; } periods[]={{"0",0},{"200",200},{"1",1},{"2000",2000},{"2001",0},
        {"-200",0},{"200ms",0},{" 200",0},{"",0},{"99999999999999",0}};
    for(const auto& p:periods){
        SetEnvironmentVariableA(period,p.text);
        assert(flip_period_from(period)==p.ms);
    }
    SetEnvironmentVariableA(period,nullptr);
    std::puts("PASS flip present period: 0 to 2000 ms, anything else is 0; no GPU calls");
}
