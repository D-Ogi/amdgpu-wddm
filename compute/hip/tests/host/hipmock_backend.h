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

#include "bc250hsa.h"

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
    BC250HSA_MOCK_COPY_FROM_DEVICE,
    /* The GPU copy route of scratch/hip-gpu-copy/CONTRACT.md. The three kinds say which
     * of layer 1's new calls layer 2 chose, so a test can tell the direct route of
     * section 5a from the staged route of section 5b and from the host-processor path. */
    BC250HSA_MOCK_GPU_COPY,
    BC250HSA_MOCK_GPU_COPY_STAGED,
    BC250HSA_MOCK_GPU_FILL
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

/* The GPU copy route of CONTRACT.md, from the side a test drives it.
 *
 *   coherence 0 makes bc250hsa_gpu_copy_ready report 0, which is the control of section 8
 *     failing: every device-to-host copy of layer 2 must then take the host-processor
 *     path and count a fallback, and no device-to-host GPU copy may be recorded.
 *   refuse 1 makes the three GPU calls answer BC250HSA_EUNSUPPORTED, which is layer 1
 *     refusing one copy: layer 2 must fall back and stay correct.
 *
 * Both default to the working case (coherence 1, refuse 0), and bc250hsa_mock_reset
 * restores them.
 *
 * The coherence gate follows the destination allocation, as layer 1's does: a destination
 * with a host mapping, or one made with BC250HSA_MEM_HOST, _HOST_WC, _EXEC or _MAPPABLE, is
 * gated whatever the call is named, so a device-to-device copy into a hipMalloc buffer or
 * into the GPU alias of a hipHostMalloc buffer is refused while coherence is 0. */
void bc250hsa_mock_set_gpu_copy(int coherence, int refuse);

/* A failure of the three GPU calls, which bc250hsa_mock_reset clears.
 *
 *   BC250HSA_MOCK_FAULT_BEFORE  every call answers status before it submits anything, with the
 *                               receipt at BC250HSA_OP_NOT_SUBMITTED: a refusal after which
 *                               layer 2 may do the work another way if the status allows it.
 *   BC250HSA_MOCK_FAULT_AFTER   every call submits its fence value and then answers status, as
 *                               a wait that could not establish retirement does, with the
 *                               receipt at BC250HSA_OP_SUBMITTED. The bytes never land, so a
 *                               host-processor copy that layer 2 must not do would show in the
 *                               destination.
 *   BC250HSA_MOCK_FAULT_UNRETIRED  the next call submits its fence value and answers status with
 *                               the receipt at BC250HSA_OP_SUBMITTED, and that value then stays
 *                               unretired, as the copy of the exclusion of bc250hsa.h section
 *                               5.1 does: every later submission is refused with status and
 *                               bc250hsa_copy_unretired names the copy, until the test calls
 *                               bc250hsa_mock_retire_unretired, which lands the copy's bytes and
 *                               retires its value. The fault acts on one call and then clears
 *                               itself. */
#define BC250HSA_MOCK_FAULT_NONE   0
#define BC250HSA_MOCK_FAULT_BEFORE 1
#define BC250HSA_MOCK_FAULT_AFTER  2
#define BC250HSA_MOCK_FAULT_UNRETIRED 3
void bc250hsa_mock_set_gpu_fault(int when, bc250hsa_status status);

/* The late retirement of a copy that BC250HSA_MOCK_FAULT_UNRETIRED left unretired: its bytes
 * land now and the fence reaches its value. The exclusion ends at the next submission or
 * bc250hsa_copy_unretired that sees it, as in layer 1. bc250hsa_mock_reset retires such a copy
 * as well, without its bytes. */
void bc250hsa_mock_retire_unretired(void);

/* A device that layer 1 marked lost, as far as bc250hsa_copy_unretired and
 * bc250hsa_query_fault tell it: the first answers BC250HSA_EDEVICELOST whatever the fence reads
 * and whether a copy is unretired or not, and retires nothing. Every other call of the mock goes
 * on as before, so a test sees which layer-2 path asks. bc250hsa_mock_reset clears it. */
void bc250hsa_mock_set_lost(int lost);

/* A delay between the start of a GPU call and its submission, with the lock of the mock
 * released, which bc250hsa_mock_reset clears. It holds the window open in which another thread
 * could free memory the call is about to name. */
void bc250hsa_mock_set_gpu_delay_ms(uint32_t delay_ms);

/* The GPU calls between their start and their return. It takes the lock of the mock, so a
 * test may read it while its threads run. */
uint32_t bc250hsa_mock_gpu_in_flight(void);

/* Frees of an allocation that a GPU call still named or whose last GPU fence value had not
 * retired. It counts for the life of the process and takes the lock. */
uint32_t bc250hsa_mock_free_violations(void);

/* The duration counters of bc250hsa.h section 3. The mock times the wait, the emulated
 * submission and the three GPU copy calls with bc250hsa_ticks, as layer 1 times the real ones,
 * and counts each of those durations as at least one tick, so that every operation moves its
 * total even when the host finishes it inside one tick. cpu_copy_fallback_ticks is what layer 2
 * reports, kept as it is. All of them are host durations of a mock and never a measurement of
 * the device. */

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
