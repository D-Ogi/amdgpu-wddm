// SPDX-License-Identifier: MIT
#include "host-contract.h"
#include "adapter-query-scope.h"
#include <cstdio>

namespace {
unsigned checks{}, failures{}, calls{};
int mode{};
void check(bool ok, const char* what) {
    ++checks;
    if(!ok){++failures;std::printf("FAIL %s\n",what);}
}
VKAPI_ATTR VkResult VKAPI_CALL query(uint32_t size, bc250_host_contract* out) {
    ++calls;
    if(mode==1 || size!=sizeof(*out))return VK_ERROR_INITIALIZATION_FAILED;
    *out=bc250_host_contract_description();
    if(mode>=2){
        static_assert(sizeof(*out)==17*sizeof(uint32_t));
        uint32_t words[17]{};
        std::memcpy(words,out,sizeof(words));
        words[mode-2]^=1;
        std::memcpy(out,words,sizeof(words));
    }
    return VK_SUCCESS;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL get(VkInstance instance,const char* name) {
    if(instance || mode==-1 || std::strcmp(name,BC250_HOST_CONTRACT_QUERY_NAME))return nullptr;
    return reinterpret_cast<PFN_vkVoidFunction>(query);
}
}
int main(int argc,char** argv) {
    const char* reason{};
    const HRESULT mismatch=HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    check(native12::check_host_contract(nullptr,&reason)==mismatch,"no entry point");
    mode=-1;
    check(native12::check_host_contract(get,&reason)==mismatch && calls==0 &&
          !std::strcmp(reason,"missing-query"),"old ICD fails before instance creation");
    mode=0;
    check(native12::check_host_contract(get,&reason)==S_OK && !std::strcmp(reason,"match"),"same contract");
    mode=1;
    check(native12::check_host_contract(get,&reason)==mismatch &&
          !std::strcmp(reason,"query-refused"),"refused query");
    for(mode=2;mode<19;++mode)
        check(native12::check_host_contract(get,&reason)==mismatch &&
              !std::strcmp(reason,"layout-or-feature-mismatch"),"each contract word is checked");
    if(argc==2){
        // The DirectMesh preset must not silently opt this process into unmeasured hybrid task.
        SetEnvironmentVariableA("RADV_DIRECTMESH", "1");
        SetEnvironmentVariableA("RADV_BC250_HYBRID_TASK", nullptr);
        HMODULE module=LoadLibraryExA(argv[1],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        check(module!=nullptr,"load exact ICD");
        if(module){
            auto entry=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(module,"vk_icdGetInstanceProcAddr"));
            const HRESULT hr=native12::check_host_contract(entry,&reason);
            check(hr==S_OK,"built ICD agrees with shell contract");
            std::printf("ICD contract %s %08lx\n",reason,static_cast<unsigned long>(hr));
            if(hr==S_OK){
                native12::AdapterQueryScope scope(entry,42,BC250_HOST_POLICY_SPARSE);
                check(scope.entered(),"adapter query scope entered");
                auto make=reinterpret_cast<PFN_vkCreateInstance>(scope.entry()(VK_NULL_HANDLE,"vkCreateInstance"));
                VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
                VkInstance instance{};
                const VkResult result=make?make(&info,nullptr,&instance):VK_ERROR_INITIALIZATION_FAILED;
                check(result==VK_SUCCESS && instance,"built ICD creates adapter-query instance");
                char hybrid[8]{};
                check(!GetEnvironmentVariableA("RADV_BC250_HYBRID_TASK", hybrid, sizeof(hybrid)),
                      "DirectMesh keeps hybrid task opt-in");
                if(result==VK_SUCCESS && instance){
                    auto drop=reinterpret_cast<PFN_vkDestroyInstance>(scope.entry()(instance,"vkDestroyInstance"));
                    check(drop!=nullptr,"built ICD destroy entry exists");
                    if(drop)drop(instance,nullptr);
                }
                check(scope.completed(),"query instance retires without any forbidden runtime operation");
            }
            FreeLibrary(module);
        }
    }else if(argc!=1){
        check(false,"expected zero or one ICD pathname");
    }
    std::printf("host contract: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
