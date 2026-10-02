// Host test of driver/kmd/object_index.h, the adapter's O(1) object membership index (wddm.c).
// Insert, find, remove at the head, middle and tail of one chain, keys that are not indexed (some of them
// unmapped: a lookup that read through its key would fault here), miss accounting, a bulk run at the
// 43,000 live objects measured in a Vulkan CTS sparse process, and the spread of strided pool addresses.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../object_index.h"
#define REQUIRE(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static unsigned checks;

// The link sits away from offset 0, as BC250_WDDM_OBJECT's HashNext does behind its LIST_ENTRY.
typedef struct OBJECT { unsigned long long Head[3]; void* HashNext; unsigned long long Serial; unsigned long long Tail[5]; } OBJECT;
#define LIVE 43000u
#define POOL 200000u
static OBJECT pool[POOL];
static void* buckets[BC250_OBJECT_INDEX_BUCKETS];
static BC250_OBJECT_INDEX index_;
#define UNMAPPED_KERNEL ((const void*)(uintptr_t)0xFFFFF78000001230ull)
#define UNMAPPED_LOW ((const void*)(uintptr_t)0x10ull)

static void reset(void)
{
    memset(buckets, 0, sizeof(buckets));
    memset(pool, 0, sizeof(pool));
    Bc250ObjectIndexInit(&index_, buckets, (unsigned long)offsetof(OBJECT, HashNext));
}

static unsigned chain(unsigned long bucket)
{
    unsigned n = 0;
    void* object;
    for (object = buckets[bucket]; object; object = *Bc250ObjectIndexNext(&index_, object)) n++;
    return n;
}

static unsigned maxChainOf(uintptr_t base, uintptr_t stride, unsigned count)
{
    static unsigned short hits[BC250_OBJECT_INDEX_BUCKETS];
    unsigned i, worst = 0;
    memset(hits, 0, sizeof(hits));
    for (i = 0; i < count; i++) {
        unsigned long bucket = Bc250ObjectIndexBucket((const void*)(base + i * stride));
        REQUIRE(bucket < BC250_OBJECT_INDEX_BUCKETS);
        if (++hits[bucket] > worst) worst = hits[bucket];
    }
    return worst;
}

