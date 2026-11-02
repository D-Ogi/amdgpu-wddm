/* test_aggregate_alloc.c - BD-117, a device-local allocation above the VidMm limit of one
 * allocation, through the production code of layer 1 (kmt_memory.c, included whole by
 * test_copy_lifecycle.c with its fake kernel driver).
 *
 * The fake refuses CreateAllocation2 above 0xFFFF0000 bytes as unit A did, keeps GPU address
 * reservations, maps a child only at an address inside one, gives children above 64 MiB a
 * sparse host backing, and can fail the n-th call of any kind. The cases cover:
 *   - the 11484656026-byte PyTorch request: three children, one contiguous range, the exact
 *     reservation, offsets above 4 GiB, mixed heaps under a local budget;
 *   - small children (the private cap of this file) with real, disjoint host views: host to
 *     device, device to host, device to device across two different grids, fill and ZERO
 *     against a flat reference, and the GPU copy across children;
 *   - a failure at every call of every kind of the construction, the largest paging fence,
 *     no publication before the paging wait, a failed paging wait that keeps everything;
 *   - a release that fails part way, the refusals while it is retiring, the retry, and the
 *     drain at close;
 *   - planner refusals with no kernel call.
 */
#define BC250HSA_LIFECYCLE_NO_MAIN 1
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-function"
#elif defined(_MSC_VER)
#pragma warning(disable : 4505)
#endif
#include "test_copy_lifecycle.c"

static unsigned g_seed = 12345u;
static uint8_t next_byte(void)
{
    g_seed = g_seed * 1103515245u + 12345u;
    return (uint8_t)(g_seed >> 16);
}

static uint32_t live_allocs(void)
{
    uint32_t i, n = 0u;
    for (i = 0; i < MAX_ALLOCS; i++) { n += g_allocs[i].live ? 1u : 0u; }
    return n;
}

static uint32_t live_reservations(void)
{
    uint32_t i, n = 0u;
    for (i = 0; i < MAX_RESERVATIONS; i++) { n += g_reservations[i].live ? 1u : 0u; }
    return n;
}

static uint32_t record_count(const struct bc250hsa_device* d)
{
    uint32_t                     n = 0u;
    const bc250hsa_alloc_record* r;
    for (r = d->allocations; r != NULL; r = r->next) { ++n; }
    return n;
}

static const bc250hsa_alloc_record* rec(const bc250hsa_mem* m)
{
    return (const bc250hsa_alloc_record*)m->opaque;
}

static uint32_t access(struct bc250hsa_device* d, const bc250hsa_mem* m)
{
    uint32_t a = 0xFFu;
    CHECK(bc250hsa_memory_access(d, m, &a) == BC250HSA_OK);
    return a;
}

/* The construction invariants of one published aggregate. */
static void check_shape(const bc250hsa_mem* m, uint64_t want_bytes, uint32_t want_children,
                        uint64_t stride)
{
    const bc250hsa_alloc_record* r = rec(m);
    uint64_t                     at = 0u;
    uint32_t                     i;
    CHECK(r != NULL && r->kind == BC250HSA_RECORD_AGGREGATE && !r->retiring);
    if (r == NULL) { return; }
    CHECK(m->bytes == want_bytes && r->bytes == want_bytes);
    CHECK(r->child_count == want_children && r->child_stride == stride);
    CHECK(m->host == NULL && m->va == r->va);
    CHECK(r->reserve_base != 0u && m->va >= r->reserve_base);
    CHECK(m->va - r->reserve_base + m->bytes <= r->reserve_bytes);
    CHECK(reservation_holding(r->reserve_base, r->reserve_bytes) != NULL);
    for (i = 0; i < r->child_count; i++) {
        const bc250hsa_alloc_child* c = &r->children[i];
        const fake_alloc*           a = alloc_by_handle(c->handle);
        CHECK(c->offset == at);
        CHECK(c->bytes != 0u && c->bytes <= stride && c->bytes % 0x40000u == 0u);
        CHECK(i + 1u == r->child_count || c->bytes == stride);
        CHECK(c->va == m->va + c->offset);
        CHECK(a != NULL && a->va == c->va && a->va_live && a->bytes == c->bytes);
        CHECK(a != NULL && a->heap == c->heap);
        at += c->bytes;
    }
    CHECK(at == want_bytes);
}

/* Commits the pages of [host, host + bytes) of a sparse backing before a case touches them. */
static void fake_commit(void* host, uint64_t bytes)
{
    if (bytes == 0u) { return; }
    CHECK(VirtualAlloc(host, (SIZE_T)bytes, MEM_COMMIT, PAGE_READWRITE) != NULL);
}

