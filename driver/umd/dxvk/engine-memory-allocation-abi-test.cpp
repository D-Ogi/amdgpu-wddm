// SPDX-License-Identifier: MIT
// BD-309: the shell's copy of the hosted ICD's memory allocation query (engine-memory-allocation.h) against the
// Mesa header it copies (src/amd/vulkan/radv_bc250_memory.h). The shell does not build against Mesa headers, so
// this test is the only place where the two meet: name, revision, flag, struct layout and the query's signature.
// tools/build/test-umd-memory-allocation-abi.ps1 builds it with -MesaSource.
#include "engine-memory-allocation.h"
extern "C" {
#include "radv_bc250_memory.h"
}
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <tuple>
#include <type_traits>
using namespace bc250::umd;
namespace {
int checks,failures;
void check(bool ok,const char *what){
    ++checks;
    if(!ok){++failures;std::printf("FAIL memory allocation ABI: %s\n",what);}
}
template<typename> struct Signature;
template<typename R,typename... A> struct Signature<R (VKAPI_PTR *)(A...)> {
    using result=R; using args=std::tuple<A...>; static constexpr size_t arity=sizeof...(A);
};
using Shell=Signature<PFN_bc250GetMemoryAllocation>;
using Mesa=Signature<PFN_bc250_get_memory_allocation>;
using MesaInfo=bc250_memory_allocation;
#define SAME_FIELD(shell,mesa) \
    check(offsetof(MemoryAllocationInfo,shell)==offsetof(MesaInfo,mesa) && \
          std::is_same_v<decltype(MemoryAllocationInfo::shell),decltype(MesaInfo::mesa)>, \
          "field " #shell ": same offset and type as " #mesa)
}
int main(){
    check(!std::strcmp(kMemoryAllocationQueryName,BC250_MEMORY_ALLOCATION_QUERY_NAME),"query name");
    check(kMemoryAllocationRevision==BC250_MEMORY_ALLOCATION_REVISION,"revision");
    check(kMemoryAllocationCpuMapped==BC250_MEMORY_ALLOCATION_CPU_MAPPED,"CPU-mapped flag");
    check(sizeof(MemoryAllocationInfo)==sizeof(MesaInfo) && alignof(MemoryAllocationInfo)==alignof(MesaInfo),
          "struct size and alignment");
    SAME_FIELD(revision,revision); SAME_FIELD(size,size); SAME_FIELD(allocation,allocation);
    SAME_FIELD(flags,flags); SAME_FIELD(bytes,bytes);
    check(std::is_same_v<Shell::result,Mesa::result> && Shell::arity==3 && Mesa::arity==3,"query result and arity");
    check(std::is_same_v<std::tuple_element_t<0,Shell::args>,std::tuple_element_t<0,Mesa::args>> &&
          std::is_same_v<std::tuple_element_t<1,Shell::args>,std::tuple_element_t<1,Mesa::args>>,
          "query device and memory parameters");
    check(std::is_same_v<std::tuple_element_t<2,Shell::args>,MemoryAllocationInfo *> &&
          std::is_same_v<std::tuple_element_t<2,Mesa::args>,MesaInfo *>,"query output parameter");
    // The values the ICD writes land where the shell reads them.
    MesaInfo written{BC250_MEMORY_ALLOCATION_REVISION,sizeof(MesaInfo),0x40000123u,BC250_MEMORY_ALLOCATION_CPU_MAPPED,
                     0x123456789ull};
    MemoryAllocationInfo read{};
    std::memcpy(&read,&written,sizeof(read)<sizeof(written) ? sizeof(read) : sizeof(written));
    check(read.revision==kMemoryAllocationRevision && read.size==sizeof(MesaInfo) && read.allocation==0x40000123u &&
          read.flags==kMemoryAllocationCpuMapped && read.bytes==0x123456789ull,"a filled Mesa struct read as the shell's");
    if(failures){std::printf("FAIL memory allocation ABI: %d of %d checks\n",failures,checks);return 1;}
    std::printf("PASS memory allocation ABI (BD-309): name, revision, flag, layout of %zu bytes and the query signature "
                "match radv_bc250_memory.h (%d checks)\n",sizeof(MemoryAllocationInfo),checks);
    return 0;
}
