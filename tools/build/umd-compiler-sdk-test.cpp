// Cross-compiler SDK layout and x86 stdcall control. No runtime, GPU or installation.
#include <windows.h>
#include <d3d9types.h>
#include <d3dumddi.h>
#include <d3d12umddi.h>
#include <d3dkmthk.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>

struct NaturalPacking { char byte; uint64_t wide; };
static void sdk_layout(ULONG* out) {
    const ULONG values[]={sizeof(void*),sizeof(NaturalPacking),offsetof(NaturalPacking,wide),
        sizeof(D3DDDI_ALLOCATIONINFO2),offsetof(D3DDDI_ALLOCATIONINFO2,hAllocation),
        offsetof(D3DDDI_ALLOCATIONINFO2,Flags),sizeof(D3DDDICB_ALLOCATE),
        offsetof(D3DDDICB_ALLOCATE,pPrivateDriverData),offsetof(D3DDDICB_ALLOCATE,pAllocationInfo2),
        sizeof(D3D12DDIARG_CREATEHEAP_0001),alignof(D3D12DDIARG_CREATEHEAP_0001),
        sizeof(D3DKMT_LOCK2),offsetof(D3DKMT_LOCK2,pData)};
    for(unsigned i=0;i<sizeof(values)/sizeof(values[0]);++i)out[i]=values[i];
}
extern "C" uint64_t __stdcall bc250_compiler_probe(ULONG seed,uint64_t salt,ULONG* layout);
#if defined(BC250_COMPILER_CALLER)
int main() {
    ULONG expected[13]{},actual[13]{};sdk_layout(expected);
    // A 64-bit argument and result cross the MSVC/Clang stdcall boundary repeatedly.
    for(ULONG n=0;n<4096;++n) {
        const uint64_t salt=0xfedcba9876543210ull+n;
        if(bc250_compiler_probe(n,salt,actual)!=(salt^(uint64_t(n)<<33)))return 1;
        for(unsigned i=0;i<13;++i)if(actual[i]!=expected[i]) {
            std::printf("SDK layout mismatch %u: clang %lu, MSVC %lu\n",i,actual[i],expected[i]);return 2;
        }
    }
    std::printf("PASS: 13 SDK layouts and 4096 MSVC/Clang calls (%u-bit)\n",unsigned(sizeof(void*)*8));
}
#else
extern "C" uint64_t __stdcall bc250_compiler_probe(ULONG seed,uint64_t salt,ULONG* layout) {
    sdk_layout(layout);return salt^(uint64_t(seed)<<33);
}
extern "C" void bc250_compiler_prefetch(const char* p) {
    _mm_prefetch(p,_MM_HINT_T0);_mm_prefetch(p,_MM_HINT_T1);_mm_prefetch(p,_MM_HINT_T2);
}
#endif
