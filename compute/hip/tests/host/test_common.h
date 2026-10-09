/* test_common.h - the three lines every host test of bc250hsa needs.
 *
 * Each test takes the directory of tests/data as its one argument, so the build
 * script can run it from anywhere. A test prints one line per check it failed and
 * exits with the number of failures, which is what compute/hip/build.ps1 reads.
 */
#ifndef BC250HSA_TEST_COMMON_H
#define BC250HSA_TEST_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bc250hsa.h"

static int g_failures;
static int g_checks;

#define TEST_COUNT(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        g_checks++;                                                                      \
        if (!(cond)) {                                                                   \
            g_failures++;                                                                \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                       \
        }                                                                                \
    } while (0)

#define CHECK_U64(actual, expected)                                                      \
    do {                                                                                 \
        const unsigned long long a_ = (unsigned long long)(actual);                       \
        const unsigned long long e_ = (unsigned long long)(expected);                     \
        g_checks++;                                                                      \
        if (a_ != e_) {                                                                  \
            g_failures++;                                                                \
            printf("FAIL %s:%d  %s is 0x%llx, expected 0x%llx\n", __FILE__, __LINE__,    \
                   #actual, a_, e_);                                                     \
        }                                                                                \
    } while (0)

#define CHECK_STATUS(call, expected)                                                     \
    do {                                                                                 \
        const bc250hsa_status s_ = (call);                                               \
        g_checks++;                                                                      \
        if (s_ != (expected)) {                                                          \
            g_failures++;                                                                \
            printf("FAIL %s:%d  %s returned %s, expected %s\n", __FILE__, __LINE__,      \
                   #call, bc250hsa_status_string(s_),                                    \
                   bc250hsa_status_string(expected));                                    \
        }                                                                                \
    } while (0)

static int test_report(const char* name)
{
    printf("%s: %d checks, %d failed\n", name, g_checks, g_failures);
    return g_failures;
}

static const char* test_data_dir(int argc, char** argv)
{
    return (argc > 1) ? argv[1] : "compute/hip/tests/data";
}

/* Reads a whole fixture into memory. A test that cannot read its fixture fails
 * loudly instead of passing with nothing to check. */
static void* test_read_file(const char* dir, const char* name, size_t* bytes_out)
{
    char   path[1024];
    FILE*  f;
    void*  buffer;
    long   length;
    size_t read;

    *bytes_out = 0;
    if (snprintf(path, sizeof(path), "%s/%s", dir, name) < 0) {
        return NULL;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        printf("FAIL cannot open %s\n", path);
        g_failures++;
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (length = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        g_failures++;
        return NULL;
    }
    buffer = malloc((size_t)length);
    if (buffer == NULL) {
        fclose(f);
        g_failures++;
        return NULL;
    }
    read = fread(buffer, 1, (size_t)length, f);
    fclose(f);
    if (read != (size_t)length) {
        free(buffer);
        g_failures++;
        return NULL;
    }
    *bytes_out = read;
    return buffer;
}

/* --------------------------------------------------------------------------
 * The host allocator: malloc with a synthetic GPU address. It is what lets the
 * loader, the packer and the PM4 builder run with no BC-250 adapter.
 * ------------------------------------------------------------------------ */

typedef struct test_alloc_block {
    void*    raw;
    uint64_t bytes;
} test_alloc_block;

static bc250hsa_status test_host_alloc(void* ctx, uint64_t bytes, uint64_t alignment,
                                       uint32_t flags, bc250hsa_mem* out)
{
    uint8_t*          raw;
    uintptr_t         aligned;
    test_alloc_block* block;

    (void)ctx;
    if (alignment < 4096u) {
        alignment = 4096u;
    }
    raw = (uint8_t*)calloc(1, (size_t)(bytes + alignment));
    block = (test_alloc_block*)calloc(1, sizeof(*block));
    if (raw == NULL || block == NULL) {
        free(raw);
        free(block);
        return BC250HSA_ENOMEM;
    }
    aligned = ((uintptr_t)raw + (uintptr_t)alignment - 1u) & ~((uintptr_t)alignment - 1u);
    block->raw = raw;
    block->bytes = bytes;
    memset(out, 0, sizeof(*out));
    out->host = (void*)aligned;
    /* The host pointer doubles as the synthetic GPU address: it is aligned the same
     * way, so the 256-byte entry address rule of COMPUTE_PGM_LO still holds. */
    out->va = (uint64_t)aligned;
    out->bytes = bytes;
    out->flags = flags;
    out->opaque = block;
    return BC250HSA_OK;
}

static void test_host_free(void* ctx, bc250hsa_mem* mem)
{
    test_alloc_block* block = (test_alloc_block*)mem->opaque;
    (void)ctx;
    if (block != NULL) {
        free(block->raw);
        free(block);
    }
    memset(mem, 0, sizeof(*mem));
}

static void test_allocator(bc250hsa_allocator* out)
{
    out->ctx = NULL;
    out->alloc = test_host_alloc;
    out->free = test_host_free;
}

#endif /* BC250HSA_TEST_COMMON_H */