/* Commits the host pages of [offset, offset + bytes) of every sparse child it touches. */
static void commit_range(const bc250hsa_mem* m, uint64_t offset, uint64_t bytes)
{
    const bc250hsa_alloc_record* r = rec(m);
    uint32_t                     i;
    for (i = 0; i < r->child_count; i++) {
        const bc250hsa_alloc_child* c = &r->children[i];
        const uint64_t              lo = offset > c->offset ? offset : c->offset;
        const uint64_t              hi = offset + bytes < c->offset + c->bytes ? offset + bytes
                                                                                : c->offset + c->bytes;
        if (lo < hi && c->host != NULL) {
            fake_commit((uint8_t*)c->host + (lo - c->offset), hi - lo);
        }
    }
}

static void case_pytorch_request(void)
{
    struct bc250hsa_device d;
    bc250hsa_mem           m;
    const uint64_t         request = 11484656026ull;
    const uint64_t         aligned = (request + 0x3FFFFu) & ~0x3FFFFull;
    const uint64_t         probes[] = { 0u, 0xFFFC0000ull - 3u, 0xFFFC0000ull * 2u - 5u,
                                        0x100000000ull + 17u, aligned - 16u };
    uint8_t                want[16], got[16];
    uint32_t               i, j, creates, budgeted;
    void*                  host = NULL;

    printf("aggregate: the PyTorch request of 11484656026 bytes\n");
    for (budgeted = 0; budgeted < 2u; budgeted++) {
        dev_init(&d);
        creates = g_creates;
        if (budgeted) {
            g_local_budget = 6ull << 30;   /* child 0 fits, then the carve-out is full */
        }
        memset(&m, 0xa5, sizeof(m));
        CHECK(bc250hsa_alloc(&d, request, 0u, BC250HSA_MEM_MAPPABLE, &m) == BC250HSA_OK);
        check_shape(&m, aligned, 3u, 0xFFFC0000ull);
        CHECK(g_creates - creates == 3u && g_reserves == 1u && g_must_succeed == 0u);
        CHECK(m.va % 0x40000u == 0u);
        CHECK(access(&d, &m) == BC250HSA_ACCESS_SCATTER);
        CHECK(bc250hsa_map(&d, &m, &host) == BC250HSA_EUNSUPPORTED && host == NULL);
        CHECK(bc250hsa_unmap(&d, &m) == BC250HSA_OK && rec(&m)->cpu_access == 1u);
        if (budgeted) {
            CHECK(rec(&m)->children[0].heap == BC250HSA_HEAP_VRAM);
            CHECK(rec(&m)->children[1].heap == BC250HSA_HEAP_GTT);
            CHECK(rec(&m)->children[2].heap == BC250HSA_HEAP_GTT);
        } else {
            for (i = 0; i < 3u; i++) { CHECK(rec(&m)->children[i].heap == BC250HSA_HEAP_VRAM); }
        }
        /* Writes and reads that cross both child boundaries, one above 4 GiB, and the last
         * bytes of the range. */
        for (i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
            commit_range(&m, probes[i], sizeof(want));
            for (j = 0; j < sizeof(want); j++) { want[j] = next_byte(); }
            CHECK(bc250hsa_copy_to_device(&d, &m, probes[i], want, sizeof(want)) == BC250HSA_OK);
            memset(got, 0, sizeof(got));
            CHECK(bc250hsa_copy_from_device(&d, got, &m, probes[i], sizeof(got)) == BC250HSA_OK);
            CHECK(memcmp(want, got, sizeof(want)) == 0);
        }
        /* The child view under a probe holds the byte at its own offset, so a walker that
         * wrote everything into the first child would fail here. */
        {
            const uint64_t              off = 0x100000000ull + 17u;
            const bc250hsa_alloc_child* c2 = &rec(&m)->children[off / 0xFFFC0000ull];
            CHECK(off / 0xFFFC0000ull == 1u && off >= c2->offset);
            CHECK(bc250hsa_copy_from_device(&d, got, &m, off, 1u) == BC250HSA_OK);
            CHECK(((const uint8_t*)c2->host)[off - c2->offset] == got[0]);
        }
        CHECK(bc250hsa_copy_to_device(&d, &m, aligned - 15u, want, 16u) == BC250HSA_EINVAL);
        CHECK(bc250hsa_copy_from_device(&d, got, &m, aligned, 1u) == BC250HSA_EINVAL);
        CHECK(bc250hsa_free(&d, &m) == BC250HSA_OK);
        CHECK(m.opaque == NULL && m.va == 0u);
        CHECK(live_allocs() == 0u && live_reservations() == 0u && d.allocations == NULL);
        CHECK(g_reservation_frees == 1u && g_bad_frees == 0u && g_bad_destroys == 0u);
        CHECK(g_unlocks - g_unlocks_at_init == g_locks);
        dev_finish(&d, 0);
    }
}

/* The flat reference model of one allocation. */
typedef struct model {
    bc250hsa_mem m;
    uint8_t*     ref;
    uint64_t     bytes;
} model;

