// SPDX-License-Identifier: MIT
#include "memory-policy.h"
#include <cassert>
#include <cstdio>
using namespace native12;
static const LUID expected{0x89abcdefu,static_cast<LONG>(0xfedcba98u)};
static unsigned opens{},queries{},closes{};
static NTSTATUS open_status{},query_status{},close_status{};
static bool null_handle{},coherent{true},wrong_index{},reserved{},missing_bits{};
static NTSTATUS APIENTRY open_adapter(D3DKMT_OPENADAPTERFROMLUID* a){
 ++opens;assert(a && !a->hAdapter && a->AdapterLuid.LowPart==expected.LowPart && a->AdapterLuid.HighPart==expected.HighPart);
 if(open_status>=0 && !null_handle)a->hAdapter=0x12345678;return open_status;
}
static NTSTATUS APIENTRY query_adapter(const D3DKMT_QUERYADAPTERINFO* a){
 ++queries;assert(a && a->hAdapter==0x12345678 && a->Type==KMTQAITYPE_QUERY_GPUMMU_CAPS);
 assert(a->PrivateDriverDataSize==sizeof(D3DKMT_QUERY_GPUMMU_CAPS) && a->pPrivateDriverData);
 auto caps=static_cast<D3DKMT_QUERY_GPUMMU_CAPS*>(a->pPrivateDriverData);
 assert(!caps->PhysicalAdapterIndex && !caps->Caps.Flags.Value && !caps->Caps.VirtualAddressBitCount);
 caps->PhysicalAdapterIndex=wrong_index?1u:0u;
 caps->Caps.Flags.CacheCoherentMemorySupported=coherent;
 caps->Caps.Flags.Reserved=reserved?1u:0u;
 caps->Caps.VirtualAddressBitCount=missing_bits?0u:48u;return query_status;
}
static NTSTATUS APIENTRY close_adapter(const D3DKMT_CLOSEADAPTER* a){
 ++closes;assert(a && a->hAdapter==0x12345678);return close_status;
}
int main(){
 MemoryPolicyKmt kmt{open_adapter,query_adapter,close_adapter};bool result=true;D3DKMT_HANDLE unresolved=99;
 assert(query_memory_policy(expected,kmt,nullptr,&unresolved)==E_INVALIDARG && !opens && !unresolved);
 assert(query_memory_policy(expected,kmt,&result,nullptr)==E_INVALIDARG && !result && !opens);
 for(unsigned i=0;i<3;++i){auto bad=kmt;if(i==0)bad.open=nullptr;if(i==1)bad.query=nullptr;if(i==2)bad.close=nullptr;
 result=true;assert(query_memory_policy(expected,bad,&result,&unresolved)==E_INVALIDARG && !result && !opens);}
 assert(query_memory_policy(expected,kmt,&result,&unresolved)==S_OK && result && opens==1 && queries==1 && closes==1);
 coherent=false;result=true;assert(query_memory_policy(expected,kmt,&result,&unresolved)==S_OK && !result && closes==2);coherent=true;
 const auto failure=static_cast<NTSTATUS>(0xc0000001u);
 open_status=failure;assert(query_memory_policy(expected,kmt,&result,&unresolved)==HRESULT_FROM_NT(failure) && !result && queries==2 && closes==2);open_status=0;
 null_handle=true;assert(query_memory_policy(expected,kmt,&result,&unresolved)==E_UNEXPECTED && !result && closes==2);null_handle=false;
 open_status=0x103;assert(query_memory_policy(expected,kmt,&result,&unresolved)==E_UNEXPECTED && !result && queries==2 && closes==3);open_status=0;
 query_status=failure;assert(query_memory_policy(expected,kmt,&result,&unresolved)==HRESULT_FROM_NT(failure) && !result && closes==4);query_status=0;
 query_status=0x103;assert(query_memory_policy(expected,kmt,&result,&unresolved)==E_UNEXPECTED && !result && closes==5);query_status=0;
 close_status=failure;assert(query_memory_policy(expected,kmt,&result,&unresolved)==HRESULT_FROM_NT(failure) && !result && closes==6 && unresolved==0x12345678);close_status=0;
 close_status=0x103;assert(query_memory_policy(expected,kmt,&result,&unresolved)==E_UNEXPECTED && !result && closes==7 && unresolved==0x12345678);close_status=0;
 wrong_index=true;assert(query_memory_policy(expected,kmt,&result,&unresolved)==E_UNEXPECTED && !result && closes==8);wrong_index=false;
 reserved=true;assert(query_memory_policy(expected,kmt,&result,&unresolved)==E_UNEXPECTED && !result && closes==9);reserved=false;
 missing_bits=true;assert(query_memory_policy(expected,kmt,&result,&unresolved)==E_UNEXPECTED && !result && closes==10);missing_bits=false;
 assert(query_memory_policy(expected,kmt,&result,&unresolved)==S_OK && result && closes==11 && !unresolved);
 // Both failures must preserve the owner and report closure, not the query.
 const auto close_failure=static_cast<NTSTATUS>(0xc000000du);
 query_status=failure;close_status=close_failure;
 assert(query_memory_policy(expected,kmt,&result,&unresolved)==HRESULT_FROM_NT(close_failure) && !result && unresolved==0x12345678 && closes==12);
 query_status=0;open_status=0x103;
 const auto old_queries=queries;
 assert(query_memory_policy(expected,kmt,&result,&unresolved)==HRESULT_FROM_NT(close_failure) && !result && unresolved==0x12345678 && closes==13 && queries==old_queries);
 open_status=0;close_status=0;
 assert(query_memory_policy(expected,kmt,&result,&unresolved)==S_OK && result && !unresolved && closes==14);
 // The RAII fallback also transfers failed ownership, exactly once.
 unresolved=0;close_status=close_failure;
 {memory_policy_detail::AdapterHandle guard(0x12345678,close_adapter,&unresolved);}
 assert(unresolved==0x12345678 && closes==15);
 close_status=0;
 std::puts("PASS memory policy: typed LUID query, true/false capability, open/query/close failures, malformed responses, exact cleanup");
}
