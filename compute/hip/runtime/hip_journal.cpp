// Immutable host-side record packing. The caller holds the runtime state lock.
#include "runtime_internal.h"
#include "../../../driver/shim/include/bc250_hip_journal.h"
#include <cstring>
#include <new>

namespace bc250hip {
hipError_t prepare_dispatch_record(const bc250hsa_dispatch& dispatch,
                                  const void* packed, uint32_t packed_bytes,
                                  uint64_t dispatch_id,
                                  const std::map<uint64_t, Allocation>& allocations,
                                  std::vector<unsigned char>& output) {
    output.clear();
    const bc250hsa_kernel* kernel = dispatch.kernel;
    if (!kernel || !kernel->name || !dispatch_id ||
        (packed_bytes && !packed) || packed_bytes > BC250_HIP_JOURNAL_MAX_KERNARG ||
        (kernel->arg_count && !kernel->args)) return hipErrorNotSupported;
    size_t name_bytes = 0;
    while (name_bytes < BC250_HIP_JOURNAL_MAX_SYMBOL && kernel->name[name_bytes])
        ++name_bytes;
    if (name_bytes == BC250_HIP_JOURNAL_MAX_SYMBOL) return hipErrorNotSupported;
    ++name_bytes; // Include NUL; do not truncate a symbol.
    BC250_HIP_POINTER_BINDING bindings[BC250_HIP_JOURNAL_MAX_BINDINGS]{};
    uint32_t count = 0;
    for (uint32_t i = 0; i < kernel->arg_count; ++i) {
        const bc250hsa_arg& arg = kernel->args[i];
        if (arg.kind != BC250HSA_ARG_GLOBAL_BUFFER) continue;
        if (count == BC250_HIP_JOURNAL_MAX_BINDINGS || arg.size != 8 ||
            arg.offset > packed_bytes || packed_bytes - arg.offset < 8)
            return hipErrorNotSupported;
        auto& binding = bindings[count++];
        binding.ArgumentIndex = i;
        binding.ArgumentOffset = arg.offset;
        binding.Kind = BC250_HIP_BINDING_GLOBAL_BUFFER;
        std::memcpy(&binding.Value, static_cast<const unsigned char*>(packed) + arg.offset, 8);
        if (binding.Value) {
            auto interval = allocations.upper_bound(binding.Value);
            if (interval != allocations.begin()) {
                --interval;
                const auto& memory = interval->second.mem;
                if (memory.va == interval->first && binding.Value >= memory.va &&
                    binding.Value - memory.va < memory.bytes &&
                    memory.bytes <= UINT64_MAX - memory.va) {
                    binding.Flags = BC250_HIP_BINDING_INTERVAL_KNOWN;
                    binding.Base = memory.va;
                    binding.Bytes = memory.bytes;
                }
            }
        }
    }
    BC250_HIP_DISPATCH_RECORD record{};
    record.SymbolOffset = sizeof(record);
    record.SymbolBytes = static_cast<uint32_t>(name_bytes);
    record.KernargOffset = (record.SymbolOffset + record.SymbolBytes + 7u) & ~7u;
    record.KernargBytes = packed_bytes;
    record.BindingsOffset = (record.KernargOffset + packed_bytes + 7u) & ~7u;
    record.BindingCount = count;
    record.Bytes = record.BindingsOffset + count * sizeof(bindings[0]);
    if (record.Bytes > BC250_HIP_JOURNAL_MAX_RECORD_BYTES) return hipErrorNotSupported;
    record.EntryVa = kernel->entry_va;
    record.DescriptorVa = kernel->descriptor_va;
    record.KernargVa = dispatch.kernarg_va;
    record.DispatchId = dispatch_id;
    std::memcpy(record.Grid, dispatch.launch.grid, sizeof(record.Grid));
    std::memcpy(record.Block, dispatch.launch.block, sizeof(record.Block));
    record.DynamicLdsBytes = dispatch.launch.dynamic_group_bytes;
    try { output.resize(record.Bytes, 0); }
    catch (const std::bad_alloc&) { return hipErrorOutOfMemory; }
    std::memcpy(output.data(), &record, sizeof(record));
    std::memcpy(output.data() + record.SymbolOffset, kernel->name, name_bytes);
    if (packed_bytes) std::memcpy(output.data() + record.KernargOffset, packed, packed_bytes);
    if (count) std::memcpy(output.data() + record.BindingsOffset, bindings, count * sizeof(bindings[0]));
    return hipSuccess;
}
} // namespace bc250hip