int main(void)
{
    unsigned i, found = 0, same[3];
    unsigned long bucket;

    // Empty, and without buckets at all.
    reset();
    REQUIRE(Bc250ObjectIndexFind(&index_, NULL) == NULL);
    REQUIRE(Bc250ObjectIndexFind(&index_, &pool[0]) == NULL);
    REQUIRE(Bc250ObjectIndexFind(&index_, UNMAPPED_KERNEL) == NULL);
    REQUIRE(Bc250ObjectIndexFind(&index_, UNMAPPED_LOW) == NULL);
    {
        BC250_OBJECT_INDEX none;
        Bc250ObjectIndexInit(&none, NULL, 0);
        REQUIRE(Bc250ObjectIndexFind(&none, &pool[0]) == NULL);
    }

    // One object: found by its own address only, never by a neighbouring value.
    Bc250ObjectIndexInsert(&index_, &pool[1]);
    REQUIRE(index_.Count == 1);
    REQUIRE(Bc250ObjectIndexFind(&index_, &pool[1]) == &pool[1]);
    REQUIRE(Bc250ObjectIndexFind(&index_, (const char*)&pool[1] + 1) == NULL);
    REQUIRE(Bc250ObjectIndexFind(&index_, (const char*)&pool[1] + 16) == NULL);
    REQUIRE(Bc250ObjectIndexFind(&index_, &pool[1].HashNext) == NULL);
    REQUIRE(Bc250ObjectIndexFind(&index_, &pool[2]) == NULL);
    REQUIRE(Bc250ObjectIndexRemove(&index_, &pool[1]) == 1);
    REQUIRE(index_.Count == 0 && index_.Misses == 0 && pool[1].HashNext == NULL);
    REQUIRE(Bc250ObjectIndexFind(&index_, &pool[1]) == NULL);
    REQUIRE(Bc250ObjectIndexRemove(&index_, &pool[1]) == 0 && index_.Misses == 1);

    // Four pool slots that share a bucket (200,000 slots in 32,768 buckets: some bucket has seven): three
    // indexed, one not.
    reset();
    {
        static unsigned short hits[BC250_OBJECT_INDEX_BUCKETS];
        unsigned outsider = POOL;
        memset(hits, 0, sizeof(hits));
        for (i = 0; i < POOL; i++) hits[Bc250ObjectIndexBucket(&pool[i])]++;
        for (bucket = 0; bucket < BC250_OBJECT_INDEX_BUCKETS && hits[bucket] < 4; bucket++) {}
        REQUIRE(bucket < BC250_OBJECT_INDEX_BUCKETS);
        for (i = 0; i < POOL && found < 3; i++) {
            if (Bc250ObjectIndexBucket(&pool[i]) != bucket) continue;
            if (outsider == POOL) outsider = i; else same[found++] = i;
        }
        REQUIRE(found == 3 && outsider < POOL);
        Bc250ObjectIndexInsert(&index_, &pool[same[0]]);
        Bc250ObjectIndexInsert(&index_, &pool[same[1]]);
        Bc250ObjectIndexInsert(&index_, &pool[same[2]]);
        REQUIRE(chain(bucket) == 3 && index_.Count == 3);
        REQUIRE(buckets[bucket] == &pool[same[2]]);                 // head insertion: 2 -> 1 -> 0
        for (i = 0; i < 3; i++) REQUIRE(Bc250ObjectIndexFind(&index_, &pool[same[i]]) == &pool[same[i]]);
        REQUIRE(Bc250ObjectIndexFind(&index_, &pool[outsider]) == NULL);   // same bucket, not indexed
        REQUIRE(Bc250ObjectIndexRemove(&index_, &pool[outsider]) == 0 && index_.Misses == 1 && chain(bucket) == 3);
    }
    REQUIRE(Bc250ObjectIndexRemove(&index_, &pool[same[1]]) == 1);  // middle
    REQUIRE(chain(bucket) == 2 && Bc250ObjectIndexFind(&index_, &pool[same[1]]) == NULL);
    REQUIRE(Bc250ObjectIndexFind(&index_, &pool[same[0]]) == &pool[same[0]]);
    REQUIRE(Bc250ObjectIndexFind(&index_, &pool[same[2]]) == &pool[same[2]]);
    REQUIRE(Bc250ObjectIndexRemove(&index_, &pool[same[2]]) == 1);  // head
    REQUIRE(chain(bucket) == 1 && buckets[bucket] == &pool[same[0]]);
    Bc250ObjectIndexInsert(&index_, &pool[same[1]]);               // reinsert ahead of the tail
    REQUIRE(Bc250ObjectIndexRemove(&index_, &pool[same[0]]) == 1);  // tail
    REQUIRE(chain(bucket) == 1 && buckets[bucket] == &pool[same[1]]);
    REQUIRE(Bc250ObjectIndexRemove(&index_, &pool[same[1]]) == 1);
    REQUIRE(buckets[bucket] == NULL && index_.Count == 0 && index_.Misses == 1);

    // Bulk at the measured CTS peak: every object found, removals exact, nothing left behind.
    reset();
    for (i = 0; i < LIVE; i++) Bc250ObjectIndexInsert(&index_, &pool[i]);
    REQUIRE(index_.Count == LIVE);
    for (i = 0; i < LIVE; i++) REQUIRE(Bc250ObjectIndexFind(&index_, &pool[i]) == &pool[i]);
    for (i = LIVE; i < POOL; i += 7) REQUIRE(Bc250ObjectIndexFind(&index_, &pool[i]) == NULL);
    for (i = 0; i < LIVE; i += 2) REQUIRE(Bc250ObjectIndexRemove(&index_, &pool[i]) == 1);
    for (i = 0; i < LIVE; i++)
        REQUIRE(Bc250ObjectIndexFind(&index_, &pool[i]) == ((i & 1) ? &pool[i] : NULL));
    for (i = 0; i < LIVE; i += 2) Bc250ObjectIndexInsert(&index_, &pool[i]);
    for (i = 0; i < LIVE; i++) REQUIRE(Bc250ObjectIndexFind(&index_, &pool[i]) == &pool[i]);
    {
        unsigned worst = 0, used = 0, n;
        for (bucket = 0; bucket < BC250_OBJECT_INDEX_BUCKETS; bucket++) {
            n = chain(bucket);
            if (n) used++;
            if (n > worst) worst = n;
        }
        printf("bulk: %u objects of %u bytes in %u buckets, longest chain %u\n", LIVE, (unsigned)sizeof(OBJECT), used, worst);
        REQUIRE(worst <= 6);
    }
    for (i = LIVE; i-- > 0;) REQUIRE(Bc250ObjectIndexRemove(&index_, &pool[i]) == 1);
    REQUIRE(index_.Count == 0 && index_.Misses == 0);
    for (bucket = 0; bucket < BC250_OBJECT_INDEX_BUCKETS; bucket++) REQUIRE(buckets[bucket] == NULL);

    // Spread of 43,000 pool-like addresses at the strides small blocks and pages come in.
    {
        static const uintptr_t strides[] = { 16, 32, 48, 64, 80, 128, 208, 256, 4096, 65536 };
        static const uintptr_t bases[] = { (uintptr_t)0xFFFFB28F12340000ull, (uintptr_t)0xFFFF800000000000ull, (uintptr_t)0x7FF600010000ull };
        unsigned s, t, worst = 0;
        for (t = 0; t < sizeof(bases) / sizeof(bases[0]); t++)
            for (s = 0; s < sizeof(strides) / sizeof(strides[0]); s++) {
                unsigned m = maxChainOf(bases[t], strides[s], LIVE);
                if (m > worst) worst = m;
            }
        printf("strided: longest chain %u over %u address sequences of %u\n", worst,
               (unsigned)(sizeof(bases) / sizeof(bases[0]) * sizeof(strides) / sizeof(strides[0])), LIVE);
        REQUIRE(worst <= 6);
    }

    printf("object_index PASS checks=%u\n", checks);
    return 0;
}
