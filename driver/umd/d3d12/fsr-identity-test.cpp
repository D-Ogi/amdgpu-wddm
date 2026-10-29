// SPDX-License-Identifier: MIT
// Host test of fsr-identity.h: the escape the D3D12 shell sends for FsrUpgrade, against fake KMT entry points.
#include "fsr-identity.h"
#include <cstdio>
#include <cwchar>

namespace {
int failures=0;
#define CHECK(c) do{if(!(c)){std::printf("FAIL %s (line %d)\n",#c,__LINE__);++failures;}}while(0)

struct Fake {
    unsigned opens=0,escapes=0,closes=0;
    NTSTATUS open_status=0,escape_status=0,close_status=0;
    D3DKMT_HANDLE handle=0x40000200u;
    LUID luid{};
    BC250_ESCAPE_PROCESS_POLICY seen{};
    UINT seen_size=0;
    D3DKMT_ESCAPETYPE seen_type=D3DKMT_ESCAPE_DRIVERPRIVATE;
    D3DKMT_HANDLE seen_adapter=0,closed_adapter=0;
    unsigned long reply_status=BC250_ESCAPE_STATUS_DONE;
} fake;

NTSTATUS APIENTRY open(D3DKMT_OPENADAPTERFROMLUID* a) {
    ++fake.opens;fake.luid=a->AdapterLuid;
    if(fake.open_status==0)a->hAdapter=fake.handle;
    return fake.open_status;
}
NTSTATUS APIENTRY escape(const D3DKMT_ESCAPE* e) {
    ++fake.escapes;fake.seen_adapter=e->hAdapter;fake.seen_type=e->Type;fake.seen_size=e->PrivateDriverDataSize;
    if(e->PrivateDriverDataSize==sizeof(fake.seen))std::memcpy(&fake.seen,e->pPrivateDriverData,sizeof(fake.seen));
    if(fake.escape_status==0){
        auto* p=static_cast<BC250_ESCAPE_PROCESS_POLICY*>(e->pPrivateDriverData);
        p->Status=fake.reply_status;p->Answer=p->FsrIdentity;p->Source=BC250_AMD_IDENTITY_SOURCE_SHELL;
        p->NameFlags=BC250_AMD_IDENTITY_NAME_KNOWN;
    }
    return fake.escape_status;
}
NTSTATUS APIENTRY close(const D3DKMT_CLOSEADAPTER* c) {++fake.closes;fake.closed_adapter=c->hAdapter;return fake.close_status;}

const native12::FsrIdentityKmt kmt{open,escape,close};

void request_tests() {
    const auto p=native12::fsr_identity_request(1,L"Game.EXE");
    CHECK(sizeof(p)==176);
    CHECK(p.Magic==BC250_ESCAPE_MAGIC && p.Command==BC250_ESCAPE_SET_PROCESS_POLICY && p.AbiVersion==BC250_PROCESS_POLICY_ABI);
    CHECK(p.FsrIdentity==1 && p.Reserved0==0 && p.Status==0 && p.Answer==0);
    CHECK(p.Image[0]==L'G' && p.Image[7]==L'E' && p.Image[8]==0);
    wchar_t longName[200];
    for(unsigned i=0;i<199;++i)longName[i]=L'a';
    longName[199]=0;
    const auto q=native12::fsr_identity_request(0,longName);
    CHECK(q.Image[BC250_PROCESS_POLICY_IMAGE_CHARS-2]==L'a' && q.Image[BC250_PROCESS_POLICY_IMAGE_CHARS-1]==0);
    const auto n=native12::fsr_identity_request(0,nullptr);
    CHECK(n.Image[0]==0);
}

void send_tests() {
    LUID luid{};luid.LowPart=0x1234;luid.HighPart=5;
    {   // Not set: nothing is opened or sent.
        fake=Fake{};
        const auto r=native12::send_fsr_identity(luid,kmt,false,1,L"game.exe");
        CHECK(r.hr==S_FALSE && fake.opens==0 && fake.escapes==0 && fake.closes==0);
    }
    {   // 0 and 1 are sent once, on the adapter of the LUID, which is closed again.
        for(unsigned v=0;v<=1;++v){
            fake=Fake{};
            const auto r=native12::send_fsr_identity(luid,kmt,true,v,L"game.exe");
            CHECK(r.hr==S_OK && fake.opens==1 && fake.escapes==1 && fake.closes==1);
            CHECK(fake.luid.LowPart==0x1234 && fake.luid.HighPart==5);
            CHECK(fake.seen_adapter==fake.handle && fake.closed_adapter==fake.handle);
            CHECK(fake.seen_type==D3DKMT_ESCAPE_DRIVERPRIVATE && fake.seen_size==sizeof(BC250_ESCAPE_PROCESS_POLICY));
            CHECK(fake.seen.FsrIdentity==v && !std::wcscmp(reinterpret_cast<const wchar_t*>(fake.seen.Image),L"game.exe"));
            CHECK(r.status==BC250_ESCAPE_STATUS_DONE && r.answer==v && r.source==BC250_AMD_IDENTITY_SOURCE_SHELL);
            CHECK(!std::strcmp(native12::fsr_identity_source_name(r.source),"shell"));
        }
    }
    {   // A value out of range and missing entry points are refused before any call.
        fake=Fake{};
        CHECK(native12::send_fsr_identity(luid,kmt,true,2,L"game.exe").hr==E_INVALIDARG && fake.opens==0);
        native12::FsrIdentityKmt none{};
        CHECK(native12::send_fsr_identity(luid,none,true,1,L"game.exe").hr==E_INVALIDARG);
    }
    {   // Open fails: no escape, no close.
        fake=Fake{};fake.open_status=static_cast<NTSTATUS>(0xC0000225L);
        const auto r=native12::send_fsr_identity(luid,kmt,true,1,L"game.exe");
        CHECK(FAILED(r.hr) && fake.escapes==0 && fake.closes==0 && r.status==0xFFFFFFFFul);
    }
    {   // The escape fails: the adapter is still closed.
        fake=Fake{};fake.escape_status=static_cast<NTSTATUS>(0xC000000DL);
        const auto r=native12::send_fsr_identity(luid,kmt,true,1,L"game.exe");
        CHECK(r.hr==HRESULT_FROM_NT(static_cast<NTSTATUS>(0xC000000DL)) && fake.closes==1 && r.status==0xFFFFFFFFul);
    }
    {   // The KMD refused (no process object): an error, with its status for the log.
        fake=Fake{};fake.reply_status=BC250_ESCAPE_STATUS_REFUSED;
        const auto r=native12::send_fsr_identity(luid,kmt,true,1,L"game.exe");
        CHECK(r.hr==E_FAIL && r.status==BC250_ESCAPE_STATUS_REFUSED && fake.closes==1);
    }
    {   // The close fails after a good escape: reported.
        fake=Fake{};fake.close_status=static_cast<NTSTATUS>(0xC0000008L);
        const auto r=native12::send_fsr_identity(luid,kmt,true,0,L"game.exe");
        CHECK(FAILED(r.hr) && r.status==BC250_ESCAPE_STATUS_DONE && r.close==static_cast<NTSTATUS>(0xC0000008L));
    }
    CHECK(!std::strcmp(native12::fsr_identity_source_name(99),"?"));
}
} // namespace

int main() {
    request_tests();
    send_tests();
    std::printf("fsr-identity: %s (%d failure%s)\n",failures?"FAIL":"PASS",failures,failures==1?"":"s");
    return failures?1:0;
}
