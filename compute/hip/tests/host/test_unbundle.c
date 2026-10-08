/* test_unbundle.c - the clang offload bundle reader.
 *
 * Section 5.4 of docs/design/m16-hip-route-b.md: this test must catch a reader that
 * picks the host entry, that keys an entry by its offset, that misses the compressed
 * magic, or that splits a target identifier on a hyphen.
 *
 * The three traps are measured facts of the committed fat binary beside this file:
 *   1. the host entry has size 0,
 *   2. the host entry has the same offset (0x1000) as the device entry,
 *   3. the host entry's identifier names a fixed placeholder triple,
 *      "host-x86_64-unknown-linux-gnu-", even though clang ran on Windows.
 *
 * The rest of the cases are built in this file, because a bundle with two device
 * entries needs a compiler invocation that our fixture does not use.
 */
#include "test_common.h"

#include "co_internal.h"

#define FATBIN_BODY_OFFSET 0x1000u
#define FATBIN_BODY_BYTES  7128u
#define FATBIN_BYTES       11224u

/* --------------------------------------------------------------------------------
 * A bundle builder
 * ------------------------------------------------------------------------------ */

#define BUNDLE_MAX_ENTRIES 4u
#define BUNDLE_BYTES       4096u

typedef struct bundle_entry {
    const char* id;
    uint32_t    body_bytes;  /* 0 makes a host entry: size 0, the device entry's offset */
    int         body_is_elf;
} bundle_entry;

static uint8_t  g_bundle[BUNDLE_BYTES];
static uint32_t g_bundle_bytes;

/* Lays out magic, count, the entry headers and then the bodies. Every size-0 entry
 * takes the offset of the first body, which is what clang does. */
static void build_bundle(const bundle_entry* entries, uint32_t count)
{
    uint32_t at = BC250HSA_BUNDLE_MAGIC_BYTES + 8u;
    uint32_t body_at;
    uint32_t i;
    uint64_t value;

    memset(g_bundle, 0, sizeof(g_bundle));
    memcpy(g_bundle, BC250HSA_BUNDLE_MAGIC, BC250HSA_BUNDLE_MAGIC_BYTES);
    value = count;
    memcpy(g_bundle + BC250HSA_BUNDLE_MAGIC_BYTES, &value, 8);

    for (i = 0; i < count; i++) {
        at += 24u + (uint32_t)strlen(entries[i].id);
    }
    body_at = (at + 15u) & ~15u;

    at = BC250HSA_BUNDLE_MAGIC_BYTES + 8u;
    {
        uint32_t next_body = body_at;
        for (i = 0; i < count; i++) {
            const uint32_t id_bytes = (uint32_t)strlen(entries[i].id);
            const uint32_t size = entries[i].body_bytes;
            value = (size == 0u) ? body_at : next_body;
            memcpy(g_bundle + at + 0u, &value, 8);
            value = size;
            memcpy(g_bundle + at + 8u, &value, 8);
            value = id_bytes;
            memcpy(g_bundle + at + 16u, &value, 8);
            memcpy(g_bundle + at + 24u, entries[i].id, id_bytes);
            if (size != 0u) {
                memset(g_bundle + next_body, (int)('A' + (int)i), size);
                if (entries[i].body_is_elf) {
                    memcpy(g_bundle + next_body, "\177ELF", 4);
                }
                next_body += size;
            }
            at += 24u + id_bytes;
        }
        g_bundle_bytes = (next_body > body_at) ? next_body : body_at;
    }
}

static const void* g_image;
static size_t      g_image_bytes;

static bc250hsa_status unbundle_built(const char* target)
{
    g_image = NULL;
    g_image_bytes = 0;
    return bc250hsa_unbundle(g_bundle, g_bundle_bytes, target, &g_image, &g_image_bytes);
}

/* --------------------------------------------------------------------------------
 * The committed fat binary
 * ------------------------------------------------------------------------------ */

