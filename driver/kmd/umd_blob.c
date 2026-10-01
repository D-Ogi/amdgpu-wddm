// See umd_blob.h. Little-endian reads, explicit, so a short or odd buffer cannot be a torn word and
// this file needs no header beyond its own.
#include "umd_blob.h"

static unsigned long Rd32(const unsigned char* p)
{
    return (unsigned long)p[0]
        | ((unsigned long)p[1] << 8)
        | ((unsigned long)p[2] << 16)
        | ((unsigned long)p[3] << 24);
}

static unsigned long long Rd64(const unsigned char* p)
{
    return (unsigned long long)Rd32(p) | ((unsigned long long)Rd32(p + 4) << 32);
}

const char* UmdBlobStatusText(int status)
{
    switch (status)
    {
    case UMD_BLOB_OK: return "ok";
    case UMD_BLOB_TOO_SMALL: return "too small";
    case UMD_BLOB_BAD_MAGIC: return "bad magic";
    case UMD_BLOB_BAD_VERSION: return "bad version";
    case UMD_BLOB_BAD_SIZE: return "bad size";
    case UMD_BLOB_BAD_HEAP: return "bad heap";
    case UMD_BLOB_BAD_FLAGS: return "bad flags";
    case UMD_BLOB_BAD_VA: return "bad va";
    case UMD_BLOB_BAD_IP: return "bad ip";
    case UMD_BLOB_BAD_NODE: return "bad node";
    case UMD_BLOB_BAD_IB: return "bad ib";
    default: return "unknown";
    }
}

int UmdBlobIsAlloc(const void* bytes, unsigned len)
{
    if (bytes == 0 || len < 4) return 0;
    return Rd32((const unsigned char*)bytes) == UMD_BLOB_ALLOC_MAGIC;
}

unsigned long UmdBlobFirstWord(const void* bytes, unsigned len)
{
    if (bytes == 0 || len < 4) return 0;
    return Rd32((const unsigned char*)bytes);
}

static void ClearAlloc(struct umd_alloc_view* out)
{
    if (out == 0) return;
    out->bytes = 0;
    out->alignment = 0;
    out->requested_va = 0;
    out->heap = 0;
    out->flags = 0;
    out->version = 0;
    out->exact_va = 0;
    out->gem_flags = 0;
    out->cache_policy_valid = 0;
}

static void ClearContext(struct umd_context_view* out)
{
    if (out == 0) return;
    out->ip_type = 0;
    out->node_ordinal = 0;
    out->priority = 0;
}

static void ClearSubmit(struct umd_submit_view* out)
{
    if (out == 0) return;
    out->num_ibs = 0;
    out->ib_va = 0;
    out->ib_bytes = 0;
    out->single_ib = 0;
}

int UmdBlobParseAlloc(const void* bytes, unsigned len, struct umd_alloc_view* out)
{
    const unsigned char* p = (const unsigned char*)bytes;
    unsigned long magic, version, size, flags, heap;
    unsigned long long allocSize, align, requested;

    ClearAlloc(out);
    if (p == 0 || len < UMD_BLOB_ALLOC_BYTES) return UMD_BLOB_TOO_SMALL;
    magic = Rd32(p + 0);
    version = Rd32(p + 4);
    size = Rd32(p + 8);
    if (magic != UMD_BLOB_ALLOC_MAGIC) return UMD_BLOB_BAD_MAGIC;
    if (version < 1) return UMD_BLOB_BAD_VERSION;
    // A later version may be longer. Everything this reader knows is in the first version's bytes,
    // and only when the blob claims to be at least that long and the buffer actually holds the claim.
    if (size < UMD_BLOB_ALLOC_BYTES || len < size) return UMD_BLOB_BAD_SIZE;
    flags = Rd32(p + 12);
    allocSize = Rd64(p + 16);
    align = Rd64(p + 24);
    heap = Rd32(p + 32);
    requested = Rd64(p + 48);
    if (allocSize == 0 || allocSize > UMD_BLOB_ALLOC_MAX) return UMD_BLOB_BAD_SIZE;
    if (heap != UMD_BLOB_HEAP_GTT && heap != UMD_BLOB_HEAP_VRAM) return UMD_BLOB_BAD_HEAP;
    // Sparse and userptr are a different allocation, not a flag to ignore. Exact-VA with no address
    // cannot be honoured, so it is a refusal rather than "any VA".
    if ((flags & (UMD_BLOB_A_SPARSE | UMD_BLOB_A_USERPTR)) != 0) return UMD_BLOB_BAD_FLAGS;
    if ((flags & UMD_BLOB_A_EXACT_VA) != 0 && requested == 0) return UMD_BLOB_BAD_VA;
    if (out != 0)
    {
        out->bytes = allocSize;
        out->alignment = align;
        out->requested_va = requested;
        out->heap = heap;
        out->flags = flags;
        out->version = version;
        out->gem_flags = Rd64(p + 40);
        out->cache_policy_valid = version >= 2;
        out->exact_va = (flags & UMD_BLOB_A_EXACT_VA) != 0;
    }
    return UMD_BLOB_OK;
}

