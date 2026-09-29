// SPDX-License-Identifier: MIT
#include "instance-policy.h"
#include <cassert>
#include <cstdio>
using namespace native12;
static const LUID expected{0x89abcdefu,static_cast<LONG>(0xfedcba98u)};
static unsigned opens{},queries{},closes{};
static NTSTATUS open_status{},query_status{},close_status{};
static bool null_handle{};
static D3DDDI_QUERYREGISTRY_STATUS answer_status{D3DDDI_QUERYREGISTRY_STATUS_SUCCESS};
static ULONG answer_size{sizeof(DWORD)};
static DWORD answer_value{};
static NTSTATUS APIENTRY open_adapter(D3DKMT_OPENADAPTERFROMLUID* a){
 ++opens;assert(a && !a->hAdapter && a->AdapterLuid.LowPart==expected.LowPart && a->AdapterLuid.HighPart==expected.HighPart);
 if(open_status>=0 && !null_handle)a->hAdapter=0x12345678;return open_status;
}
static NTSTATUS APIENTRY query_adapter(const D3DKMT_QUERYADAPTERINFO* a){
 ++queries;assert(a && a->hAdapter==0x12345678 && a->Type==KMTQAITYPE_QUERYREGISTRY);
 assert(a->PrivateDriverDataSize==sizeof(D3DDDI_QUERYREGISTRY_INFO) && a->pPrivateDriverData);
 auto info=static_cast<D3DDDI_QUERYREGISTRY_INFO*>(a->pPrivateDriverData);
 assert(info->QueryType==D3DDDI_QUERYREGISTRY_ADAPTERKEY && !info->QueryFlags.Value && info->ValueType==REG_DWORD);
 assert(!std::wcscmp(info->ValueName,L"AmdgpuWddmSparseBinding") && !info->PhysicalAdapterIndex);
 assert(!info->OutputValueSize && !info->OutputQword);
 info->Status=answer_status;info->OutputValueSize=answer_size;info->OutputDword=answer_value;return query_status;
}
static NTSTATUS APIENTRY close_adapter(const D3DKMT_CLOSEADAPTER* a){
 ++closes;assert(a && a->hAdapter==0x12345678);return close_status;
}
int main(){
 using Source=SparsePolicySource;
 MemoryPolicyKmt kmt{open_adapter,query_adapter,close_adapter};SparsePolicy policy{true};D3DKMT_HANDLE unresolved=99;
 assert(query_sparse_policy(expected,kmt,nullptr,&unresolved)==E_INVALIDARG && !opens && !unresolved);
 assert(query_sparse_policy(expected,kmt,&policy,nullptr)==E_INVALIDARG && !policy.sparse && !opens);
 for(unsigned i=0;i<3;++i){auto bad=kmt;if(i==0)bad.open=nullptr;if(i==1)bad.query=nullptr;if(i==2)bad.close=nullptr;
 policy.sparse=true;assert(query_sparse_policy(expected,bad,&policy,&unresolved)==E_INVALIDARG && !policy.sparse && !opens);}
 const auto resolved=[&](bool sparse,Source source){
  const unsigned before=closes;policy={};policy.sparse=!sparse;
  return query_sparse_policy(expected,kmt,&policy,&unresolved)==S_OK && policy.sparse==sparse && policy.source==source &&
         policy.status==query_status && closes==before+1 && !unresolved;
 };
 // The value as written: 1 on, 0 off, anything else off.
 answer_value=1;assert(resolved(true,Source::RegistryOn));
 answer_value=0;assert(resolved(false,Source::RegistryOff));
 answer_value=2;assert(resolved(false,Source::Invalid));
 answer_value=0xffffffffu;assert(resolved(false,Source::Invalid));
 answer_value=1;answer_size=8;assert(resolved(false,Source::Invalid));
 answer_size=0;assert(resolved(false,Source::Invalid));answer_size=sizeof(DWORD);
 answer_status=D3DDDI_QUERYREGISTRY_STATUS_BUFFER_OVERFLOW;assert(resolved(false,Source::Invalid));
 answer_status=D3DDDI_QUERYREGISTRY_STATUS_MAX;assert(resolved(false,Source::Invalid));
 // No such value, in either of the two ways the system can say so: the default, on. What the output
 // fields hold then is not read.
 answer_status=D3DDDI_QUERYREGISTRY_STATUS_FAIL;answer_value=0;assert(resolved(true,Source::Default));
 answer_status=D3DDDI_QUERYREGISTRY_STATUS_SUCCESS;
 query_status=static_cast<NTSTATUS>(0xc0000034u);assert(resolved(true,Source::Default));
 // A key that cannot be asked: off, and the call itself succeeds. The answer's fields are not read.
 answer_value=1;
 const auto failure=static_cast<NTSTATUS>(0xc0000001u);
 query_status=failure;assert(resolved(false,Source::Unreadable));
 query_status=0x103;assert(resolved(false,Source::Unreadable));query_status=0;
 // Open and close failures are errors of the call; a failed close hands the handle over.
 const unsigned asked=queries;
 open_status=failure;policy.sparse=true;
 assert(query_sparse_policy(expected,kmt,&policy,&unresolved)==HRESULT_FROM_NT(failure) && !policy.sparse && queries==asked);open_status=0;
 null_handle=true;assert(query_sparse_policy(expected,kmt,&policy,&unresolved)==E_UNEXPECTED && !policy.sparse && queries==asked);null_handle=false;
 unsigned closed=closes;
 open_status=0x103;assert(query_sparse_policy(expected,kmt,&policy,&unresolved)==E_UNEXPECTED && !policy.sparse && queries==asked && closes==closed+1);open_status=0;
 const auto close_failure=static_cast<NTSTATUS>(0xc000000du);
 close_status=close_failure;closed=closes;
 assert(query_sparse_policy(expected,kmt,&policy,&unresolved)==HRESULT_FROM_NT(close_failure) && !policy.sparse &&
        policy.source==Source::Unreadable && unresolved==0x12345678 && closes==closed+1);
 close_status=0x103;
 assert(query_sparse_policy(expected,kmt,&policy,&unresolved)==E_UNEXPECTED && !policy.sparse && unresolved==0x12345678);
 close_status=0;
 assert(resolved(true,Source::RegistryOn));
 std::puts("PASS instance policy: value 1 on, 0 off, absent on, invalid or unreadable off, close failure hands the adapter over");
}
