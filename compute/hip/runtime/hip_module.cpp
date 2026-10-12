// hip_module.cpp - the HIP registration interface that clang's module constructor calls.
//
// Design docs/design/m16-hip-route-b.md section 4.2. MEASURED facts that this file depends on:
//   - the 24 bytes of .hipFatBinSegment are { int magic; int version; void* gpu_binary;
//     void* unused; }, magic 0x48495046 ("HIPF"), version 1, a null fourth field;
//   - __hipRegisterFatBinary receives the address of that wrapper and not the fat binary;
//   - the bundle inside .hip_fatbin starts with "__CLANG_OFFLOAD_BUNDLE__", a 64-bit entry
//     count, and one (offset, size, id length, id) record per entry;
//   - the host entry of a HIP bundle has size 0, and both entries can name the same offset.
//
// The bundle is read here, so that a wrong target is reported at registration. The code object
// is loaded on the first launch, so a process that starts no kernel pays nothing.

#include <cstdio>
#include <cstring>

#include "runtime_internal.h"

namespace {

constexpr uint32_t kHipFatBinMagic = 0x48495046u;  // 'HIPF'
constexpr uint32_t kCudaFatBinMagic = 0x466243B1u;
constexpr char kBundleMagic[] = "__CLANG_OFFLOAD_BUNDLE__";
constexpr size_t kBundleMagicBytes = sizeof(kBundleMagic) - 1;
constexpr uint64_t kMaxBundleEntries = 64;
constexpr uint64_t kMaxBundleBytes = 64ull * 1024ull * 1024ull;

struct FatBinWrapper {
    int32_t  magic;
    int32_t  version;
    const void* gpu_binary;
    const void* unused;
};

uint64_t read_u64(const unsigned char* p) {
    uint64_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

// The size of the bundle, from its own header. The wrapper gives a pointer and no length, so
// the reader must find the end itself: it is the largest offset plus size of any entry, and
// never less than the header. The caps keep the result bounded, but they cannot make the walk
// safe: the entry table is read before any cap can refuse it, so a wrapper that names a wrong
// or freed image can send this function up to 64 x (24 + 1024) bytes past the pointer. Only
// clang's own module constructor passes this pointer, and it passes the image of the same
// binary, which is the whole defence we have.
bool bundle_extent(const void* blob, size_t* out_bytes) {
    const unsigned char* p = static_cast<const unsigned char*>(blob);
    if (std::memcmp(p, kBundleMagic, kBundleMagicBytes) != 0) {
        return false;
    }
    const uint64_t count = read_u64(p + kBundleMagicBytes);
    if (count == 0 || count > kMaxBundleEntries) {
        return false;
    }
    uint64_t cursor = kBundleMagicBytes + 8;
    uint64_t end = cursor;
    for (uint64_t i = 0; i < count; ++i) {
        const uint64_t offset = read_u64(p + cursor);
        const uint64_t size = read_u64(p + cursor + 8);
        const uint64_t id_bytes = read_u64(p + cursor + 16);
        if (id_bytes > 1024 || offset > kMaxBundleBytes || size > kMaxBundleBytes) {
            return false;
        }
        cursor += 24 + id_bytes;
        if (cursor > end) {
            end = cursor;
        }
        if (offset + size > end) {
            end = offset + size;
        }
    }
    if (end > kMaxBundleBytes) {
        return false;
    }
    *out_bytes = static_cast<size_t>(end);
    return true;
}

void report(const char* message) {
    // One line on the error stream, and nothing else: a registration failure happens before
    // main() and a HIP program has no way to ask about it.
    std::fprintf(stderr, "amdhip64: %s\n", message);
}

}  // namespace

namespace bc250hip {

bc250hsa_status module_ensure_loaded(Module* module) {
    if (module->load_tried) {
        return module->load_status;
    }
    module->load_tried = true;
    if (module->bundle_status != BC250HSA_OK) {
        module->load_status = module->bundle_status;
        return module->load_status;
    }
    bc250hsa_device* dev = nullptr;
    if (device(&dev) != hipSuccess) {
        State& s = state();
        module->load_tried = false;  // a later call may find a device
        return s.open_status == BC250HSA_OK ? BC250HSA_ENODEV : s.open_status;
    }
    module->load_status =
        bc250hsa_module_load(dev, module->image, module->image_bytes, &module->loaded);
    if (module->load_status != BC250HSA_OK) {
        module->loaded = nullptr;
        return module->load_status;
    }
    // Resolve every device global that clang registered for this module.
    for (Var& var : module->vars) {
        uint64_t va = 0;
        uint64_t bytes = 0;
        if (bc250hsa_module_symbol(module->loaded, var.device_name.c_str(), &va, &bytes) ==
            BC250HSA_OK) {
            var.va = va;
            var.resolved = true;
        }
    }
    return BC250HSA_OK;
}

}  // namespace bc250hip

using bc250hip::Function;
using bc250hip::Module;
using bc250hip::state;
using bc250hip::Var;

extern "C" {

void** __hipRegisterFatBinary(const void* data) {
    if (data == nullptr) {
        report("__hipRegisterFatBinary received a null wrapper");
        return nullptr;
    }
    FatBinWrapper wrapper;
    std::memcpy(&wrapper, data, sizeof(wrapper));
    if (static_cast<uint32_t>(wrapper.magic) == kCudaFatBinMagic) {
        report("this is a CUDA fat binary, not a HIP one; compile with clang -x hip");
        return nullptr;
    }
    if (static_cast<uint32_t>(wrapper.magic) != kHipFatBinMagic) {
        report("the fat binary wrapper has no HIPF magic");
        return nullptr;
    }
    if (wrapper.gpu_binary == nullptr) {
        report("the fat binary wrapper names no device image");
        return nullptr;
    }

    bc250hip::Guard guard;
    bc250hip::State& s = state();
    const auto known = s.module_by_wrapper.find(data);
    if (known != s.module_by_wrapper.end()) {
        // Clang's module constructor registers one time per process, because it guards on the
        // null handle. A shared library of its own brings its own wrapper, so this path only
        // runs when the same wrapper arrives twice.
        known->second->registrations++;
        return reinterpret_cast<void**>(known->second);
    }

    Module* module = new Module();
    module->wrapper = data;
    module->registrations = 1;

    size_t bundle_bytes = 0;
    if (!bundle_extent(wrapper.gpu_binary, &bundle_bytes)) {
        module->bundle_status = BC250HSA_EBADBUNDLE;
        report("the device image is not a clang offload bundle");
    } else {
        module->bundle_status = bc250hsa_unbundle(wrapper.gpu_binary, bundle_bytes, nullptr,
                                                  &module->image, &module->image_bytes);
        if (module->bundle_status != BC250HSA_OK) {
            char line[160];
            std::snprintf(line, sizeof(line), "the offload bundle holds no usable gfx1013 code object (%s)",
                          bc250hsa_status_string(module->bundle_status));
            report(line);
        }
    }

    s.modules.push_back(module);
    s.module_by_wrapper.emplace(data, module);
    return reinterpret_cast<void**>(module);
}

void __hipUnregisterFatBinary(void** modules) {
    if (modules == nullptr) {
        return;
    }
    bc250hip::Guard guard;
    bc250hip::State& s = state();
    Module* module = reinterpret_cast<Module*>(modules);
    size_t at = s.modules.size();
    for (size_t i = 0; i < s.modules.size(); ++i) {
        if (s.modules[i] == module) {
            at = i;
            break;
        }
    }
    if (at == s.modules.size()) {
        return;
    }
    if (module->registrations > 1) {
        // The same wrapper is still registered. Take one registration away and keep the module
        // and its kernels: one unregister must not destroy what another registration uses.
        module->registrations--;
        return;
    }
    module->registrations = 0;
    s.modules.erase(s.modules.begin() + static_cast<ptrdiff_t>(at));
    if (module->wrapper != nullptr) {
        s.module_by_wrapper.erase(module->wrapper);
    }
    for (auto it = s.functions.begin(); it != s.functions.end();) {
        it = it->second.module == module ? s.functions.erase(it) : std::next(it);
    }
    if (module->loaded != nullptr) {
        bc250hsa_module_unload(module->loaded);
    }
    delete module;
}

int __hipRegisterFunction(void** modules, const void* hostFunction, char* deviceFunction,
                          const char* deviceName, int threadLimit, void* tid, void* bid,
                          void* blockDim, void* gridDim, int* workgroupSizeHint) {
    // MEASURED: clang passes the host stub, the device name twice, -1 and six null pointers.
    (void)deviceFunction;
    (void)threadLimit;
    (void)tid;
    (void)bid;
    (void)blockDim;
    (void)gridDim;
    (void)workgroupSizeHint;
    if (modules == nullptr || hostFunction == nullptr || deviceName == nullptr) {
        return 1;
    }
    bc250hip::Guard guard;
    Function entry;
    entry.module = reinterpret_cast<Module*>(modules);
    entry.device_name = deviceName;
    entry.kernel = nullptr;
    state().functions[hostFunction] = entry;
    return 0;
}

void __hipRegisterVar(void** modules, void* hostVar, char* deviceVar, const char* deviceName,
                      int isExtern, size_t size, int constant, int global) {
    (void)deviceVar;
    (void)isExtern;
    (void)constant;
    (void)global;
    if (modules == nullptr || deviceName == nullptr) {
        return;
    }
    bc250hip::Guard guard;
    Module* module = reinterpret_cast<Module*>(modules);
    Var var;
    var.host_var = hostVar;
    var.device_name = deviceName;
    var.bytes = size;
    module->vars.push_back(var);
}

void __hipRegisterManagedVar(void** modules, void* pointer, void* initValue, const char* name,
                             size_t size, unsigned align) {
    // Managed memory needs a page migration service, which this build does not have. The stub
    // reports the missing capability instead of pretending.
    (void)modules;
    (void)pointer;
    (void)initValue;
    (void)size;
    (void)align;
    char line[160];
    std::snprintf(line, sizeof(line), "managed variable '%s' needs managed memory, which this build does not support",
                  name != nullptr ? name : "?");
    report(line);
    bc250hip::log_line(BC250HSA_LOG_ERROR, "__hipRegisterManagedVar refuses variable '%s':"
                                           " managed memory needs page migration",
                       name != nullptr ? name : "?");
    bc250hip::last_error_set(hipErrorNotSupported);
}

}  // extern "C"