int UmdBlobAllocCpuCached(const struct umd_alloc_view* allocation)
{
    return allocation != 0 && allocation->cache_policy_valid && allocation->heap == UMD_BLOB_HEAP_GTT &&
        (allocation->gem_flags & (UMD_BLOB_GEM_NO_CPU_ACCESS | UMD_BLOB_GEM_GTT_USWC)) == 0;
}

int UmdBlobParseContext(const void* bytes, unsigned len, unsigned ddi_node, struct umd_context_view* out)
{
    const unsigned char* p = (const unsigned char*)bytes;
    unsigned long magic, version, size, ip, instance, ring, priority, node;

    ClearContext(out);
    // Twelve bytes is the envelope. Version 1 is only 64 and has no node_ordinal, so requiring the
    // version-2 length before reading the version would hide that refusal behind "too small".
    if (p == 0 || len < 12) return UMD_BLOB_TOO_SMALL;
    magic = Rd32(p + 0);
    version = Rd32(p + 4);
    size = Rd32(p + 8);
    if (magic != UMD_BLOB_CONTEXT_MAGIC) return UMD_BLOB_BAD_MAGIC;
    // Version 1 has no node_ordinal. There is no writer of it, and a context without the cross-check
    // is the bug the field was added to catch, so version 1 is refused rather than accepted blindly.
    if (version < 2) return UMD_BLOB_BAD_VERSION;
    if (size < UMD_BLOB_CONTEXT_BYTES || len < size || len < UMD_BLOB_CONTEXT_BYTES) return UMD_BLOB_BAD_SIZE;
    ip = Rd32(p + 16);
    instance = Rd32(p + 20);
    ring = Rd32(p + 24);
    priority = Rd32(p + 28);
    node = Rd32(p + UMD_BLOB_CONTEXT_V1);
    if (ip != UMD_BLOB_IP_GFX || instance != 0 || ring != 0) return UMD_BLOB_BAD_IP;
    if (node != ddi_node || node != UMD_BLOB_NODE_3D) return UMD_BLOB_BAD_NODE;
    if (out != 0)
    {
        out->ip_type = ip;
        out->node_ordinal = node;
        out->priority = priority;
    }
    return UMD_BLOB_OK;
}

int UmdBlobParseSubmit(const void* bytes, unsigned len, struct umd_submit_view* out)
{
    const unsigned char* p = (const unsigned char*)bytes;
    unsigned long magic, version, size, ip, num, i, expect;
    unsigned long long firstVa = 0;
    unsigned long firstBytes = 0;

    ClearSubmit(out);
    if (p == 0 || len < UMD_BLOB_SUBMIT_PREFIX) return UMD_BLOB_TOO_SMALL;
    magic = Rd32(p + 0);
    version = Rd32(p + 4);
    size = Rd32(p + 8);
    ip = Rd32(p + 16);
    num = Rd32(p + 20);
    if (magic != UMD_BLOB_SUBMIT_MAGIC) return UMD_BLOB_BAD_MAGIC;
    if (version < 1) return UMD_BLOB_BAD_VERSION;
    if (num < 1 || num > UMD_BLOB_SUBMIT_MAX_IBS) return UMD_BLOB_BAD_SIZE;
    expect = UMD_BLOB_SUBMIT_PREFIX + num * UMD_BLOB_IB_BYTES;
    // Equality, not ">=". The contract's size word is the used prefix, not the reserved tail of the
    // struct, so a winsys that writes sizeof(the whole struct) is a malformed blob and not a submit.
    if (size != expect || len < size) return UMD_BLOB_BAD_SIZE;
    if (ip != UMD_BLOB_IP_GFX) return UMD_BLOB_BAD_IP;
    for (i = 0; i < num; i++)
    {
        const unsigned char* ib = p + UMD_BLOB_SUBMIT_PREFIX + i * UMD_BLOB_IB_BYTES;
        unsigned long long va = Rd64(ib + 0);
        unsigned long bytesIb = Rd32(ib + 8);
        unsigned long ipIb = Rd32(ib + 12);
        unsigned long instance = Rd32(ib + 16);
        unsigned long ring = Rd32(ib + 20);

        if (va == 0 || (va & 3ull) != 0) return UMD_BLOB_BAD_IB;
        if (bytesIb == 0 || (bytesIb & 3ul) != 0) return UMD_BLOB_BAD_IB;
        if (ipIb != UMD_BLOB_IP_GFX || instance != 0 || ring != 0) return UMD_BLOB_BAD_IB;
        if (i == 0) { firstVa = va; firstBytes = bytesIb; }
    }
    if (out != 0)
    {
        out->num_ibs = num;
        out->ib_va = firstVa;
        out->ib_bytes = firstBytes;
        out->single_ib = (num == 1);
    }
    return UMD_BLOB_OK;
}
