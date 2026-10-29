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
}
