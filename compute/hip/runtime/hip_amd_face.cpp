// hip_amd_face.cpp - the runtime side of amd_face.h (the AMD ABI face, amdhip64_7.dll only).
//
// Each function here is one entry point of amd_abi.cpp minus its AMD types. It takes the
// process lock exactly as the entry points of our own header do, and it follows the same rule:
// no thread holds the lock while it waits for the device (runtime_internal.h).

#include <cstdio>
#include <cstring>
#include <vector>

#include "amd_face.h"
#include "runtime_internal.h"

namespace {

using bc250hip::Guard;
using bc250hip::Module;
using bc250hip::ModuleFunction;
using bc250hip::ModuleKind;
using bc250hip::State;
using bc250hip::state;

// AMD's numbers for the errors our hipError_t does not name (hip_runtime_api.h of HIP 7.18).
constexpr int kErrorInvalidSymbol = 13;
constexpr int kErrorFileNotFound = 301;
constexpr int kErrorNotFound = 500;

Module* program_module(void* handle) {
    for (Module* module : state().modules) {
        if (module == handle && module->kind == ModuleKind::kProgram) {
            return module;
        }
    }
    return nullptr;
}

const bc250hip::Allocation* host_allocation(const void* ptr, uint64_t* offset) {
    const unsigned char* want = static_cast<const unsigned char*>(ptr);
    for (const auto& entry : state().va_by_host) {
        const auto found = state().by_va.find(entry.second);
        if (found == state().by_va.end()) {
            continue;
        }
        const bc250hsa_mem& mem = found->second.mem;
        const unsigned char* base = static_cast<const unsigned char*>(mem.host);
        if (base != nullptr && want >= base && want < base + mem.bytes) {
            *offset = static_cast<uint64_t>(want - base);
            return &found->second;
        }
    }
    return nullptr;
}

bool is_host_allocation(const bc250hip::Allocation& allocation) {
    return allocation.mem.host != nullptr &&
           state().va_by_host.find(allocation.mem.host) != state().va_by_host.end();
}

uint32_t lds_limit(const State& s) {
    return s.props_valid && s.props.lds_bytes_per_workgroup != 0 ? s.props.lds_bytes_per_workgroup
                                                                  : 65536u;
}

}  // namespace

namespace bc250face {

int fail(int err) { return bc250hip::fail(static_cast<hipError_t>(err)); }

int refuse(const char* call, const char* why, int err) {
    return bc250hip::refuse(call, nullptr, why, static_cast<hipError_t>(err));
}

int device_ready() {
    Guard guard;
    bc250hsa_device* dev = nullptr;
    return bc250hip::fail(bc250hip::device(&dev));
}

int device_info(DeviceInfo* out) {
    if (out == nullptr) {
        return fail(hipErrorInvalidValue);
    }
    Guard guard;
    bc250hsa_device* dev = nullptr;
    const hipError_t err = bc250hip::device(&dev);
    if (err != hipSuccess) {
        return bc250hip::fail(err);
    }
    const State& s = state();
    const bc250hsa_props& p = s.props;
    std::memset(out, 0, sizeof(*out));
    std::snprintf(out->name, sizeof(out->name), "%s",
                  s.props_valid && p.name[0] != '\0' ? p.name : "AMD BC-250");
    out->gfx_major = p.gfx_ip_major;
    out->gfx_minor = p.gfx_ip_minor;
    out->gfx_rev = p.gfx_ip_rev;
    out->pci_bus = p.pci_bus;
    out->pci_device = p.pci_device;
    out->pci_function = p.pci_function;
    out->wave_size = p.wave_size != 0 ? p.wave_size : 32u;
    out->cu_count = p.cu_count;
    out->max_workgroup_size = p.max_workgroup_size != 0 ? p.max_workgroup_size : 1024u;
    out->max_workgroups_per_dim = p.max_workgroups_per_dim;
    out->lds_bytes_per_workgroup = lds_limit(s);
    out->waves_per_cu = p.waves_per_cu;
    out->vram_bytes = p.vram_bytes;
    out->visible_vram_bytes = p.visible_vram_bytes;
    size_t total = 0;
    bc250hip::memory_info(nullptr, &total);
    out->total_bytes = total;
    out->gfx_clock_khz = p.gfx_clock_khz;
    out->mem_clock_khz = p.mem_clock_khz;
    out->mem_bus_width = p.mem_bus_width;
    return hipSuccess;
}

int module_load(const void* image, size_t bytes, void** module_out) {
    if (module_out == nullptr || image == nullptr) {
        return fail(hipErrorInvalidValue);
    }
    Guard guard;
    Module* module = nullptr;
    const hipError_t err = bc250hip::program_module_load(image, bytes, &module);
    if (err != hipSuccess) {
        return bc250hip::fail(err);
    }
    *module_out = module;
    return hipSuccess;
}

int module_load_file(const char* path, void** module_out) {
    if (path == nullptr || module_out == nullptr) {
        return fail(hipErrorInvalidValue);
    }
    // The file is read before the lock is taken: a read of a large code object is no business
    // of another thread's launch.
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        // The path names a file of the application's own installation, not its data, so the
        // log may carry it.
        return refuse("hipModuleLoad", path, kErrorFileNotFound);
    }
    std::vector<unsigned char> bytes;
    unsigned char chunk[65536];
    size_t got = 0;
    while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
        bytes.insert(bytes.end(), chunk, chunk + got);
    }
    std::fclose(file);
    if (bytes.size() < 4) {
        return fail(hipErrorInvalidImage);
    }
    // The loader copies what it keeps, so the buffer may die when this returns.
    return module_load(bytes.data(), bytes.size(), module_out);
}