static void check_committed_fatbin(const char* dir)
{
    void*       fatbin;
    size_t      fatbin_bytes = 0;
    const void* image = NULL;
    size_t      image_bytes = 0;
    void*       object;
    size_t      object_bytes = 0;

    fatbin = test_read_file(dir, "m16_kernels.fatbin", &fatbin_bytes);
    object = test_read_file(dir, "m16_kernels.gfx1013.co", &object_bytes);
    if (fatbin == NULL || object == NULL) {
        free(fatbin);
        free(object);
        return;
    }
    CHECK_U64(fatbin_bytes, FATBIN_BYTES);

    /* target NULL takes gfx1013. */
    CHECK_STATUS(bc250hsa_unbundle(fatbin, fatbin_bytes, NULL, &image, &image_bytes),
                 BC250HSA_OK);
    CHECK_U64(image_bytes, FATBIN_BODY_BYTES);
    /* The reader copies nothing: it points into the caller's buffer. */
    CHECK((const uint8_t*)image == (const uint8_t*)fatbin + FATBIN_BODY_OFFSET);
    /* The device entry is the same code object as the committed one, of the same size,
     * but not byte for byte: clang names the module identifier symbol
     * __hip_cuid_<hash> from the compilation, and the two invocations of PROVENANCE.txt
     * produce two hashes. The entry is therefore checked by what it holds. */
    CHECK_U64(object_bytes, FATBIN_BODY_BYTES);
    {
        bc250hsa_allocator      alloc;
        struct bc250hsa_module* mod = NULL;
        uint64_t                range_va = 0;
        uint64_t                range_bytes = 0;
        const bc250hsa_kernel*  k;

        test_allocator(&alloc);
        CHECK_STATUS(bc250hsa_module_load_alloc(&alloc, image, image_bytes, &mod),
                     BC250HSA_OK);
        CHECK_U64(bc250hsa_module_kernel_count(mod), 3u);
        CHECK_STATUS(bc250hsa_module_range(mod, &range_va, &range_bytes), BC250HSA_OK);
        k = bc250hsa_module_kernel_by_name(mod, "vadd");
        CHECK(k != NULL);
        if (k != NULL) {
            CHECK_U64(k->descriptor_va - range_va, 0x0C80u);
            CHECK_U64(k->entry_va - range_va, 0x1E00u);
        }
        bc250hsa_module_unload(mod);
    }

    /* The same answer with the processor named. */
    CHECK_STATUS(bc250hsa_unbundle(fatbin, fatbin_bytes, "gfx1013", &image, &image_bytes),
                 BC250HSA_OK);
    CHECK_U64(image_bytes, FATBIN_BODY_BYTES);

    /* Another processor is not in this bundle, and the generic entry is not either,
     * so the answer is a refusal and not the host entry. */
    CHECK_STATUS(bc250hsa_unbundle(fatbin, fatbin_bytes, "gfx1030", &image, &image_bytes),
                 BC250HSA_ENOTFOUND);
    CHECK(image == NULL);
    CHECK_U64(image_bytes, 0u);

    /* A truncated bundle, and a bundle whose magic was overwritten. */
    CHECK_STATUS(bc250hsa_unbundle(fatbin, 20u, NULL, &image, &image_bytes),
                 BC250HSA_EBADBUNDLE);
    ((uint8_t*)fatbin)[4] = 'X';
    CHECK_STATUS(bc250hsa_unbundle(fatbin, fatbin_bytes, NULL, &image, &image_bytes),
                 BC250HSA_EBADBUNDLE);
    /* The compressed magic is refused by name, so that nobody reads "CCOB" as a
     * broken bundle and rebuilds the wrong thing. */
    memcpy(fatbin, "CCOB", 4);
    CHECK_STATUS(bc250hsa_unbundle(fatbin, fatbin_bytes, NULL, &image, &image_bytes),
                 BC250HSA_ECOMPRESSEDBUNDLE);

    /* The null parameters. */
    CHECK_STATUS(bc250hsa_unbundle(NULL, fatbin_bytes, NULL, &image, &image_bytes),
                 BC250HSA_EINVAL);
    CHECK_STATUS(bc250hsa_unbundle(fatbin, fatbin_bytes, NULL, NULL, &image_bytes),
                 BC250HSA_EINVAL);
    CHECK_STATUS(bc250hsa_unbundle(fatbin, fatbin_bytes, NULL, &image, NULL),
                 BC250HSA_EINVAL);

    free(fatbin);
    free(object);
}

