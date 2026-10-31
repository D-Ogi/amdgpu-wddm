// test_amd_face_props.cpp - the properties of the AMD face that tell a ROCm library whether the
// host can read device memory through a device pointer.
//
// A hipMalloc pointer of this runtime is a GPU address only. torch reads a one-element tensor
// through its device pointer on the host when isLargeBar is 1 (_local_scalar_dense_cuda in
// torch_hip.dll), and while the face said 1, every .item() was an access violation. The test
// loads the amdhip64_7.dll that argv[1] names (the mock DLL of build-amd-abi.ps1) and fails
// when a property or an attribute says that the host can read device memory.
//
// Compiled against AMD's hip_runtime_api.h, as amd_abi.cpp is: the fields are AMD's R0600 ones.
#include <windows.h>

#include <cstdio>

#include <hip/hip_runtime_api.h>

using GetProps = hipError_t (*)(hipDeviceProp_tR0600*, int);
using GetAttr = hipError_t (*)(int*, hipDeviceAttribute_t, int);

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: test_amd_face_props <amdhip64_7.dll>\n");
        return 2;
    }
    HMODULE dll = LoadLibraryExW(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (dll == nullptr) {
        std::printf("FAIL: LoadLibrary error %lu\n", GetLastError());
        return 1;
    }
    auto get_props = reinterpret_cast<GetProps>(GetProcAddress(dll, "hipGetDevicePropertiesR0600"));
    auto get_attr = reinterpret_cast<GetAttr>(GetProcAddress(dll, "hipDeviceGetAttribute"));
    if (get_props == nullptr || get_attr == nullptr) {
        std::printf("FAIL: hipGetDevicePropertiesR0600 or hipDeviceGetAttribute is not exported\n");
        return 1;
    }
    hipDeviceProp_tR0600 p{};
    const hipError_t props_err = get_props(&p, 0);
    if (props_err != hipSuccess) {
        std::printf("FAIL: hipGetDevicePropertiesR0600 error %d\n", static_cast<int>(props_err));
        return 1;
    }
    const struct {
        const char* name;
        int value;
    } fields[] = {{"isLargeBar", p.isLargeBar},
                  {"unifiedAddressing", p.unifiedAddressing},
                  {"managedMemory", p.managedMemory},
                  {"pageableMemoryAccess", p.pageableMemoryAccess},
                  {"pageableMemoryAccessUsesHostPageTables", p.pageableMemoryAccessUsesHostPageTables},
                  {"directManagedMemAccessFromHost", p.directManagedMemAccessFromHost},
                  {"concurrentManagedAccess", p.concurrentManagedAccess}};
    const struct {
        const char* name;
        hipDeviceAttribute_t attr;
    } attrs[] = {{"IsLargeBar", hipDeviceAttributeIsLargeBar},
                 {"UnifiedAddressing", hipDeviceAttributeUnifiedAddressing},
                 {"ManagedMemory", hipDeviceAttributeManagedMemory},
                 {"PageableMemoryAccess", hipDeviceAttributePageableMemoryAccess},
                 {"PageableMemoryAccessUsesHostPageTables",
                  hipDeviceAttributePageableMemoryAccessUsesHostPageTables},
                 {"DirectManagedMemAccessFromHost", hipDeviceAttributeDirectManagedMemAccessFromHost},
                 {"ConcurrentManagedAccess", hipDeviceAttributeConcurrentManagedAccess}};
    int failed = 0;
    for (const auto& f : fields) {
        if (f.value != 0) {
            std::printf("FAIL: property %s is %d, not 0\n", f.name, f.value);
            ++failed;
        }
    }
    for (const auto& a : attrs) {
        int value = -1;
        const hipError_t err = get_attr(&value, a.attr, 0);
        if (err != hipSuccess || value != 0) {
            std::printf("FAIL: attribute %s is %d (error %d), not 0\n", a.name, value, static_cast<int>(err));
            ++failed;
        }
    }
    if (failed != 0) {
        return 1;
    }
    std::printf("PASS: %zu properties and %zu attributes say that the host cannot read device memory\n",
                sizeof(fields) / sizeof(fields[0]), sizeof(attrs) / sizeof(attrs[0]));
    return 0;
}