static int model_alloc(struct bc250hsa_device* d, model* x, uint64_t bytes, uint64_t cap,
                       uint32_t flags)
{
    g_aggregate_child_cap = cap;
    memset(&x->m, 0, sizeof(x->m));
    if (bc250hsa_alloc(d, bytes, 0u, flags, &x->m) != BC250HSA_OK) { return 0; }
    x->bytes = x->m.bytes;
    x->ref = (uint8_t*)calloc(1, (size_t)x->bytes);
    g_aggregate_child_cap = BC250HSA_ALLOCATION_CHILD_MAX;
    return x->ref != NULL;
}

static int model_same(struct bc250hsa_device* d, const model* x)
{
    uint8_t* back = (uint8_t*)malloc((size_t)x->bytes);
    int      same;
    if (back == NULL) { return 0; }
    same = bc250hsa_copy_from_device(d, back, &x->m, 0u, x->bytes) == BC250HSA_OK &&
           memcmp(back, x->ref, (size_t)x->bytes) == 0;
    free(back);
    return same;
}

static void case_scatter_walkers(void)
{
    struct bc250hsa_device d;
    model                  a, b, h;
    uint8_t                buf[0x90000];
    /* Offsets and lengths around the 256 KiB grid of a and the 512 KiB grid of b. */
    static const uint64_t  offs[] = { 0u, 1u, 0x3FFFFu, 0x40000u, 0x7FFFFu, 0x80001u, 0xBFFF0u,
                                      0x100000u };
    static const uint64_t  lens[] = { 1u, 2u, 0x10u, 0x40000u, 0x40001u, 0x80000u };
    uint64_t               i, j, k;
    uint32_t               access_a;

    printf("aggregate: scatter copies and fills against a flat reference\n");
    dev_init(&d);
    CHECK(model_alloc(&d, &a, 0x100001u, 0x40000u, BC250HSA_MEM_MAPPABLE | BC250HSA_MEM_ZERO));
    CHECK(model_alloc(&d, &b, 0x140000u, 0x80000u, BC250HSA_MEM_MAPPABLE));
    CHECK(model_alloc(&d, &h, 0x140000u, 0x80000u, BC250HSA_MEM_HOST));
    check_shape(&a.m, 0x140000u, 5u, 0x40000u);
    check_shape(&b.m, 0x140000u, 3u, 0x80000u);
    CHECK(rec(&h.m)->kind == BC250HSA_RECORD_SINGLE && h.m.host != NULL);
    access_a = access(&d, &a.m);
    CHECK(access_a == BC250HSA_ACCESS_SCATTER && access(&d, &h.m) == BC250HSA_ACCESS_DIRECT);
    CHECK(model_same(&d, &a));     /* ZERO walked every child */
    memcpy(b.ref, h.m.host, 0u);   /* b starts unknown: write all of it first */
    for (i = 0; i < b.bytes; i++) { b.ref[i] = next_byte(); }
    CHECK(bc250hsa_copy_to_device(&d, &b.m, 0u, b.ref, b.bytes) == BC250HSA_OK);
    memset(h.m.host, 0, (size_t)h.bytes);
    CHECK(model_same(&d, &b) && model_same(&d, &h));

    for (i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
        for (j = 0; j < sizeof(lens) / sizeof(lens[0]); j++) {
            const uint64_t o = offs[i], n = lens[j];
            const uint64_t o2 = offs[(i + 3u) % (sizeof(offs) / sizeof(offs[0]))];
            if (o + n > a.bytes) { continue; }
            for (k = 0; k < n; k++) { buf[k] = next_byte(); }
            /* host to device and back */
            CHECK(bc250hsa_copy_to_device(&d, &a.m, o, buf, n) == BC250HSA_OK);
            memcpy(a.ref + o, buf, (size_t)n);
            /* device to device: a scatter source on one grid into another grid, and into a
             * direct mapping */
            if (o2 + n <= b.bytes) {
                CHECK(bc250hsa_copy_device_cpu(&d, &b.m, o2, &a.m, o, n) == BC250HSA_OK);
                memcpy(b.ref + o2, a.ref + o, (size_t)n);
                CHECK(bc250hsa_copy_device_cpu(&d, &h.m, o2, &b.m, o2, n) == BC250HSA_OK);
                memcpy(h.ref + o2, b.ref + o2, (size_t)n);
            }
            /* a fill across the boundaries */
            if (o + n + 7u <= a.bytes) {
                CHECK(bc250hsa_fill_device_cpu(&d, &a.m, o + 3u, (uint8_t)(i * 7u + j), n + 4u) ==
                      BC250HSA_OK);
                memset(a.ref + o + 3u, (int)(uint8_t)(i * 7u + j), (size_t)(n + 4u));
            }
        }
    }
    CHECK(model_same(&d, &a) && model_same(&d, &b) && model_same(&d, &h));
    CHECK(memcmp(h.m.host, h.ref, (size_t)h.bytes) == 0);
    /* the very last byte, and one past it */
    CHECK(bc250hsa_fill_device_cpu(&d, &a.m, a.bytes - 1u, 0x5Au, 1u) == BC250HSA_OK);
    a.ref[a.bytes - 1u] = 0x5Au;
    CHECK(model_same(&d, &a));
    CHECK(bc250hsa_fill_device_cpu(&d, &a.m, a.bytes, 0u, 1u) == BC250HSA_EINVAL);
    CHECK(bc250hsa_copy_device_cpu(&d, &b.m, b.bytes - 1u, &a.m, 0u, 2u) == BC250HSA_EINVAL);
    CHECK(bc250hsa_copy_device_cpu(&d, &b.m, 0u, &a.m, UINT64_MAX, 2u) == BC250HSA_EINVAL);

    /* The GPU copy sees one contiguous range across the children of both grids. */
    {
        bc250hsa_op_receipt r = receipt_new();
        uint32_t            ready = 0u;
        CHECK(bc250hsa_gpu_copy_ready(&d, &ready) == BC250HSA_OK && ready == 1u);
        CHECK(bc250hsa_copy_device_to_device(&d, &b.m, 0x7FFF0u, &a.m, 0x3FFF8u, 0x80020u, 0u,
                                             &r) == BC250HSA_OK);
        CHECK(r.state == BC250HSA_OP_RETIRED);
        memcpy(b.ref + 0x7FFF0u, a.ref + 0x3FFF8u, 0x80020u);
        CHECK(model_same(&d, &b));
        r = receipt_new();
        CHECK(bc250hsa_fill_device(&d, &a.m, 0x3FFFEu, 0xC7u, 0x40004u, &r) == BC250HSA_OK);
        memset(a.ref + 0x3FFFEu, 0xC7, 0x40004u);
        CHECK(model_same(&d, &a));
    }

    CHECK(bc250hsa_free(&d, &a.m) == BC250HSA_OK && bc250hsa_free(&d, &b.m) == BC250HSA_OK);
    CHECK(bc250hsa_free(&d, &h.m) == BC250HSA_OK);
    free(a.ref); free(b.ref); free(h.ref);
    CHECK(live_reservations() == 0u && g_bad_frees == 0u && g_bad_destroys == 0u);
    dev_finish(&d, 0);
    CHECK(g_unlocks - g_unlocks_at_init == g_locks);
}

