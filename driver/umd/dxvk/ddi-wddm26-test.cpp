// SPDX-License-Identifier: MIT
// WDDM 2.6 D3D11 device table (BD-371): the layout the runtime gives a driver of a later interface. It is the
// WDDM 2.2 table with the relocation entry retyped and two scan-out entries appended. No GPU: the owner has no
// engine, and no entry under test reaches it.
#include "ddi-wddm26.h"
#include "ddi-wddm22.h"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
using namespace bc250::umd;
// The line is flushed before the abort: a redirected stdout is fully buffered.
#define CHECK(x) do { if(!(x)) { std::printf("FAIL wddm26 line %d\n",__LINE__);std::fflush(stdout);std::abort(); } } while(0)
namespace {
unsigned reported; HRESULT lastReported;
void APIENTRY count_error(D3D10DDI_HRTCORELAYER,HRESULT hr) { ++reported; lastReported=hr; }

// Every WDDM 2.2 entry stays where it was and keeps its implementation; the relocation entry changes; two
// entries are appended.
void device_table() {
    const auto t=make_wddm2_6_device_table();
    const auto b=make_wddm2_2_device_table();
    const size_t retyped=offsetof(D3DWDDM2_6DDI_DEVICEFUNCS,pfnRelocateDeviceFuncs);
    void *slots[sizeof(t)/sizeof(void *)]; std::memcpy(slots,&t,sizeof(t));
    void *baseSlots[sizeof(b)/sizeof(void *)]; std::memcpy(baseSlots,&b,sizeof(b));
    unsigned changed=0,appended=0;
    for (size_t i=0;i<sizeof(t)/sizeof(void *);++i) {
        if (i>=sizeof(b)/sizeof(void *)) { CHECK(slots[i]); ++appended; }
        else if (i*sizeof(void *)==retyped) { CHECK(slots[i] && slots[i]!=baseSlots[i]); ++changed; }
        else CHECK(slots[i]==baseSlots[i]);
    }
    CHECK(changed==1 && appended==2);
    static_assert(offsetof(D3DWDDM2_6DDI_DEVICEFUNCS,pfnQueryScanoutCaps)==sizeof(D3DWDDM2_2DDI_DEVICEFUNCS) &&
        offsetof(D3DWDDM2_6DDI_DEVICEFUNCS,pfnPrepareScanoutTransformation)+sizeof(void *)==sizeof(D3DWDDM2_6DDI_DEVICEFUNCS));

    DeviceOwner owner; owner.runtime().UMCallbacks.pfnSetErrorCb=count_error;
    DdiDeviceHandle storage{&owner}; D3D10DDI_HDEVICE h{}; h.pDrvPrivate=&storage;
    // The relocation rebuilds this table, not the WDDM 2.2 one.
    D3DWDDM2_6DDI_DEVICEFUNCS moved{}; std::memset(&moved,0xA5,sizeof(moved));
    reported=0; lastReported=S_OK;
    t.pfnRelocateDeviceFuncs(h,&moved);
    CHECK(!std::memcmp(&moved,&t,sizeof(t)) && !owner.runtime().domain.entered() && !reported);
    // A device that is not initialized yet: the relocation writes nothing.
    DdiDeviceHandle empty{}; D3D10DDI_HDEVICE none{}; none.pDrvPrivate=&empty;
    std::memset(&moved,0xA5,sizeof(moved));
    t.pfnRelocateDeviceFuncs(none,&moved);
    CHECK(reinterpret_cast<const unsigned char *>(&moved)[0]==0xA5);

    // QueryScanoutCaps answers NONE for every resource, source and plane, and reports nothing. A null
    // output is not written.
    static_assert(kScanoutFlags==D3DWDDM2_6DDI_SCANOUT_FLAG_NONE);
    for (UINT source:{0u,1u,7u}) for (UINT plane:{0u,3u}) {
        auto flags=static_cast<D3DWDDM2_6DDI_SCANOUT_FLAGS>(0x7F);
        t.pfnQueryScanoutCaps(h,{},0,source,plane,&flags);
        CHECK(flags==D3DWDDM2_6DDI_SCANOUT_FLAG_NONE);
    }
    t.pfnQueryScanoutCaps(h,{},0,0,0,nullptr);
    CHECK(!reported && !owner.runtime().domain.entered());

    // NONE gives the runtime no reason to call PrepareScanoutTransformation. A call is a broken contract, so the
    // entry loses the device, which is the only status a void entry may report.
    D3D10_DDI_RECT rect{0,0,16,16};
    t.pfnPrepareScanoutTransformation(h,{},0,0,0,&rect);
    CHECK(reported==1 && lastReported==D3DDDIERR_DEVICEREMOVED && !owner.runtime().domain.entered());
    t.pfnPrepareScanoutTransformation(h,{},0,0,0,nullptr);
    CHECK(reported==2);
}
}
void test_wddm2_6_ddi() {
    device_table();
    std::cout << "PASS WDDM 2.6 device table: WDDM 2.2 entries kept, relocation, QueryScanoutCaps answers NONE, "
                 "PrepareScanoutTransformation unexpected (no GPU)\n";
}
