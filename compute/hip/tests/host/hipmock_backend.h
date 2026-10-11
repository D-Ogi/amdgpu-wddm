/* hipmock_backend.h - the control interface of the mock bc250hsa backend of layer 2.
 *
 * Why it exists: layer 2 (amdhip64.dll) codes against bc250hsa.h and nothing else, so a test
 * build can replace the device half and keep the rest (bc250hsa.h, rule 3). This backend
 * implements the whole contract over host memory. It runs a kernel as a recorded dispatch: it
 * performs no computation, it records what the dispatch asked for, and it retires the fence at
 * once.
 *
 * What a test can therefore check with no GPU: that registration finds a kernel from its host
 * stub, that the kernel argument buffer holds the arguments the program passed, that stream
 * order survives an event wait, and that a second run of the same work allocates nothing new.
 *
 * What it does not check: the real submission path. The code object loader, the kernel argument
 * packer and the PM4 builder of layer 1 have their own tests on branch m16/hip-dispatch, with
 * the same code objects. This file holds a second, smaller reader of the same metadata, which
 * is the price of a test that runs before layer 1 exists.
 */

#ifndef BC250_HIPMOCK_BACKEND_H
#define BC250_HIPMOCK_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum bc250hsa_mock_kind {
    BC250HSA_MOCK_ALLOC = 0,
    BC250HSA_MOCK_FREE,
    BC250HSA_MOCK_MODULE_LOAD,
    BC250HSA_MOCK_MODULE_UNLOAD,
    BC250HSA_MOCK_DISPATCH,
    BC250HSA_MOCK_WAIT,
    BC250HSA_MOCK_COPY_TO_DEVICE,
    BC250HSA_MOCK_COPY_FROM_DEVICE
} bc250hsa_mock_kind;

#define BC250HSA_MOCK_KERNARG_MAX 512u
#define BC250HSA_MOCK_RECORD_MAX  4096u

typedef struct bc250hsa_mock_record {
    uint32_t kind;                  /* bc250hsa_mock_kind */
    char     kernel[64];            /* the kernel of a dispatch, empty otherwise */
    uint32_t grid[3];               /* workgroups */
    uint32_t block[3];              /* work items per workgroup */
    uint32_t dynamic_group_bytes;
    uint64_t va;                    /* the allocation, the kernel argument buffer or the copy */
    uint64_t bytes;
    uint64_t value;                 /* the fence value of a dispatch or of a wait */
    uint32_t wait_slice_ms;         /* the bound that the caller passed to a wait */
    uint32_t wait_total_ms;
    uint32_t kernarg_bytes;
    unsigned char kernarg[BC250HSA_MOCK_KERNARG_MAX]; /* the packed buffer, as submitted */
} bc250hsa_mock_record;

/* Empties the record and the counters. It does not free the allocations of the device, so a
 * test may reset between two runs of the same process and still count live allocations. */
void bc250hsa_mock_reset(void);
void bc250hsa_mock_journal_fail(int fail);
uint32_t bc250hsa_mock_journal_calls(void);
const void* bc250hsa_mock_journal(uint32_t* bytes);
/* Lifetime open attempts, not reset by mock_reset. */
uint32_t bc250hsa_mock_open_calls(void);

/* How long a dispatch takes to retire, in milliseconds. 0, the default, retires it at once and
 * is what every test before the multithreaded one expects. A hold time makes bc250hsa_wait
 * really wait, which is what lets a test measure what the other threads of a process can do
 * while one of them waits for the device. BC250_HIP_MOCK_HOLD_MS sets the same value in a
 * separate process, such as a clang-built HIP program against the mock build of the DLL.
 *
 * The hold time is the only control that a test sets while threads run. The readers below are
 * for a quiescent state: they take no lock and a test calls them with its threads joined. */
void     bc250hsa_mock_set_hold_ms(uint32_t hold_ms);
uint32_t bc250hsa_mock_hold_ms(void);

uint32_t                           bc250hsa_mock_record_count(void);
const bc250hsa_mock_record*        bc250hsa_mock_record_at(uint32_t index);
const char*                        bc250hsa_mock_kind_name(uint32_t kind);

/* The allocations that the device still holds, and their bytes. The leak test compares the two
 * numbers before and after a second run. */
uint32_t bc250hsa_mock_live_allocations(void);
uint64_t bc250hsa_mock_live_bytes(void);

/* The number of dispatches whose kernel has this name. */
uint32_t bc250hsa_mock_dispatch_count(const char* kernel);

/* Writes every record to a text file, one line each. The mock does this by itself when the
 * environment variable BC250_HIP_MOCK_RECORD names a path, which is how a separate process (a
 * clang-built HIP program linked against the mock DLL) is checked by a build script. */
int bc250hsa_mock_write_record(const char* path);

#ifdef __cplusplus
}
#endif

#endif /* BC250_HIPMOCK_BACKEND_H */