int module_unload(void* handle) {
    Guard guard;
    if (program_module(handle) == nullptr) {
        return fail(hipErrorInvalidHandle);
    }
    bc250hsa_device* dev = nullptr;
    const hipError_t err = bc250hip::device(&dev);
    if (err != hipSuccess) {
        return bc250hip::fail(err);
    }
    // The device may still run a kernel of this module: every submission up to now retires
    // before the code goes away.
    const uint64_t value = bc250hsa_fence_last_submitted(dev);
    if (value != 0) {
        const hipError_t waited = guard.wait(dev, value);
        if (waited != hipSuccess) {
            return bc250hip::fail(waited);
        }
    }
    // The wait opened the lock, and another thread may have unloaded the module meanwhile.
    Module* module = program_module(handle);
    if (module == nullptr) {
        return fail(hipErrorInvalidHandle);
    }
    State& s = state();
    for (ModuleFunction* function : module->program_functions) {
        s.module_functions.erase(function);
        function->magic = 0;
        delete function;
    }
    module->program_functions.clear();
    for (size_t i = 0; i < s.modules.size(); ++i) {
        if (s.modules[i] == module) {
            s.modules.erase(s.modules.begin() + static_cast<ptrdiff_t>(i));
            break;
        }
    }
    if (module->loaded != nullptr) {
        bc250hsa_module_unload(module->loaded);
    }
    delete module;
    return hipSuccess;
}

int module_get_function(void** function_out, void* handle, const char* name) {
    if (function_out == nullptr || name == nullptr) {
        return fail(hipErrorInvalidValue);
    }
    Guard guard;
    Module* module = program_module(handle);
    if (module == nullptr || module->loaded == nullptr) {
        return fail(hipErrorInvalidHandle);
    }
    for (ModuleFunction* function : module->program_functions) {
        if (function->name == name) {
            *function_out = function;
            return hipSuccess;
        }
    }
    const bc250hsa_kernel* kernel = bc250hsa_module_kernel_by_name(module->loaded, name);
    if (kernel == nullptr) {
        // Not an error of the runtime: rocBLAS (Tensile) looks a kernel up in each of its code
        // objects in turn until one holds it, and each miss before that gets hipErrorNotFound.
        bc250hip::log_line(BC250HSA_LOG_INFO,
                           "hipModuleGetFunction: no kernel named '%s' in this module", name);
        return fail(kErrorNotFound);
    }
    ModuleFunction* function = new ModuleFunction();
    function->module = module;
    function->name = name;
    function->kernel = kernel;
    module->program_functions.push_back(function);
    state().module_functions[function] = function;
    *function_out = function;
    return hipSuccess;
}

int kernel_info(const void* host_stub, const void* function, KernelInfo* out) {
    if (out == nullptr || (host_stub == nullptr) == (function == nullptr)) {
        return fail(hipErrorInvalidValue);
    }
    Guard guard;
    bc250hsa_device* dev = nullptr;
    hipError_t err = bc250hip::device(&dev);
    if (err != hipSuccess) {
        return bc250hip::fail(err);
    }
    const bc250hsa_kernel* kernel = nullptr;
    err = bc250hip::resolve_kernel_ref(bc250hip::KernelRef{host_stub, function}, &kernel);
    if (err != hipSuccess) {
        return bc250hip::fail(err);
    }
    State& s = state();
    std::memset(out, 0, sizeof(*out));
    out->max_threads_per_block = kernel->max_flat_workgroup_size != 0
                                     ? kernel->max_flat_workgroup_size
                                     : 1024u;
    out->group_segment_bytes = kernel->group_segment_bytes;
    out->private_segment_bytes = kernel->private_segment_bytes;
    out->vgpr_count = kernel->vgpr_count;
    out->sgpr_count = kernel->sgpr_count;
    out->lds_limit = lds_limit(s);
    out->dynamic_group_max = -1;
    if (host_stub != nullptr) {
        const auto found = s.functions.find(host_stub);
        if (found != s.functions.end()) {
            out->dynamic_group_max = found->second.dynamic_group_max;
        }
    } else {
        out->dynamic_group_max = static_cast<const ModuleFunction*>(function)->dynamic_group_max;
    }
    return hipSuccess;
}