/* --------------------------------------------------------------------------------
 * The built bundles
 * ------------------------------------------------------------------------------ */

static void check_built_bundles(void)
{
    /* The shape of the committed fat binary: a host entry with size 0 first, then the
     * device entry. The reader must take the second one. */
    {
        const bundle_entry entries[] = {
            { "host-x86_64-unknown-linux-gnu-", 0u, 0 },
            { "hipv4-amdgcn-amd-amdhsa--gfx1013", 64u, 1 }
        };
        build_bundle(entries, TEST_COUNT(entries));
        CHECK_STATUS(unbundle_built(NULL), BC250HSA_OK);
        CHECK_U64(g_image_bytes, 64u);
        CHECK(g_image != NULL);
        /* The device body, not the host entry that shares its offset. */
        CHECK(g_image != NULL && memcmp(g_image, "\177ELF", 4) == 0);
    }

    /* A bundle with the host entry only. Nothing to run, and the host entry is never
     * the answer. */
    {
        const bundle_entry entries[] = {
            { "host-x86_64-unknown-linux-gnu-", 0u, 0 }
        };
        build_bundle(entries, TEST_COUNT(entries));
        CHECK_STATUS(unbundle_built(NULL), BC250HSA_ENOTFOUND);
    }

    /* A host entry that carries a body is still not a device entry: its identifier
     * does not start with hip. */
    {
        const bundle_entry entries[] = {
            { "host-x86_64-pc-windows-msvc-", 96u, 1 }
        };
        build_bundle(entries, TEST_COUNT(entries));
        CHECK_STATUS(unbundle_built(NULL), BC250HSA_ENOTFOUND);
    }

    /* "gfx10-1-generic" holds two hyphens. A reader that splits the identifier on a
     * hyphen reads the target as "gfx10" and finds nothing. */
    {
        const bundle_entry entries[] = {
            { "host-x86_64-unknown-linux-gnu-", 0u, 0 },
            { "hipv4-amdgcn-amd-amdhsa--gfx10-1-generic", 48u, 1 }
        };
        build_bundle(entries, TEST_COUNT(entries));
        CHECK_STATUS(unbundle_built(NULL), BC250HSA_OK);
        CHECK_U64(g_image_bytes, 48u);
    }

    /* An exact processor wins over the generic entry, whichever comes first. */
    {
        const bundle_entry generic_first[] = {
            { "hipv4-amdgcn-amd-amdhsa--gfx10-1-generic", 48u, 1 },
            { "hipv4-amdgcn-amd-amdhsa--gfx1013", 64u, 1 }
        };
        const bundle_entry exact_first[] = {
            { "hipv4-amdgcn-amd-amdhsa--gfx1013", 64u, 1 },
            { "hipv4-amdgcn-amd-amdhsa--gfx10-1-generic", 48u, 1 }
        };
        build_bundle(generic_first, TEST_COUNT(generic_first));
        CHECK_STATUS(unbundle_built(NULL), BC250HSA_OK);
        CHECK_U64(g_image_bytes, 64u);
        build_bundle(exact_first, TEST_COUNT(exact_first));
        CHECK_STATUS(unbundle_built(NULL), BC250HSA_OK);
        CHECK_U64(g_image_bytes, 64u);
    }

    /* Another processor's entry is never taken, even when it is the only one. */
    {
        const bundle_entry entries[] = {
            { "hipv4-amdgcn-amd-amdhsa--gfx1030", 64u, 1 }
        };
        build_bundle(entries, TEST_COUNT(entries));
        CHECK_STATUS(unbundle_built(NULL), BC250HSA_ENOTFOUND);
        CHECK_STATUS(unbundle_built("gfx1030"), BC250HSA_OK);
    }

    /* A target identifier with feature suffixes after a colon still names gfx1013. */
    {
        const bundle_entry entries[] = {
            { "hipv4-amdgcn-amd-amdhsa--gfx1013:xnack-", 64u, 1 }
        };
        build_bundle(entries, TEST_COUNT(entries));
        CHECK_STATUS(unbundle_built(NULL), BC250HSA_OK);
        CHECK_U64(g_image_bytes, 64u);
    }

    /* The older "hip-" prefix of a code object v4 bundle is read as well. */
    {
        const bundle_entry entries[] = {
            { "hip-amdgcn-amd-amdhsa--gfx1013", 64u, 1 }
        };
        build_bundle(entries, TEST_COUNT(entries));
        CHECK_STATUS(unbundle_built(NULL), BC250HSA_OK);
    }

    /* An entry whose body does not start with the ELF magic is a malformed bundle,
     * not an entry to skip: something else would run as a code object. */
    {
        const bundle_entry entries[] = {
            { "hipv4-amdgcn-amd-amdhsa--gfx1013", 64u, 0 }
        };
        build_bundle(entries, TEST_COUNT(entries));
        CHECK_STATUS(unbundle_built(NULL), BC250HSA_EBADBUNDLE);
    }

    /* A body that reaches past the end of the bundle. */
    {
        const bundle_entry entries[] = {
            { "hipv4-amdgcn-amd-amdhsa--gfx1013", 64u, 1 }
        };
        uint64_t huge = 0x100000u;
        build_bundle(entries, TEST_COUNT(entries));
        memcpy(g_bundle + BC250HSA_BUNDLE_MAGIC_BYTES + 8u + 8u, &huge, 8);
        CHECK_STATUS(unbundle_built(NULL), BC250HSA_EBADBUNDLE);
    }

    /* An entry count that cannot fit in the bundle. */
    {
        const bundle_entry entries[] = {
            { "hipv4-amdgcn-amd-amdhsa--gfx1013", 64u, 1 }
        };
        uint64_t many = 0xFFFFFFFFull;
        build_bundle(entries, TEST_COUNT(entries));
        memcpy(g_bundle + BC250HSA_BUNDLE_MAGIC_BYTES, &many, 8);
        CHECK_STATUS(unbundle_built(NULL), BC250HSA_EBADBUNDLE);
    }

    /* An identifier length that reaches past the end of the bundle. */
    {
        const bundle_entry entries[] = {
            { "hipv4-amdgcn-amd-amdhsa--gfx1013", 64u, 1 }
        };
        uint64_t long_id = 0x8000u;
        build_bundle(entries, TEST_COUNT(entries));
        memcpy(g_bundle + BC250HSA_BUNDLE_MAGIC_BYTES + 8u + 16u, &long_id, 8);
        CHECK_STATUS(unbundle_built(NULL), BC250HSA_EBADBUNDLE);
    }

    /* An identifier with no triple in it at all. */
    {
        const bundle_entry entries[] = {
            { "hipv4-gfx1013", 64u, 1 }
        };
        build_bundle(entries, TEST_COUNT(entries));
        CHECK_STATUS(unbundle_built(NULL), BC250HSA_ENOTFOUND);
    }
}

int main(int argc, char** argv)
{
    const char* dir = test_data_dir(argc, argv);

    check_committed_fatbin(dir);
    check_built_bundles();
    return test_report("test_unbundle");
}
