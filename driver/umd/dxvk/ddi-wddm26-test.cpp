// SPDX-License-Identifier: MIT
// WDDM 2.6 D3D11 device table (BD-371): the layout the runtime gives a driver of a later interface. It is the
// WDDM 2.2 table with the relocation entry retyped and two scan-out entries appended. Also the offer of the WDDM
// 2.9 interface, which takes that table. No GPU: the owner has no engine, and no entry under test reaches it.
#include "ddi-wddm26.h"
#include "ddi-wddm22.h"
#include "ddi-negotiation.h"
#include "ddi-experiment.h"
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

// The WDDM 2.9 interface, which takes this table: offered from a KMD of WDDM 2.9 or later, under both switches,
// at its own build only.
void negotiation() {
    const auto newest=[](D3DKMT_DRIVERVERSION kmt,bool wddm22=true,bool wddm29=true,
                         D3D_FEATURE_LEVEL maximum=D3D_FEATURE_LEVEL_12_1,bool known=true) {
        return newest_ddi_level({maximum,known,kmt,wddm22,wddm29});
    };
    CHECK(newest(KMT_DRIVERVERSION_WDDM_2_9)==DdiLevel::wddm2_9);
    CHECK(newest(KMT_DRIVERVERSION_WDDM_3_0)==DdiLevel::wddm2_9);
    CHECK(newest(KMT_DRIVERVERSION_WDDM_2_8)==DdiLevel::wddm2_2);
    CHECK(newest(KMT_DRIVERVERSION_WDDM_2_2)==DdiLevel::wddm2_2);
    CHECK(newest(KMT_DRIVERVERSION_WDDM_2_1)==DdiLevel::wddm2_0);
    // The switches only lower the offer: wddm29-ddi-off stops at WDDM 2.2, wddm22-ddi-off at WDDM 2.0.
    CHECK(newest(KMT_DRIVERVERSION_WDDM_2_9,true,false)==DdiLevel::wddm2_2);
    CHECK(newest(KMT_DRIVERVERSION_WDDM_2_9,false,true)==DdiLevel::wddm2_0);
    CHECK(newest(KMT_DRIVERVERSION_WDDM_2_9,false,false)==DdiLevel::wddm2_0);
    // An unknown KMD version keeps WDDM 2.0, and an FL11 adapter the D3D11.1 table alone.
    CHECK(newest(KMT_DRIVERVERSION_WDDM_2_9,true,true,D3D_FEATURE_LEVEL_12_1,false)==DdiLevel::wddm2_0);
    CHECK(newest(KMT_DRIVERVERSION_WDDM_2_9,true,true,D3D_FEATURE_LEVEL_11_1)==DdiLevel::d3d11_1);
    // Four offers, oldest first, the newest at the WDDM 2.9 interface and build.
    UINT32 count=0; UINT64 versions[5]={1,2,3,4,5};
    CHECK(supported_ddi_versions(DdiLevel::wddm2_9,&count,nullptr)==S_OK && count==4);
    count=3;
    CHECK(supported_ddi_versions(DdiLevel::wddm2_9,&count,versions)==HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) &&
          count==3 && versions[0]==1);
    count=5;
    CHECK(supported_ddi_versions(DdiLevel::wddm2_9,&count,versions)==S_OK && count==4 &&
          versions[0]==D3D11_1_DDI_SUPPORTED && versions[1]==D3DWDDM2_0_DDI_SUPPORTED &&
          versions[2]==D3DWDDM2_2_DDI_SUPPORTED && versions[3]==D3DWDDM2_9_DDI_SUPPORTED && versions[4]==5);
    static_assert(D3DWDDM2_9_DDI_INTERFACE_VERSION==0xB002A && D3DWDDM2_9_DDI_BUILD_VERSION==0);
    CHECK(!std::strcmp(ddi_version_name(D3DWDDM2_9_DDI_SUPPORTED),"WDDM2_9"));
    DdiLevel asked=DdiLevel::d3d11_1;
    CHECK(level_of(D3DWDDM2_9_DDI_INTERFACE_VERSION,UINT(D3DWDDM2_9_DDI_BUILD_VERSION)<<16,asked) &&
          asked==DdiLevel::wddm2_9);
    CHECK(!level_of(D3DWDDM2_9_DDI_INTERFACE_VERSION,UINT(D3DWDDM2_9_DDI_BUILD_VERSION+1)<<16,asked));
    // The interfaces between WDDM 2.2 and WDDM 2.9 have no row: the shell offers none of them.
    CHECK(!level_of(D3DWDDM2_6_DDI_INTERFACE_VERSION,UINT(D3DWDDM2_6_DDI_BUILD_VERSION)<<16,asked));
    CHECK(!level_of(D3DWDDM2_8_DDI_INTERFACE_VERSION,UINT(D3DWDDM2_8_DDI_BUILD_VERSION)<<16,asked));
    // The WDDM 2.9 device takes the DXGI 1.6.1 table and the whole present callback.
    CHECK(IS_DXGI1_6_1_BASE_FUNCTIONS(D3DWDDM2_9_DDI_INTERFACE_VERSION,UINT(D3DWDDM2_9_DDI_BUILD_VERSION)<<16));
    // The switch: a Feature switch, so a release build honours it, and it reads "wddm29-ddi-off".
    CHECK(wddm2_9_offered()!=d3d11_experiment_off("wddm29-ddi"));
    if (!d3d11_experiment_name()[0]) CHECK(wddm2_9_offered());
    char list[64]="wddm29-ddi-off",dropped[64]{};
    const native12::SwitchAdmission release=native12::admit_switches(list,kD3D11Switches,false,dropped);
    CHECK(release.kept==1 && !release.lab && !release.unknown && !std::strcmp(list,"wddm29-ddi-off"));
}
}
void test_wddm2_6_ddi() {
    device_table();
    negotiation();
    std::cout << "PASS WDDM 2.6 device table and WDDM 2.9 interface: WDDM 2.2 entries kept, relocation, "
                 "QueryScanoutCaps answers NONE, PrepareScanoutTransformation unexpected, offer and switch (no GPU)\n";
}