/* MEM_DEVICE: no host route at all, only the GPU calls. ZERO needs the host route. */
static void case_device_only(void)
{
    struct bc250hsa_device d;
    bc250hsa_mem           m, z, src;
    uint8_t                one = 1u;
    uint32_t               creates;

    printf("aggregate: device memory without a host route\n");
    dev_init(&d);
    g_aggregate_child_cap = 0x40000u;
    CHECK(bc250hsa_alloc(&d, 0xC0000u, 0u, BC250HSA_MEM_DEVICE, &m) == BC250HSA_OK);
    check_shape(&m, 0xC0000u, 3u, 0x40000u);
    CHECK(access(&d, &m) == BC250HSA_ACCESS_NONE && g_locks == 0u);
    CHECK(bc250hsa_copy_to_device(&d, &m, 0u, &one, 1u) == BC250HSA_EUNSUPPORTED);
    CHECK(bc250hsa_fill_device_cpu(&d, &m, 0u, 0u, 1u) == BC250HSA_EUNSUPPORTED);
    {
        void* host = NULL;
        CHECK(bc250hsa_map(&d, &m, &host) == BC250HSA_EUNSUPPORTED);
    }
    creates = g_creates;
    memset(&z, 0xa5, sizeof(z));
    CHECK(bc250hsa_alloc(&d, 0xC0000u, 0u, BC250HSA_MEM_DEVICE | BC250HSA_MEM_ZERO, &z) ==
          BC250HSA_EUNSUPPORTED);
    CHECK(z.opaque == NULL && g_creates - creates == 3u && live_allocs() == 3u);
    CHECK(record_count(&d) == 1u && live_reservations() == 1u);
    /* GPU route: a scatter source into the device-only range and back, across children */
    CHECK(bc250hsa_alloc(&d, 0xC0000u, 0u, BC250HSA_MEM_MAPPABLE, &src) == BC250HSA_OK);
    g_aggregate_child_cap = BC250HSA_ALLOCATION_CHILD_MAX;
    {
        uint8_t             pattern[0x100], back[0x100];
        bc250hsa_op_receipt r = receipt_new();
        uint32_t            i, ready = 0u;
        for (i = 0; i < sizeof(pattern); i++) { pattern[i] = next_byte(); }
        CHECK(bc250hsa_copy_to_device(&d, &src, 0x3FF80u, pattern, sizeof(pattern)) ==
              BC250HSA_OK);
        CHECK(bc250hsa_gpu_copy_ready(&d, &ready) == BC250HSA_OK && ready == 1u);
        CHECK(bc250hsa_copy_device_to_device(&d, &m, 0x7FF80u, &src, 0x3FF80u, sizeof(pattern),
                                             0u, &r) == BC250HSA_OK);
        r = receipt_new();
        CHECK(bc250hsa_copy_device_to_device(&d, &src, 0x40u, &m, 0x7FF80u, sizeof(pattern),
                                             0u, &r) == BC250HSA_OK);
        CHECK(bc250hsa_copy_from_device(&d, back, &src, 0x40u, sizeof(back)) == BC250HSA_OK);
        CHECK(memcmp(pattern, back, sizeof(back)) == 0);
    }
    CHECK(bc250hsa_free(&d, &m) == BC250HSA_OK && bc250hsa_free(&d, &src) == BC250HSA_OK);
    CHECK(d.allocations == NULL && live_allocs() == 0u);
    dev_finish(&d, 0);
}

