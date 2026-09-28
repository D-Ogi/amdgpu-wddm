#include "selection.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(v) do {if(!(v)){std::printf("FAIL %d\n",__LINE__);std::abort();}}while(0)
int main(){
    using namespace m14_probe;
    CHECK(select(client,L"1",100,100));
    CHECK(select(LR"(c:\bc250\m14\runtime001\D3D11BENCH.EXE)",L"1",600000099,100));
    CHECK(!select(client,L"1",600000100,100));
    CHECK(!select(client,L"1",99,100));
    CHECK(!select(client,L"0",100,100));
    CHECK(!select(client,L"11",100,100));
    CHECK(!select(client,L"",100,100));
    CHECK(!select(client,nullptr,100,100));
    CHECK(!select(nullptr,L"1",100,100));
    CHECK(!select(L"d3d11bench.exe",L"1",100,100));
    CHECK(!select(LR"(C:\Windows\System32\dwm.exe)",L"1",100,100));
    CHECK(!select(LR"(C:\BC250\m14\other\d3d11bench.exe)",L"1",100,100));
    std::puts("PASS M14 exact process, explicit flag, expiry boundary and future timestamp");
}
