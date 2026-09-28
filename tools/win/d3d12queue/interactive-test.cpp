// SPDX-License-Identifier: MIT
#include "interactive.h"
#include <cassert>
#include <cstdio>
int main(){
    using namespace interactive;
    assert(adapter_mode("--interactive")==AdapterMode::Bc250);
    assert(adapter_mode("--interactive-warp")==AdapterMode::Warp);
    const char* invalid[]={nullptr,"","--warp","--interactive-WARP","--interactive-warp extra"};
    for(const auto text:invalid)
        assert(adapter_mode(text)==AdapterMode::Invalid);
    // Reject invalid mode/deadline before any filesystem or graphics work.
    assert(run(nullptr,20,AdapterMode::Warp)==2);
    assert(run("",20,AdapterMode::Warp)==2);
    assert(run("unused",0,AdapterMode::Warp)==2);
    assert(run("unused",151,AdapterMode::Warp)==2);
    assert(run("unused",20,AdapterMode::Invalid)==2);
    Session defaults;assert(defaults.mode==AdapterMode::Bc250);
    std::puts("PASS interactive adapter selection: explicit WARP, preserved BC-250 default, invalid CLI rejected; no GPU calls");
}