/* A failure of the n-th call of each kind. persist: every later call of that kind fails too,
 * so the second heap is refused as well. */
static void case_construction_failures(void)
{
    static const int kinds[] = { INJ_CREATE, INJ_RESIDENT, INJ_MAP, INJ_LOCK, INJ_RESERVE };
    static const char* names[] = { "create", "resident", "map", "lock", "reserve" };
    static const NTSTATUS statuses[] = { STATUS_NO_MEMORY, STATUS_ACCESS_DENIED };
    uint32_t k, at, s, persist;

    printf("aggregate: a failure at every call of the construction\n");
    for (k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
        const uint32_t calls = kinds[k] == INJ_RESERVE ? 1u : 4u;
        for (at = 1u; at <= calls; at++) {
            for (s = 0; s < 2u; s++) {
                for (persist = 0; persist < 2u; persist++) {
                    struct bc250hsa_device d;
                    bc250hsa_mem           m;
                    bc250hsa_status        got, want;
                    const int              heaps = kinds[k] == INJ_CREATE ||
                                                   kinds[k] == INJ_RESIDENT;
                    dev_init(&d);
                    g_aggregate_child_cap = 0x40000u;
                    inject_set(kinds[k], at, (int)persist, statuses[s]);
                    memset(&m, 0xa5, sizeof(m));
                    got = bc250hsa_alloc(&d, 0x100000u, 0u, BC250HSA_MEM_MAPPABLE, &m);
                    g_aggregate_child_cap = BC250HSA_ALLOCATION_CHILD_MAX;
                    /* A single refusal of a create or a residency in the carve-out is answered
                     * with system memory; the map, the lock and the reservation have no second
                     * try. */
                    if (heaps && !persist) {
                        want = BC250HSA_OK;
                    } else {
                        want = statuses[s] == STATUS_NO_MEMORY && kinds[k] != INJ_LOCK
                                   ? BC250HSA_ENOMEM : BC250HSA_EOS;
                    }
                    if (got != want) {
                        printf("  %s at %u status 0x%08lx persist %u: %d, want %d\n", names[k], at,
                               (unsigned long)statuses[s], persist, got, want);
                    }
                    CHECK(got == want);
                    if (got == BC250HSA_OK) {
                        check_shape(&m, 0x100000u, 4u, 0x40000u);
                        CHECK(bc250hsa_free(&d, &m) == BC250HSA_OK);
                    } else {
                        CHECK(bc250hsa_last_os_status() == (int32_t)statuses[s]);
                        CHECK(m.opaque == NULL && m.va == 0u && m.host == NULL);
                    }
                    CHECK(d.allocations == NULL);
                    CHECK(live_allocs() == 0u && live_reservations() == 0u);
                    CHECK(g_bad_frees == 0u && g_bad_destroys == 0u);
                    CHECK(g_unlocks - g_unlocks_at_init == g_locks);
                    dev_finish(&d, 0);
                }
            }
        }
    }
}

/* A child mapped elsewhere than asked, and a reservation outside the request. */
static void case_wrong_addresses(void)
{
    struct bc250hsa_device d;
    bc250hsa_mem           m;

    printf("aggregate: a map or a reservation at another address\n");
    dev_init(&d);
    g_aggregate_child_cap = 0x40000u;
    g_map_skew = 0x10000u;
    CHECK(bc250hsa_alloc(&d, 0x100000u, 0u, BC250HSA_MEM_MAPPABLE, &m) == BC250HSA_EOS);
    CHECK(bc250hsa_last_os_status() == (int32_t)STATUS_CONFLICTING_ADDRESSES);
    CHECK(m.opaque == NULL && d.allocations == NULL && live_allocs() == 0u);
    CHECK(live_reservations() == 0u && g_bad_frees == 0u);
    dev_finish(&d, 0);

    dev_init(&d);
    g_aggregate_child_cap = 0x40000u;
    g_reserve_skew = 0x1000u;   /* not on the 64 KiB grid of a reservation */
    CHECK(bc250hsa_alloc(&d, 0x100000u, 0u, BC250HSA_MEM_MAPPABLE, &m) == BC250HSA_EOS);
    CHECK(g_creates == 0u || live_allocs() == 0u);
    CHECK(d.allocations == NULL && live_reservations() == 0u && g_bad_frees == 0u);
    dev_finish(&d, 0);

    /* A reservation on the 64 KiB grid but not on the 2 MiB alignment asked for: the logical
     * base moves up inside the slack, and the whole reservation is freed. */
    dev_init(&d);
    g_aggregate_child_cap = 0x40000u;
    g_reserve_skew = 0x10000u;
    CHECK(bc250hsa_alloc(&d, 0x100000u, 0x200000u, BC250HSA_MEM_MAPPABLE, &m) == BC250HSA_OK);
    CHECK(m.va % 0x200000u == 0u && m.va != rec(&m)->reserve_base);
    CHECK(rec(&m)->reserve_bytes == 0x100000u + 0x200000u - 0x10000u);
    check_shape(&m, 0x100000u, 4u, 0x40000u);
    CHECK(bc250hsa_free(&d, &m) == BC250HSA_OK);
    CHECK(g_reservation_frees == 1u && live_reservations() == 0u && g_bad_frees == 0u);
    g_aggregate_child_cap = BC250HSA_ALLOCATION_CHILD_MAX;
    dev_finish(&d, 0);
}