// The model of hipOccupancyMaxActiveBlocksPerMultiprocessor (hip_device.cpp) for either kind
// of kernel handle.
int occupancy(int* blocks, const void* host_stub, const void* function, int block_size,
              size_t dynamic_bytes) {
    if (blocks == nullptr || block_size <= 0 || (host_stub == nullptr) == (function == nullptr)) {
        return fail(hipErrorInvalidValue);
    }
    Guard guard;
    bc250hsa_device* dev = nullptr;
    hipError_t err = bc250hip::device(&dev);
    if (err != hipSuccess) {
        return bc250hip::fail(err);
    }
    const bc250hsa_kernel* kernel = nullptr;
    err = bc250hip::resolve_kernel_ref(bc250hip::KernelRef{host_stub, function}, &kernel);
    if (err != hipSuccess) {
        return bc250hip::fail(err);
    }
    const State& s = state();
    const uint32_t wave = s.props.wave_size != 0 ? s.props.wave_size : 32u;
    const uint32_t max_block = s.props.max_workgroup_size != 0 ? s.props.max_workgroup_size : 1024u;
    const uint64_t group_bytes = dynamic_bytes + kernel->group_segment_bytes;
    if (group_bytes > lds_limit(s) || static_cast<uint32_t>(block_size) > max_block) {
        *blocks = 0;
        return hipSuccess;
    }
    bc250hip::OccupancyInput in;
    in.block_threads = static_cast<uint32_t>(block_size);
    in.wave_size = wave;
    in.waves_per_cu = s.props.waves_per_cu;
    in.vgprs = s.occupancy_registers ? bc250hip::occupancy_kernel_vgprs(*kernel) : 0u;
    in.group_bytes = group_bytes;
    in.lds_bytes_per_cu = lds_limit(s);
    *blocks = bc250hip::occupancy_blocks_per_cu(in);
    return hipSuccess;
}

int launch(const void* function, const uint32_t grid[3], const uint32_t block[3],
           uint32_t shared_bytes, void* stream, void** params, const void* packed,
           size_t packed_bytes) {
    if (function == nullptr) {
        return fail(hipErrorInvalidHandle);
    }
    Guard guard;
    state().face.module_launches++;
    return bc250hip::launch_kernel(guard, "hipModuleLaunchKernel",
                                   bc250hip::KernelRef{nullptr, function}, grid, block,
                                   shared_bytes, static_cast<hipStream_t>(stream), params, packed,
                                   packed_bytes);
}

int launch_cooperative(const void* host_stub, const uint32_t grid[3], const uint32_t block[3],
                       void** args, uint32_t shared_bytes, void* stream) {
    return hipLaunchCooperativeKernel(host_stub, dim3(grid[0], grid[1], grid[2]),
                                      dim3(block[0], block[1], block[2]), args, shared_bytes,
                                      static_cast<hipStream_t>(stream));
}

int stream_valid(void* stream) {
    Guard guard;
    return bc250hip::resolve_stream(static_cast<hipStream_t>(stream)) != nullptr
               ? hipSuccess
               : fail(hipErrorInvalidHandle);
}

int stream_flags(void* stream, unsigned* flags) {
    if (flags == nullptr) {
        return fail(hipErrorInvalidValue);
    }
    Guard guard;
    const ihipStream_t* target = bc250hip::resolve_stream(static_cast<hipStream_t>(stream));
    if (target == nullptr) {
        return fail(hipErrorInvalidHandle);
    }
    *flags = bc250hip::is_null_stream(target) ? 0u : target->flags;
    return hipSuccess;
}

// hipStreamQuery and hipEventQuery do not wait. A launch that still sits in an open batch has
// not reached the device, and a program that polls would poll forever, so both submit what is
// open first (a flush point of section 8.1 of bc250hsa.h, as hipEventRecord is).
int stream_query(void* stream) {
    Guard guard;
    const ihipStream_t* target = bc250hip::resolve_stream(static_cast<hipStream_t>(stream));
    if (target == nullptr) {
        return fail(hipErrorInvalidHandle);
    }
    bc250hsa_device* dev = nullptr;
    const hipError_t err = bc250hip::device(&dev);
    if (err != hipSuccess) {
        return bc250hip::fail(err);
    }
    (void)bc250hsa_flush(dev, nullptr);
    const uint64_t want = bc250hip::stream_target_value(dev, target);
    const uint64_t retired = bc250hsa_fence_read(dev);
    if (retired == UINT64_MAX) {
        return bc250hip::fail_status(BC250HSA_EDEVICELOST);
    }
    return want <= retired ? hipSuccess : hipErrorNotReady;   // not ready is not an error
}

