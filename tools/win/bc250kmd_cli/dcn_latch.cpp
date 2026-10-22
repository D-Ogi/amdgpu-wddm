// One mutating lab experiment, never an automatic retry or a presentation API.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <dxgi1_2.h>
#include <d3dkmthk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "../../../driver/kmd/dcn_latch_escape.h"

static bool number(const char* text, unsigned long* result)
{
    if (text[0]<'0' || text[0]>'9') return false;
    char* end=nullptr;
    errno=0;
    unsigned long value=strtoul(text,&end,0);
    if (errno || *end) return false;
    *result=value;
    return true;
}
static int error(const char* stage, unsigned long code)
{
    fprintf(stderr,"{\"type\":\"error\",\"stage\":\"%s\",\"code\":%lu}\n",stage,code);
    return 1;
}
static bool select_adapter(LUID* luid)
{
    IDXGIFactory1* factory=nullptr;
    HRESULT hr=CreateDXGIFactory1(__uuidof(IDXGIFactory1),reinterpret_cast<void**>(&factory));
    if (FAILED(hr)) { error("factory",(unsigned long)hr); return false; }
    unsigned found=0;
    for (UINT index=0;;++index) {
        IDXGIAdapter1* adapter=nullptr;
        hr=factory->EnumAdapters1(index,&adapter);
        if (hr==DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) { factory->Release(); error("enumeration",(unsigned long)hr); return false; }
        DXGI_ADAPTER_DESC1 desc={};
        hr=adapter->GetDesc1(&desc);
        adapter->Release();
        if (FAILED(hr)) { factory->Release(); error("description",(unsigned long)hr); return false; }
        if (!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) && desc.VendorId==0x1002 && desc.DeviceId==0x13fe) {
            *luid=desc.AdapterLuid; ++found;
        }
    }
    factory->Release();
    if (found!=1) { error("unique-bc250",found); return false; }
    return true;
}
static bool sample_valid(const BC250_LATCH_SAMPLE& s)
{
    return s.ValidMask==((1ul<<Bc250LatchWordCount)-1) && s.BracketsMatch==1;
}
static bool complete_reply(NTSTATUS status,const BC250_ESCAPE_LATCH_PROBE& data,
                           unsigned long version,unsigned long arm)
{
    const auto& r=data.Result;
    return status==0 && data.Magic==BC250_ESCAPE_MAGIC && data.Command==BC250_ESCAPE_LATCH_PROBE_COMMAND &&
        data.Version==version && data.ExpectedVersion==version && data.AbiVersion==BC250_LATCH_PROBE_ABI &&
        data.Arm==arm && r.Arm==arm && data.Status==BC250_ESCAPE_STATUS_DONE && data.NtStatus==0 &&
        r.Status==0 && r.UnlockStatus==0 && r.RestoreStatus==0 && r.EndStatus==0 &&
        r.Restored==1 && r.BoundaryObserved==1 && r.Samples>=4 && r.WritesAttempted>0 &&
        r.AddressA!=0 && r.AddressB!=0 && r.AddressA!=r.AddressB && sample_valid(r.Before) &&
        sample_valid(r.Armed) && sample_valid(r.Boundary) && sample_valid(r.AfterUnlock) &&
        r.Before.Physical==r.AddressA && r.Before.LockClear==1 &&
        r.AfterUnlock.Physical==r.AddressB && r.AfterUnlock.LockClear==1;
}
int main(int argc,char** argv)
{
    if (argc==2 && !strcmp(argv[1],"--help")) {
        puts("dcn_latch --expected-version <KMD version word> --arm <0|1|2>");
        puts("Administrator and lab KMD required. MUTATES scanout; driver-owned buffers only.");
        puts("0: unlocked control; 1: lock-before-program control; 2: program-before-lock discriminator.");
        puts("Run controls separately; no automatic retry. Freeze loaded KMD identity before use.");
        return 0;
    }
    unsigned long version=0,arm=Bc250LatchArmCount;
    unsigned seen=0;
    if (argc!=5) return error("arguments",0);
    for (int i=1;i<argc;i+=2) {
        unsigned flag=!strcmp(argv[i],"--expected-version")?1u:!strcmp(argv[i],"--arm")?2u:0u;
        if (!flag || (seen&flag) || !number(argv[i+1],flag==1?&version:&arm)) return error("arguments",0);
        seen|=flag;
    }
    if (seen!=3 || !version || arm>=Bc250LatchArmCount) return error("bounds",0);
    LARGE_INTEGER frequency={},before={},after={};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart<=0) return error("qpc",0);
    LUID luid={};
    if (!select_adapter(&luid)) return 1;
    D3DKMT_OPENADAPTERFROMLUID open={}; open.AdapterLuid=luid;
    NTSTATUS status=D3DKMTOpenAdapterFromLuid(&open);
    if (status<0) return error("open",(unsigned long)status);
    D3DKMT_CLOSEADAPTER close={}; close.hAdapter=open.hAdapter;
    BC250_ESCAPE info={}; info.Magic=BC250_ESCAPE_MAGIC; info.Command=BC250_ESCAPE_GET_INFO;
    D3DKMT_ESCAPE request={};
    request.hAdapter=open.hAdapter; request.Type=D3DKMT_ESCAPE_DRIVERPRIVATE;
    request.pPrivateDriverData=&info; request.PrivateDriverDataSize=sizeof(info);
    status=D3DKMTEscape(&request);
    if (status!=0 || info.Magic!=BC250_ESCAPE_MAGIC || info.Command!=BC250_ESCAPE_GET_INFO ||
        info.Status!=BC250_ESCAPE_STATUS_DONE || info.Version!=version) {
        D3DKMTCloseAdapter(&close);
        return error("identity-before-probe",(unsigned long)status);
    }
    BC250_ESCAPE_LATCH_PROBE data={};
    data.Magic=BC250_ESCAPE_MAGIC; data.Command=BC250_ESCAPE_LATCH_PROBE_COMMAND;
    data.AbiVersion=BC250_LATCH_PROBE_ABI; data.Arm=arm; data.ExpectedVersion=version;
    request.Flags.HardwareAccess=1;
    request.pPrivateDriverData=&data; request.PrivateDriverDataSize=sizeof(data);
    printf("{\"type\":\"header\",\"schema\":1,\"arm\":%lu,\"expected_version\":%lu,"
           "\"qpc_frequency\":%lld,\"hardware_access\":true,\"atomic\":false}\n",arm,version,frequency.QuadPart);
    fflush(stdout);
    QueryPerformanceCounter(&before);
    status=D3DKMTEscape(&request); // One call, including all bounded kernel cleanup.
    QueryPerformanceCounter(&after);
    // Preserve all raw bytes, including a partial/refused reply. DWORDs are
    // little-endian Windows ABI words; addresses are never converted to double.
    unsigned long words[sizeof(data)/sizeof(unsigned long)];
    memcpy(words,&data,sizeof(data));
    printf("{\"type\":\"result\",\"begin_qpc\":%lld,\"end_qpc\":%lld,\"escape_status\":%lu,"
           "\"nt_status\":%lu,\"measurement_status\":%lu,\"unlock_status\":%lu,"
           "\"restore_status\":%lu,\"end_status\":%lu,\"samples\":%lu,\"writes_attempted\":%lu,"
           "\"boundary_observed\":%lu,\"reserved_a_restored\":%lu,\"raw_dwords\":[",
           before.QuadPart,after.QuadPart,(unsigned long)status,data.NtStatus,
           (unsigned long)data.Result.Status,(unsigned long)data.Result.UnlockStatus,
           (unsigned long)data.Result.RestoreStatus,(unsigned long)data.Result.EndStatus,
           data.Result.Samples,data.Result.WritesAttempted,data.Result.BoundaryObserved,data.Result.Restored);
    for (unsigned i=0;i<sizeof(words)/sizeof(words[0]);++i) printf("%s%lu",i?",":"",words[i]);
    puts("]}"); fflush(stdout);
    bool valid=complete_reply(status,data,version,arm);
    int result=valid?0:error("refused-or-mismatched-reply",data.NtStatus);
    status=D3DKMTCloseAdapter(&close);
    if (status<0) result=error("close",(unsigned long)status);
    // Exit zero is a complete record, not an H1/H2 verdict or desktop proof.
    printf("{\"type\":\"end\",\"exit_code\":%d,\"hardware_verdict\":null}\n",result);
    return result;
}
