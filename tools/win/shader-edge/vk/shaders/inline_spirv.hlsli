// SPDX-License-Identifier: MIT
// Inline SPIR-V helpers of the Vulkan shader edge-case client. Technique from Erfan Ahmadi, "Inline SPIR-V"
// (https://erfan-ahmadi.github.io/blog/inline-spirv/): the DXC attributes vk::ext_instruction, vk::ext_reference,
// vk::ext_capability and vk::ext_extension make DXC emit one exact SPIR-V instruction for each call, so a test
// controls the scope and the memory semantics operands itself instead of relying on what an HLSL intrinsic lowers to.
//
// Only DXC with -spirv compiles this file. The opcode, scope and semantics numbers are from the SPIR-V 1.6
// specification, sections 3.27 (scope), 3.25 (memory semantics) and 3.49.18 (atomic instructions).
#ifndef INLINE_SPIRV_HLSLI
#define INLINE_SPIRV_HLSLI

// Scope operands (SPIR-V 3.27).
static const uint kScopeDevice = 1;
static const uint kScopeWorkgroup = 2;
static const uint kScopeSubgroup = 3;

// Memory semantics operands (SPIR-V 3.25): an ordering bit and the storage classes it applies to. UniformMemory
// (0x40) covers the StorageBuffer and PhysicalStorageBuffer storage classes.
static const uint kSemAcquire = 0x2;
static const uint kSemRelease = 0x4;
static const uint kSemAcquireRelease = 0x8;
static const uint kSemUniformMemory = 0x40;
static const uint kAcquireUniform = kSemAcquire | kSemUniformMemory;               // 0x42
static const uint kReleaseUniform = kSemRelease | kSemUniformMemory;               // 0x44
static const uint kAcqRelUniform = kSemAcquireRelease | kSemUniformMemory;         // 0x48

// Atomics on a variable of a storage buffer (descriptor route). vk::ext_reference passes the pointer of the
// argument, which must be a variable or a buffer element, never a temporary.
[[vk::ext_instruction(/*OpAtomicLoad*/ 227)]]
uint spvAtomicLoad([[vk::ext_reference]] uint ptr, uint scope, uint semantics);
[[vk::ext_instruction(/*OpAtomicStore*/ 228)]]
void spvAtomicStore([[vk::ext_reference]] uint ptr, uint scope, uint semantics, uint value);
[[vk::ext_instruction(/*OpAtomicCompareExchange*/ 230)]]
uint spvAtomicCompareExchange([[vk::ext_reference]] uint ptr, uint scope, uint equalSemantics,
                              uint unequalSemantics, uint value, uint comparator);
[[vk::ext_instruction(/*OpAtomicIAdd*/ 234)]]
uint spvAtomicIAdd([[vk::ext_reference]] uint ptr, uint scope, uint semantics, uint value);

// The same atomics through a buffer device address: an OpTypePointer PhysicalStorageBuffer %uint made with
// vk::SpirvType, and OpConvertUToPtr to make one from a 64-bit address. The pointer is a value here, so the
// atomic gets its id directly and needs no vk::ext_reference.
using PhysUint = vk::SpirvType</*OpTypePointer*/ 32, 8, 8,
                               vk::Literal<vk::integral_constant<uint, /*PhysicalStorageBuffer*/ 5349> >, uint>;
[[vk::ext_instruction(/*OpConvertUToPtr*/ 120)]]
PhysUint spvUintPointer(uint64_t address);
[[vk::ext_instruction(/*OpAtomicLoad*/ 227)]]
uint spvAtomicLoadPtr(PhysUint ptr, uint scope, uint semantics);
[[vk::ext_instruction(/*OpAtomicStore*/ 228)]]
void spvAtomicStorePtr(PhysUint ptr, uint scope, uint semantics, uint value);
[[vk::ext_instruction(/*OpAtomicCompareExchange*/ 230)]]
uint spvAtomicCompareExchangePtr(PhysUint ptr, uint scope, uint equalSemantics, uint unequalSemantics, uint value,
                                 uint comparator);
[[vk::ext_instruction(/*OpAtomicIAdd*/ 234)]]
uint spvAtomicIAddPtr(PhysUint ptr, uint scope, uint semantics, uint value);

// OpReadClockKHR from SPV_KHR_shader_clock: a 64-bit counter as two 32-bit words (low, high).
[[vk::ext_extension("SPV_KHR_shader_clock")]]
[[vk::ext_capability(/*ShaderClockKHR*/ 5055)]]
[[vk::ext_instruction(/*OpReadClockKHR*/ 5056)]]
uint2 spvReadClock(uint scope);

// One push constant block for every program of the client (vkshaderedge.c, struct Push). The addresses are the
// device addresses of the buffer regions a program uses; the counts are in elements.
struct Push {
    uint64_t a0;
    uint64_t a1;
    uint64_t a2;
    uint n;
    uint spin;
    uint neg;
    uint pad;
};
[[vk::push_constant]] Push pc;

#endif