int event_query(void* handle) {
    Guard guard;
    ihipEvent_t* event = static_cast<ihipEvent_t*>(handle);
    if (event == nullptr || event->magic != BC250_HIP_EVENT_MAGIC) {
        return fail(hipErrorInvalidHandle);
    }
    if (event->recorded == 0 || event->pending == 0) {
        return hipSuccess;
    }
    bc250hsa_device* dev = nullptr;
    const hipError_t err = bc250hip::device(&dev);
    if (err != hipSuccess) {
        return bc250hip::fail(err);
    }
    (void)bc250hsa_flush(dev, nullptr);
    if (bc250hsa_fence_read(dev) == UINT64_MAX) {
        return bc250hip::fail_status(BC250HSA_EDEVICELOST);
    }
    bc250hip::events_stamp_retired(dev);
    return event->pending == 0 ? hipSuccess : hipErrorNotReady;
}

int pointer_info(const void* ptr, PointerInfo* out) {
    if (out == nullptr) {
        return fail(hipErrorInvalidValue);
    }
    std::memset(out, 0, sizeof(*out));
    if (ptr == nullptr) {
        return hipSuccess;
    }
    Guard guard;
    uint64_t offset = 0;
    // A GPU address first: the pointers of hipMalloc are GPU addresses (hip_memory.cpp).
    const bc250hip::Allocation* allocation = bc250hip::find_allocation_ptr(ptr, 0, &offset);
    if (allocation != nullptr && offset < allocation->mem.bytes) {
        const bool host = is_host_allocation(*allocation);
        out->kind = host ? 1 : 2;
        out->device_pointer = const_cast<void*>(ptr);
        out->host_pointer = host ? static_cast<unsigned char*>(allocation->mem.host) + offset
                                 : nullptr;
        out->base = host ? reinterpret_cast<uintptr_t>(allocation->mem.host)
                         : allocation->mem.va;
        out->bytes = allocation->mem.bytes;
        out->flags = allocation->mem.flags;
        return hipSuccess;
    }
    allocation = host_allocation(ptr, &offset);
    if (allocation != nullptr) {
        out->kind = 1;
        out->host_pointer = const_cast<void*>(ptr);
        out->device_pointer =
            reinterpret_cast<void*>(static_cast<uintptr_t>(allocation->mem.va + offset));
        out->base = reinterpret_cast<uintptr_t>(allocation->mem.host);
        out->bytes = allocation->mem.bytes;
        out->flags = allocation->mem.flags;
    }
    return hipSuccess;
}

int symbol_address(void** device_pointer, size_t* bytes, const void* host_symbol) {
    if (device_pointer == nullptr || host_symbol == nullptr) {
        return fail(hipErrorInvalidValue);
    }
    Guard guard;
    for (Module* module : state().modules) {
        for (const bc250hip::Var& var : module->vars) {
            if (var.host_var != host_symbol) {
                continue;
            }
            const bc250hsa_status load = bc250hip::module_ensure_loaded(module);
            if (load != BC250HSA_OK) {
                return bc250hip::fail_status(load);
            }
            // The load resolved the variables of this module; read the record again.
            for (const bc250hip::Var& again : module->vars) {
                if (again.host_var == host_symbol && again.resolved) {
                    *device_pointer = reinterpret_cast<void*>(static_cast<uintptr_t>(again.va));
                    if (bytes != nullptr) {
                        *bytes = again.bytes;
                    }
                    return hipSuccess;
                }
            }
            return fail(kErrorInvalidSymbol);
        }
    }
    return fail(kErrorInvalidSymbol);
}

int counters(Counters* out) {
    if (out == nullptr || out->struct_bytes != sizeof(Counters)) {
        return fail(hipErrorInvalidValue);
    }
    Guard guard;
    const bc250hip::FaceCounters& f = state().face;
    out->kpack_modules = f.kpack_modules;
    out->kpack_loads = f.kpack_loads;
    out->kpack_refusals = f.kpack_refusals;
    out->kpack_library_missing = f.kpack_library_missing;
    out->bundle_alias_picks = f.bundle_alias_picks;
    out->alias_loads = f.alias_loads;
    out->compressed_refusals = f.compressed_refusals;
    out->program_loads = f.program_loads;
    out->program_refusals = f.program_refusals;
    out->module_launches = f.module_launches;
    out->packed_launches = f.packed_launches;
    out->last_kpack_error = f.last_kpack_error;
    return hipSuccess;
}

}  // namespace bc250face