/* The paging fence: map and residency of every child return values out of order, with zeros
 * after nonzero ones; the one wait asks for the largest, and nothing is published before it. */
static void case_paging_fence(void)
{
    static const uint64_t script[] = { 7u, 0u, 41u, 0u, 3u, 0u, 9u, 2u };
    struct bc250hsa_device d;
    bc250hsa_mem           m;
    uint32_t               frees;

    printf("aggregate: the paging fence and publication\n");
    dev_init(&d);
    g_aggregate_child_cap = 0x40000u;
    g_paging_script = script; g_paging_script_len = 8u;
    memset(&m, 0xa5, sizeof(m));
    g_watch_mem = &m; g_watch_records = 0u;
    CHECK(bc250hsa_alloc(&d, 0x100000u, 0u, BC250HSA_MEM_MAPPABLE, &m) == BC250HSA_OK);
    g_watch_mem = NULL;
    CHECK(g_paging_issued_max == 41u && g_paging_waited_max == 41u && g_paging_waits == 1u);
    CHECK(g_watch_violations == 0u);
    CHECK(bc250hsa_free(&d, &m) == BC250HSA_OK);
    dev_finish(&d, 0);

    /* A paging wait that fails: nothing is released and nothing is published. */
    dev_init(&d);
    g_aggregate_child_cap = 0x40000u;
    g_paging_script = script; g_paging_script_len = 8u;
    inject_set(INJ_PAGING_WAIT, 1u, 0, STATUS_DEVICE_REMOVED);
    memset(&m, 0xa5, sizeof(m));
    frees = g_free_vas;
    CHECK(bc250hsa_alloc(&d, 0x100000u, 0u, BC250HSA_MEM_MAPPABLE, &m) == BC250HSA_EOS);
    CHECK(m.opaque == NULL && g_free_vas == frees && g_locks == 0u);
    CHECK(live_allocs() == 4u && live_reservations() == 1u);
    CHECK(record_count(&d) == 1u && d.allocations->retiring == 1u);
    g_aggregate_child_cap = BC250HSA_ALLOCATION_CHILD_MAX;
    dev_finish(&d, 0);   /* close drains the kept record */
    CHECK(live_allocs() == 0u && live_reservations() == 0u);

    /* A later child fails after earlier paging work: that work completes before the release. */
    dev_init(&d);
    g_aggregate_child_cap = 0x40000u;
    g_paging_script = script; g_paging_script_len = 8u;
    inject_set(INJ_MAP, 3u, 0, STATUS_ACCESS_DENIED);
    CHECK(bc250hsa_alloc(&d, 0x100000u, 0u, BC250HSA_MEM_MAPPABLE, &m) == BC250HSA_EOS);
    CHECK(g_paging_waits == 1u && g_paging_waited_max == g_paging_issued_max);
    CHECK(live_allocs() == 0u && live_reservations() == 0u);
    g_aggregate_child_cap = BC250HSA_ALLOCATION_CHILD_MAX;
    dev_finish(&d, 0);
}

