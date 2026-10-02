// The adapter's object index (wddm.c): O(1) membership of a handle value among the live DDI objects.
#pragma once

// Every object is in this index from the moment it joins the adapter's object list until it leaves it, both
// changes inside the same critical section under BC250_WDDM::Lock, and every lookup holds that lock too. The
// index has no lock of its own and allocates nothing: the bucket array is allocated and freed with the adapter
// state. It replaces full scans of the object list on the CloseAllocation, Present and OpenAllocation paths,
// which cost 10 ms an allocation at 43,000 live objects (a Vulkan CTS sparse process) and about 0.7 ms a frame
// at 4,200 (Witcher 3), each list node a cache miss in GDDR6.
//
// Review 16: a key that came out of an array dxgkrnl filled is compared as a value only. A lookup reads the
// chain links of objects already in the index and never the memory the key points at; the caller reads the
// object only after the lookup returned it, still under the lock.
//
// Intrusive and chained: the bucket heads and every object's chain link hold object addresses, so the test is
// literally candidate == key. Pool blocks are 16-byte aligned and the low four bits of a key carry nothing;
// Fibonacci hashing keeps the top bits of the product, which spreads both neighbouring pool slots and
// page-strided addresses. Host-compiled by driver/kmd/test/object_index_test.c.

#define BC250_OBJECT_INDEX_BITS 15u
#define BC250_OBJECT_INDEX_BUCKETS (1u << BC250_OBJECT_INDEX_BITS)
#define BC250_OBJECT_INDEX_BYTES (BC250_OBJECT_INDEX_BUCKETS * sizeof(void*))    // 256 KB on x64

typedef struct _BC250_OBJECT_INDEX {
    void** Buckets;                     // BC250_OBJECT_INDEX_BUCKETS chain heads, zeroed; NULL = no index
    unsigned long NextOffset;           // byte offset of the void* chain link inside every indexed object
    unsigned long Count;                // objects indexed now
    unsigned long Misses;               // removals of an object that was not indexed: never expected, counted
} BC250_OBJECT_INDEX;

static __inline unsigned long Bc250ObjectIndexBucket(const void* Key)
{
    return (unsigned long)((((unsigned long long)Key >> 4) * 0x9E3779B97F4A7C15ull) >> (64u - BC250_OBJECT_INDEX_BITS));
}

static __inline void** Bc250ObjectIndexNext(const BC250_OBJECT_INDEX* Index, void* Object)
{
    return (void**)((char*)Object + Index->NextOffset);
}

// Buckets: BC250_OBJECT_INDEX_BYTES of zeroes (ExAllocatePool2 zeroes what it returns).
static __inline void Bc250ObjectIndexInit(BC250_OBJECT_INDEX* Index, void** Buckets, unsigned long NextOffset)
{
    Index->Buckets = Buckets;
    Index->NextOffset = NextOffset;
    Index->Count = 0;
    Index->Misses = 0;
}

// Object: not indexed (it was just allocated). Caller holds the lock.
static __inline void Bc250ObjectIndexInsert(BC250_OBJECT_INDEX* Index, void* Object)
{
    void** head = &Index->Buckets[Bc250ObjectIndexBucket(Object)];

    *Bc250ObjectIndexNext(Index, Object) = *head;
    *head = Object;
    Index->Count++;
}

// 1 if the object was indexed and is not any more, 0 (and a miss counted) if it was not indexed. Caller holds
// the lock.
static __inline int Bc250ObjectIndexRemove(BC250_OBJECT_INDEX* Index, void* Object)
{
    void** link;

    for (link = &Index->Buckets[Bc250ObjectIndexBucket(Object)]; *link != 0; link = Bc250ObjectIndexNext(Index, *link))
    {
        if (*link == Object)
        {
            *link = *Bc250ObjectIndexNext(Index, Object);
            *Bc250ObjectIndexNext(Index, Object) = 0;
            Index->Count--;
            return 1;
        }
    }
    Index->Misses++;
    return 0;
}

// The indexed object whose address equals Key, or NULL. Key is never dereferenced. Caller holds the lock and
// reads the object only while it still does.
static __inline void* Bc250ObjectIndexFind(const BC250_OBJECT_INDEX* Index, const void* Key)
{
    void* object;

    if (Key == 0 || Index->Buckets == 0) return 0;
    for (object = Index->Buckets[Bc250ObjectIndexBucket(Key)]; object != 0; object = *Bc250ObjectIndexNext(Index, object))
        if (object == Key) return object;
    return 0;
}
