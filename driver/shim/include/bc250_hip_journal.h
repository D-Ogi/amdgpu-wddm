/* SPDX-License-Identifier: MIT */
#ifndef BC250_HIP_JOURNAL_H
#define BC250_HIP_JOURNAL_H

/* Diagnostic bytes only. No embedded GPU/user address is dereferenced by KMD.
 * All offsets below are relative to the start of their dispatch record.
 * Each record is canonical: fixed header, NUL-terminated symbol, zero padding
 * to 8, kernarg bytes, zero padding to 8, bindings. Bytes ends at that array.
 * Upload must succeed before the corresponding GPU submission is accepted.
 * Escape hContext selects the caller-owned BC2C context; flags are exactly
 * NoAdapterSynchronization (8), without HardwareAccess. No read API.
 */
#define BC250_HIP_JOURNAL_MAGIC 0x30353242u
#define BC250_HIP_JOURNAL_COMMAND 35u
#define BC250_HIP_JOURNAL_ABI 1u
#define BC250_HIP_JOURNAL_MAX_BYTES 131072u
#define BC250_HIP_JOURNAL_MAX_RECORDS 32u
#define BC250_HIP_JOURNAL_MAX_RECORD_BYTES 32768u
#define BC250_HIP_JOURNAL_MAX_SYMBOL 4096u
#define BC250_HIP_JOURNAL_MAX_KERNARG 16384u
#define BC250_HIP_JOURNAL_MAX_BINDINGS 128u
#define BC250_HIP_JOURNAL_SLOTS 32u
#define BC250_HIP_JOURNAL_UPLOAD_OP 0u
#define BC250_HIP_JOURNAL_CANCEL_OP 1u
#define BC250_HIP_BINDING_GLOBAL_BUFFER 1u
#define BC250_HIP_BINDING_INTERVAL_KNOWN 1u

typedef unsigned int BC250_HIP_U32;
typedef unsigned long long BC250_HIP_U64;

typedef struct BC250_HIP_JOURNAL_UPLOAD {
    BC250_HIP_U32 Magic, Command, Status, Version;
    BC250_HIP_U32 NtStatus, AbiVersion, TotalBytes, RecordCount;
    BC250_HIP_U64 IbVa, FenceVa, FenceValue, UploadId;
    BC250_HIP_U32 Operation, Reserved[3];
} BC250_HIP_JOURNAL_UPLOAD;

typedef struct BC250_HIP_DISPATCH_RECORD {
    BC250_HIP_U32 Bytes, SymbolBytes, KernargBytes, BindingCount;
    BC250_HIP_U64 EntryVa, DescriptorVa, KernargVa;
    BC250_HIP_U32 Grid[3], Block[3], DynamicLdsBytes, Flags;
    BC250_HIP_U32 SymbolOffset, KernargOffset, BindingsOffset, Reserved;
    BC250_HIP_U64 DispatchId;
} BC250_HIP_DISPATCH_RECORD;

typedef struct BC250_HIP_POINTER_BINDING {
    BC250_HIP_U32 ArgumentIndex, ArgumentOffset, Kind, Flags;
    BC250_HIP_U64 Value, Base, Bytes;
} BC250_HIP_POINTER_BINDING;

typedef char BC250_HIP_UPLOAD_SIZE[(sizeof(BC250_HIP_JOURNAL_UPLOAD) == 80) ? 1 : -1];
typedef char BC250_HIP_RECORD_SIZE[(sizeof(BC250_HIP_DISPATCH_RECORD) == 96) ? 1 : -1];
typedef char BC250_HIP_BINDING_SIZE[(sizeof(BC250_HIP_POINTER_BINDING) == 40) ? 1 : -1];

/* No alignment assumptions, allocation, OS dependencies or address dereferences.
 * The same validator runs before PM4 mutation in user mode and before retention
 * in KMD. This establishes record shape, never authenticity of diagnostic data.
 */
#ifdef _MSC_VER
#define BC250_HIP_INLINE static __inline
#else
#define BC250_HIP_INLINE static inline
#endif
BC250_HIP_INLINE void Bc250HipCopyBytes(void* destination, const void* source, BC250_HIP_U32 bytes)
{
    BC250_HIP_U32 i;
    for (i = 0; i < bytes; ++i)
        ((unsigned char*)destination)[i] = ((const unsigned char*)source)[i];
}
BC250_HIP_INLINE int Bc250HipRecordValid(const void* data, BC250_HIP_U32 bytes)
{
    BC250_HIP_DISPATCH_RECORD r;
    const unsigned char* p = (const unsigned char*)data;
    BC250_HIP_U32 i, k, expected, previous = 0;
    if (!p || bytes < sizeof(r) || bytes > BC250_HIP_JOURNAL_MAX_RECORD_BYTES) return 0;
    Bc250HipCopyBytes(&r, p, sizeof(r));
    if (r.Bytes != bytes || r.SymbolBytes < 2 || r.SymbolBytes > BC250_HIP_JOURNAL_MAX_SYMBOL ||
        r.KernargBytes > BC250_HIP_JOURNAL_MAX_KERNARG || r.BindingCount > BC250_HIP_JOURNAL_MAX_BINDINGS ||
        !r.EntryVa || !r.DescriptorVa || (!r.KernargVa && r.KernargBytes) || !r.DispatchId ||
        r.Flags || r.Reserved || r.SymbolOffset != sizeof(r)) return 0;
    for (i = 0; i < 3; ++i) if (!r.Grid[i] || !r.Block[i]) return 0;
    expected = (r.SymbolOffset + r.SymbolBytes + 7u) & ~7u;
    if (r.KernargOffset != expected) return 0;
    expected = (r.KernargOffset + r.KernargBytes + 7u) & ~7u;
    if (r.BindingsOffset != expected || bytes != expected + r.BindingCount * sizeof(BC250_HIP_POINTER_BINDING))
        return 0;
    for (i = r.SymbolOffset; i + 1 < r.SymbolOffset + r.SymbolBytes; ++i) if (!p[i]) return 0;
    if (p[r.SymbolOffset + r.SymbolBytes - 1]) return 0;
    for (i = r.SymbolOffset + r.SymbolBytes; i < r.KernargOffset; ++i) if (p[i]) return 0;
    for (i = r.KernargOffset + r.KernargBytes; i < r.BindingsOffset; ++i) if (p[i]) return 0;
    for (i = 0; i < r.BindingCount; ++i) {
        BC250_HIP_POINTER_BINDING b;
        BC250_HIP_U64 value = 0;
        Bc250HipCopyBytes(&b, p + r.BindingsOffset + i * sizeof(b), sizeof(b));
        if (b.Kind != BC250_HIP_BINDING_GLOBAL_BUFFER || b.Flags > BC250_HIP_BINDING_INTERVAL_KNOWN ||
            (i && b.ArgumentIndex <= previous) || r.KernargBytes < 8 || b.ArgumentOffset > r.KernargBytes - 8)
            return 0;
        previous = b.ArgumentIndex;
        for (k = 0; k < 8; ++k) value |= (BC250_HIP_U64)p[r.KernargOffset + b.ArgumentOffset + k] << (k * 8);
        if (value != b.Value) return 0;
        if (b.Flags == BC250_HIP_BINDING_INTERVAL_KNOWN) {
            if (!b.Bytes || b.Base > ~(BC250_HIP_U64)0 - b.Bytes || b.Value < b.Base || b.Value - b.Base >= b.Bytes)
                return 0;
        } else if (b.Base || b.Bytes) return 0;
    }
    return 1;
}
#undef BC250_HIP_INLINE

#endif