/* A release that stops part way, the retiring state, the retry, and close. */
static void case_release_failures(void)
{
    static const int kinds[] = { INJ_UNLOCK, INJ_FREE_VA, INJ_DESTROY };
    uint32_t k, at;

    printf("aggregate: a free that fails part way, and its retry\n");
    for (k = 0; k < 3u; k++) {
        for (at = 1u; at <= (kinds[k] == INJ_FREE_VA ? 1u : 4u); at++) {
            struct bc250hsa_device d;
            bc250hsa_mem           m, copy;
            uint8_t                b = 0u;
            uint32_t               a = 0u;
            void*                  host = NULL;
            dev_init(&d);
            g_aggregate_child_cap = 0x40000u;
            CHECK(bc250hsa_alloc(&d, 0x100000u, 0u, BC250HSA_MEM_MAPPABLE, &m) == BC250HSA_OK);
            g_aggregate_child_cap = BC250HSA_ALLOCATION_CHILD_MAX;
            copy = m;
            inject_set(kinds[k], at, 0, STATUS_ACCESS_DENIED);
            CHECK(bc250hsa_free(&d, &m) == BC250HSA_EOS);
            CHECK(memcmp(&m, &copy, sizeof(m)) == 0);   /* the caller keeps its handle */
            CHECK(record_count(&d) == 1u && rec(&m)->retiring == 1u);
            CHECK(bc250hsa_memory_access(&d, &m, &a) == BC250HSA_EINVAL);
            CHECK(bc250hsa_copy_to_device(&d, &m, 0u, &b, 1u) == BC250HSA_EINVAL);
            CHECK(bc250hsa_copy_from_device(&d, &b, &m, 0u, 1u) == BC250HSA_EINVAL);
            CHECK(bc250hsa_fill_device_cpu(&d, &m, 0u, 0u, 1u) == BC250HSA_EINVAL);
            CHECK(bc250hsa_map(&d, &m, &host) == BC250HSA_EINVAL);
            if (kinds[k] == INJ_DESTROY) {
                /* the reservation went; the children are destroyed from the last one, and the one
                 * whose destroy failed and those before it stay */
                CHECK(live_reservations() == 0u && live_allocs() == 5u - at);
            } else {
                CHECK(live_reservations() == 1u && live_allocs() == 4u);
            }
            if (at % 2u == 1u) {
                CHECK(bc250hsa_free(&d, &m) == BC250HSA_OK);   /* the retry finishes */
                CHECK(m.opaque == NULL && d.allocations == NULL);
            }
            dev_finish(&d, 0);   /* or close drains it */
            CHECK(live_allocs() == 0u && live_reservations() == 0u);
            CHECK(g_bad_frees == 0u && g_bad_destroys == 0u && g_unlocks - g_unlocks_at_init == g_locks);
        }
    }

    /* An unpublished record whose cleanup fails: the caller gets the first error, and the
     * rest waits for close. */
    {
        struct bc250hsa_device d;
        bc250hsa_mem           m;
        dev_init(&d);
        g_aggregate_child_cap = 0x40000u;
        /* child 2 fails in both heaps (create calls 3 and 4), and the destroy of child 1, the
         * first one the cleanup destroys, fails as well */
        inject_set(INJ_CREATE, 3u, 1, STATUS_ACCESS_DENIED);
        g_inj2_kind = INJ_DESTROY; g_inj2_at = 1u; g_inj2_status = STATUS_INVALID_HANDLE;
        memset(&m, 0xa5, sizeof(m));
        CHECK(bc250hsa_alloc(&d, 0x100000u, 0u, BC250HSA_MEM_MAPPABLE, &m) == BC250HSA_EOS);
        CHECK(bc250hsa_last_os_status() == (int32_t)STATUS_ACCESS_DENIED);
        CHECK(m.opaque == NULL && m.va == 0u);
        CHECK(live_allocs() == 2u && live_reservations() == 0u);
        CHECK(record_count(&d) == 1u && d.allocations->retiring == 1u);
        g_aggregate_child_cap = BC250HSA_ALLOCATION_CHILD_MAX;
        dev_finish(&d, 0);
        CHECK(live_allocs() == 0u && g_bad_destroys == 0u && g_bad_frees == 0u);
    }
}

/* Refusals before any kernel call. */
static void case_planner_refusals(void)
{
    struct bc250hsa_device d;
    bc250hsa_mem           m;
    printf("aggregate: refusals before any kernel call\n");
    dev_init(&d);
    CHECK(bc250hsa_alloc(&d, UINT64_MAX, 0u, BC250HSA_MEM_MAPPABLE, &m) == BC250HSA_EINVAL);
    CHECK(bc250hsa_alloc(&d, UINT64_MAX - 0x3FFFFu, 0u, BC250HSA_MEM_DEVICE, &m) ==
          BC250HSA_EINVAL);
    /* larger than the 128 GiB window of dev_init: address space, not a parameter error */
    CHECK(bc250hsa_alloc(&d, 256ull << 30, 0u, BC250HSA_MEM_MAPPABLE, &m) == BC250HSA_ENOMEM);
    CHECK(bc250hsa_alloc(&d, 8ull << 30, 1ull << 63, BC250HSA_MEM_MAPPABLE, &m) ==
          BC250HSA_ENOMEM);
    CHECK(bc250hsa_alloc(&d, 8ull << 30, 3u, BC250HSA_MEM_MAPPABLE, &m) == BC250HSA_EINVAL);
    CHECK(g_creates == 0u && g_reserves == 0u && d.allocations == NULL);
    /* A large host-visible range stays one allocation, and VidMm refuses it as on unit A. */
    CHECK(bc250hsa_alloc(&d, 5ull << 30, 0u, BC250HSA_MEM_HOST, &m) == BC250HSA_EOS);
    CHECK(bc250hsa_last_os_status() == (int32_t)STATUS_INVALID_PARAMETER && g_reserves == 0u);
    /* Exactly the cap is one allocation; one byte more is an aggregate of two. */
    CHECK(bc250hsa_alloc(&d, 0xFFFC0000ull, 0u, BC250HSA_MEM_DEVICE, &m) == BC250HSA_OK);
    CHECK(rec(&m)->kind == BC250HSA_RECORD_SINGLE && g_reserves == 0u);
    CHECK(bc250hsa_free(&d, &m) == BC250HSA_OK);
    CHECK(bc250hsa_alloc(&d, 0xFFFC0001ull, 0u, BC250HSA_MEM_DEVICE, &m) == BC250HSA_OK);
    check_shape(&m, 0xFFFC0000ull + 0x40000u, 2u, 0xFFFC0000ull);
    CHECK(rec(&m)->children[1].bytes == 0x40000u);
    CHECK(bc250hsa_free(&d, &m) == BC250HSA_OK);
    dev_finish(&d, 0);
}

