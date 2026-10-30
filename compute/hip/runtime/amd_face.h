// amd_face.h - the bridge between the AMD ABI face (amd_abi.cpp) and this runtime.
//
// amdhip64_7.dll is this runtime with the entry points that the ROCm user libraries import
// (TheRock, torch and rocBLAS among them). amd_abi.cpp is compiled against AMD's own
// hip_runtime_api.h, so that the compiler checks every entry point it defines against AMD's
// declaration; the rest of the runtime is compiled against our hip/hip_runtime.h. The two
// headers declare the same names with different types, so no file can include both.
//
// This header is the seam: plain integers, pointers and structures of our own, and nothing
// of either HIP header. hip_amd_face.cpp implements it with the state of the runtime. Every
// function returns a hipError_t value as an int; both headers give the values they share the
// same numbers (build-amd-abi.ps1 checks them).

#ifndef BC250_HIP_AMD_FACE_H
#define BC250_HIP_AMD_FACE_H

#include <stddef.h>
#include <stdint.h>

namespace bc250face {

// What the R0600 device properties and the device attributes are filled from.
struct DeviceInfo {
    char     name[64];
    uint32_t gfx_major, gfx_minor, gfx_rev;
    uint32_t pci_bus, pci_device, pci_function;
    uint32_t wave_size;
    uint32_t cu_count;
    uint32_t max_workgroup_size;
    uint32_t max_workgroups_per_dim;
    uint32_t lds_bytes_per_workgroup;
    uint32_t waves_per_cu;
    uint64_t vram_bytes;           // the carve-out
    uint64_t visible_vram_bytes;   // the part of it the host processor can map
    uint64_t total_bytes;          // what hipMemGetInfo reports as total (both budgets)
    uint32_t gfx_clock_khz;
    uint32_t mem_clock_khz;
    uint32_t mem_bus_width;
};

int device_info(DeviceInfo* out);
int device_ready();   // opens the device; hipSuccess when there is one

// Modules of hipModuleLoadData. bytes 0: the image's own headers give the size.
int module_load(const void* image, size_t bytes, void** module_out);
int module_load_file(const char* path, void** module_out);
int module_unload(void* module);
int module_get_function(void** function_out, void* module, const char* name);

// A kernel: a host stub of __hipRegisterFunction, or a hipFunction_t. Exactly one is set.
struct KernelInfo {
    uint32_t max_threads_per_block;   // the kernel's own .max_flat_workgroup_size
    uint32_t group_segment_bytes;     // static shared memory
    uint32_t private_segment_bytes;   // per work item
    uint32_t vgpr_count;
    uint32_t sgpr_count;
    uint32_t lds_limit;               // what a workgroup of this device may have in all
    int      dynamic_group_max;       // hipFuncSetAttribute, -1 when not set
};

int kernel_info(const void* host_stub, const void* function, KernelInfo* out);
int occupancy(int* blocks, const void* host_stub, const void* function, int block_size,
              size_t dynamic_bytes);

// hipModuleLaunchKernel: one of params (one pointer per explicit argument) or packed (the
// HIP_LAUNCH_PARAM_BUFFER_POINTER buffer of `extra`) is set.
int launch(const void* function, const uint32_t grid[3], const uint32_t block[3],
           uint32_t shared_bytes, void* stream, void** params, const void* packed,
           size_t packed_bytes);
// hipLaunchCooperativeKernel of a host stub. AMD passes the dynamic LDS size as unsigned int and
// our header as size_t: on x64 the fifth argument is a stack slot whose upper half AMD's caller
// does not write, so the AMD name may not reach our definition directly.
int launch_cooperative(const void* host_stub, const uint32_t grid[3], const uint32_t block[3],
                       void** args, uint32_t shared_bytes, void* stream);

int stream_query(void* stream);
int stream_flags(void* stream, unsigned* flags);
int stream_valid(void* stream);   // hipSuccess for a stream this runtime knows, the null one too
int event_query(void* event);

// A pointer the runtime knows. kind: 0 unknown, 1 host (hipHostMalloc), 2 device (hipMalloc).
struct PointerInfo {
    int      kind;
    void*    device_pointer;   // the GPU address of the byte the pointer names
    void*    host_pointer;     // the host address of that byte, or null
    uint64_t base;             // the start of the allocation, as the program sees it
    uint64_t bytes;            // the size of the allocation
    uint32_t flags;            // the BC250HSA_MEM_* flags of the allocation
};

int pointer_info(const void* ptr, PointerInfo* out);
int symbol_address(void** device_pointer, size_t* bytes, const void* host_symbol);

// The counters of the face (FaceCounters of runtime_internal.h), in a fixed order.
struct Counters {
    uint32_t struct_bytes;
    uint32_t reserved;
    uint64_t kpack_modules;
    uint64_t kpack_loads;
    uint64_t kpack_refusals;
    uint64_t kpack_library_missing;
    uint64_t bundle_alias_picks;
    uint64_t alias_loads;
    uint64_t compressed_refusals;
    uint64_t program_loads;
    uint64_t program_refusals;
    uint64_t module_launches;
    uint64_t packed_launches;
    int64_t  last_kpack_error;
};

int counters(Counters* out);

// Records err as the calling thread's last error and returns it.
int fail(int err);

// The same, with the refusal line of hip_log.cpp: which call refused, why. Every "this build
// cannot do that" of the face goes through it, as every one of the runtime does.
int refuse(const char* call, const char* why, int err);

}  // namespace bc250face

#endif