/* The budgets of the 8 GiB carve-out that unit A measured at the BD-117 trace. */
#define TRACE_LOCAL_BUDGET    7482638336ull
#define TRACE_NONLOCAL_BUDGET 3733582234ull

/* A request above both budgets, a request that only the per-child check can refuse, and the
 * request that fits: no residency past a budget, ever. */
static void case_budget_gate(void)
{
    struct bc250hsa_device d;
    bc250hsa_mem           m;
    uint32_t               creates;

    printf("aggregate: the local and non-local budgets\n");
    /* The PyTorch request is the total of both budgets plus 256 MiB: out of memory before any
     * Windows call. */
    dev_init(&d);
    g_local_budget = TRACE_LOCAL_BUDGET;
    g_nonlocal_budget = TRACE_NONLOCAL_BUDGET;
    creates = g_creates;
    memset(&m, 0xa5, sizeof(m));
    CHECK(bc250hsa_alloc(&d, 11484656026ull, 0u, BC250HSA_MEM_MAPPABLE, &m) == BC250HSA_ENOMEM);
    CHECK(m.opaque == NULL && m.va == 0u);
    CHECK(g_creates == creates && g_reserves == 0u && live_allocs() == 0u);
    CHECK(g_nonlocal_overcommits == 0u && d.allocations == NULL);
    /* 6 GiB fits the carve-out: two children, both in VRAM. */
    CHECK(bc250hsa_alloc(&d, 6ull << 30, 0u, BC250HSA_MEM_MAPPABLE, &m) == BC250HSA_OK);
    check_shape(&m, 6ull << 30, 2u, 0xFFFC0000ull);
    CHECK(rec(&m)->children[0].heap == BC250HSA_HEAP_VRAM);
    CHECK(rec(&m)->children[1].heap == BC250HSA_HEAP_VRAM);
    CHECK(bc250hsa_free(&d, &m) == BC250HSA_OK);
    dev_finish(&d, 0);

    /* 8 GiB under a 6 GiB carve-out and 3 GiB of system memory: the sum holds it, but the
     * second child fits neither group. It is refused, and nothing stays. */
    dev_init(&d);
    g_local_budget = 6ull << 30;
    g_nonlocal_budget = 3ull << 30;
    CHECK(bc250hsa_alloc(&d, 8ull << 30, 0u, BC250HSA_MEM_MAPPABLE, &m) == BC250HSA_ENOMEM);
    CHECK(m.opaque == NULL && live_allocs() == 0u && live_reservations() == 0u);
    CHECK(g_nonlocal_overcommits == 0u && d.allocations == NULL);
    /* With 5 GiB of system memory the same request takes the second child from it. */
    g_nonlocal_budget = 5ull << 30;
    CHECK(bc250hsa_alloc(&d, 8ull << 30, 0u, BC250HSA_MEM_MAPPABLE, &m) == BC250HSA_OK);
    CHECK(rec(&m)->children[0].heap == BC250HSA_HEAP_VRAM);
    CHECK(rec(&m)->children[1].heap == BC250HSA_HEAP_GTT);
    CHECK(g_nonlocal_overcommits == 0u);
    CHECK(bc250hsa_free(&d, &m) == BC250HSA_OK);
    CHECK(live_allocs() == 0u && live_reservations() == 0u);
    dev_finish(&d, 0);
}

int main(void)
{
    InitializeCriticalSection(&g_fake);
    bc250hsa_set_log(print_log, NULL, BC250HSA_LOG_INFO);
    case_planner_refusals();
    case_pytorch_request();
    case_budget_gate();
    case_scatter_walkers();
    case_device_only();
    case_construction_failures();
    case_wrong_addresses();
    case_paging_fence();
    case_release_failures();
    DeleteCriticalSection(&g_fake);
    printf("aggregate allocation: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
