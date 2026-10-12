/* status.c - the status names, the one log sink and the process counters.
 *
 * Layer 1 of docs/design/m16-hip-route-b.md, section 3.1 ("Status strings, counters,
 * the log hook"). This file touches no device and no Windows display interface, so
 * every host test links it.
 */
#include <stdio.h>

#include "internal.h"

#if defined(_MSC_VER)
#include <intrin.h>
#define BC250HSA_THREAD_LOCAL __declspec(thread)
#else
#define BC250HSA_THREAD_LOCAL _Thread_local
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/* --------------------------------------------------------------------------
 * Status names
 * ------------------------------------------------------------------------ */

const char* bc250hsa_status_string(bc250hsa_status status)
{
    switch (status) {
    case BC250HSA_OK:                 return "ok";
    case BC250HSA_EINVAL:             return "invalid argument";
    case BC250HSA_ENOMEM:             return "out of memory";
    case BC250HSA_ENODEV:             return "no device";
    case BC250HSA_ETIMEOUT:           return "timeout, device still runs";
    case BC250HSA_EDEVICELOST:        return "device lost";
    case BC250HSA_EUNSUPPORTED:       return "not supported by this build";
    case BC250HSA_EOS:                return "windows call failed";
    case BC250HSA_ENOTFOUND:          return "not found";
    case BC250HSA_EBUSY:              return "no free command ring slot";
    case BC250HSA_EBADELF:            return "not a gfx1013 code object";
    case BC250HSA_EWRONGTARGET:       return "code object names another processor";
    case BC250HSA_EBADABIVERSION:     return "code object abi version is not 5 or 6";
    case BC250HSA_EBADRELOC:          return "unsupported relocation";
    case BC250HSA_EBADMETADATA:       return "metadata note absent or unreadable";
    case BC250HSA_EBADBUNDLE:         return "offload bundle does not parse";
    case BC250HSA_ECOMPRESSEDBUNDLE:  return "compressed offload bundle";
    }
    return "unknown status";
}

uint32_t bc250hsa_abi_version_major(void) { return BC250HSA_ABI_VERSION_MAJOR; }
uint32_t bc250hsa_abi_version_minor(void) { return BC250HSA_ABI_VERSION_MINOR; }

/* --------------------------------------------------------------------------
 * The last failed Windows call of this thread
 * ------------------------------------------------------------------------ */

static BC250HSA_THREAD_LOCAL int32_t g_os_status;

void bc250hsa_set_os_status(int32_t status) { g_os_status = status; }
int32_t bc250hsa_last_os_status(void) { return g_os_status; }

/* --------------------------------------------------------------------------
 * The log sink
 * ------------------------------------------------------------------------ */

static bc250hsa_log_fn g_log_fn;
static void*           g_log_ctx;
static uint32_t        g_log_max_level;

void bc250hsa_set_log(bc250hsa_log_fn fn, void* ctx, uint32_t max_level)
{
    g_log_fn = fn;
    g_log_ctx = ctx;
    g_log_max_level = max_level;
}

void bc250hsa_log_enabled_set(void) { /* reserved for a test build */ }

void bc250hsa_log(uint32_t level, const char* format, ...)
{
    char    message[512];
    va_list args;
    int     written;

    if (g_log_fn == NULL || level > g_log_max_level) {
        return;
    }
    va_start(args, format);
    written = vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (written < 0) {
        return;
    }
    message[sizeof(message) - 1] = '\0';
    g_log_fn(g_log_ctx, level, message);
}

/* --------------------------------------------------------------------------
 * The process counters
 * ------------------------------------------------------------------------ */

static volatile int64_t g_counters[BC250HSA_C_COUNT];

void bc250hsa_count_add(bc250hsa_counter which, uint64_t delta)
{
    if ((uint32_t)which >= (uint32_t)BC250HSA_C_COUNT) {
        return;
    }
#if defined(_MSC_VER)
    (void)_InterlockedExchangeAdd64(&g_counters[which], (int64_t)delta);
#else
    g_counters[which] += (int64_t)delta;
#endif
}

void bc250hsa_count_max(bc250hsa_counter which, uint64_t value)
{
    if ((uint32_t)which >= (uint32_t)BC250HSA_C_COUNT) {
        return;
    }
#if defined(_MSC_VER)
    {
        int64_t seen = _InterlockedCompareExchange64(&g_counters[which], 0, 0);
        /* A tick count stays far below 2^63, so the signed slot compares like the unsigned
         * value. The loop ends when this thread wrote its value or another thread wrote a
         * larger one. */
        while ((uint64_t)seen < value) {
            const int64_t before =
                _InterlockedCompareExchange64(&g_counters[which], (int64_t)value, seen);
            if (before == seen) {
                break;
            }
            seen = before;
        }
    }
#else
    if ((uint64_t)g_counters[which] < value) {
        g_counters[which] = (int64_t)value;
    }
#endif
}

uint64_t bc250hsa_ticks(void)
{
#if defined(_WIN32)
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (uint64_t)now.QuadPart;
#else
    return 0u;
#endif
}

/* QueryPerformanceFrequency is fixed at boot, so it is asked once per read and never
 * stored in a counter that a reset could clear. */
static uint64_t ticks_per_second(void)
{
#if defined(_WIN32)
    LARGE_INTEGER frequency;
    if (!QueryPerformanceFrequency(&frequency)) {
        return 0u;
    }
    return (uint64_t)frequency.QuadPart;
#else
    return 0u;
#endif
}

static uint64_t counter_read(bc250hsa_counter which)
{
#if defined(_MSC_VER)
    return (uint64_t)_InterlockedCompareExchange64(&g_counters[which], 0, 0);
#else
    return (uint64_t)g_counters[which];
#endif
}

bc250hsa_status bc250hsa_counters_read(bc250hsa_counters* out)
{
    if (out == NULL || !bc250hsa_struct_bytes_ok(out->struct_bytes, sizeof(*out))) {
        return BC250HSA_EINVAL;
    }
    out->modules_loaded            = counter_read(BC250HSA_C_MODULES_LOADED);
    out->dispatches_built          = counter_read(BC250HSA_C_DISPATCHES_BUILT);
    out->submissions               = counter_read(BC250HSA_C_SUBMISSIONS);
    out->submissions_refused       = counter_read(BC250HSA_C_SUBMISSIONS_REFUSED);
    out->waits                     = counter_read(BC250HSA_C_WAITS);
    out->waits_fast                = counter_read(BC250HSA_C_WAITS_FAST);
    out->waits_timed_out           = counter_read(BC250HSA_C_WAITS_TIMED_OUT);
    out->device_losses             = counter_read(BC250HSA_C_DEVICE_LOSSES);
    out->hidden_args_zeroed        = counter_read(BC250HSA_C_HIDDEN_ARGS_ZEROED);
    out->unknown_arg_kinds         = counter_read(BC250HSA_C_UNKNOWN_ARG_KINDS);
    out->hostcall_buffer_requests  = counter_read(BC250HSA_C_HOSTCALL_BUFFER_REQUESTS);
    out->dynamic_stack_refusals    = counter_read(BC250HSA_C_DYNAMIC_STACK_REFUSALS);
    out->batches_submitted         = counter_read(BC250HSA_C_BATCHES_SUBMITTED);
    out->dispatches_batched        = counter_read(BC250HSA_C_DISPATCHES_BATCHED);
    out->gpu_copy_d2h              = counter_read(BC250HSA_C_GPU_COPY_D2H);
    out->gpu_copy_d2h_bytes        = counter_read(BC250HSA_C_GPU_COPY_D2H_BYTES);
    out->gpu_copy_d2h_staged       = counter_read(BC250HSA_C_GPU_COPY_D2H_STAGED);
    out->gpu_copy_d2h_staged_bytes = counter_read(BC250HSA_C_GPU_COPY_D2H_STAGED_BYTES);
    out->gpu_copy_d2d              = counter_read(BC250HSA_C_GPU_COPY_D2D);
    out->gpu_copy_d2d_bytes        = counter_read(BC250HSA_C_GPU_COPY_D2D_BYTES);
    out->gpu_fill                  = counter_read(BC250HSA_C_GPU_FILL);
    out->gpu_fill_bytes            = counter_read(BC250HSA_C_GPU_FILL_BYTES);
    out->cpu_copy_fallback         = counter_read(BC250HSA_C_CPU_COPY_FALLBACK);
    out->cpu_copy_fallback_bytes   = counter_read(BC250HSA_C_CPU_COPY_FALLBACK_BYTES);
    out->gpu_copy_packets          = counter_read(BC250HSA_C_GPU_COPY_PACKETS);
    out->gpu_copy_forced_flushes   = counter_read(BC250HSA_C_GPU_COPY_FORCED_FLUSHES);
    out->gpu_copy_control_failed   = counter_read(BC250HSA_C_GPU_COPY_CONTROL_FAILED);
    out->ticks_per_second          = ticks_per_second();
    out->waits_fast_ticks          = counter_read(BC250HSA_C_WAITS_FAST_TICKS);
    out->waits_fast_ticks_max      = counter_read(BC250HSA_C_WAITS_FAST_TICKS_MAX);
    out->waits_other_ticks         = counter_read(BC250HSA_C_WAITS_OTHER_TICKS);
    out->waits_other_ticks_max     = counter_read(BC250HSA_C_WAITS_OTHER_TICKS_MAX);
    out->submit_calls              = counter_read(BC250HSA_C_SUBMIT_CALLS);
    out->submit_ticks              = counter_read(BC250HSA_C_SUBMIT_TICKS);
    out->gpu_copy_d2h_ticks        = counter_read(BC250HSA_C_GPU_COPY_D2H_TICKS);
    out->gpu_copy_d2h_staged_ticks = counter_read(BC250HSA_C_GPU_COPY_D2H_STAGED_TICKS);
    out->gpu_copy_d2d_ticks        = counter_read(BC250HSA_C_GPU_COPY_D2D_TICKS);
    out->gpu_fill_ticks            = counter_read(BC250HSA_C_GPU_FILL_TICKS);
    out->cpu_copy_fallback_ticks   = counter_read(BC250HSA_C_CPU_COPY_FALLBACK_TICKS);
    out->gpu_copy_lock_holds       = counter_read(BC250HSA_C_GPU_COPY_LOCK_HOLDS);
    out->gpu_copy_lock_ticks       = counter_read(BC250HSA_C_GPU_COPY_LOCK_TICKS);
    out->gpu_copy_lock_ticks_max   = counter_read(BC250HSA_C_GPU_COPY_LOCK_TICKS_MAX);
    out->gpu_copy_lock_waits       = counter_read(BC250HSA_C_GPU_COPY_LOCK_WAITS);
    out->gpu_copy_lock_wait_ticks  = counter_read(BC250HSA_C_GPU_COPY_LOCK_WAIT_TICKS);
    out->gpu_copy_lock_wait_ticks_max = counter_read(BC250HSA_C_GPU_COPY_LOCK_WAIT_TICKS_MAX);
    out->journal_guard_refusals    = counter_read(BC250HSA_C_JOURNAL_GUARD_REFUSALS);
    out->gpu_copy_unretired        = counter_read(BC250HSA_C_GPU_COPY_UNRETIRED);
    out->gpu_copy_exclusion_refusals = counter_read(BC250HSA_C_GPU_COPY_EXCLUSION_REFUSALS);
    out->gpu_copy_late_retirements = counter_read(BC250HSA_C_GPU_COPY_LATE_RETIREMENTS);
    out->cpu_copy_exclusion_refusals = counter_read(BC250HSA_C_CPU_COPY_EXCLUSION_REFUSALS);
    return BC250HSA_OK;
}

void bc250hsa_count_cpu_copy_fallback(uint64_t bytes, uint64_t ticks)
{
    bc250hsa_count_add(BC250HSA_C_CPU_COPY_FALLBACK, 1u);
    bc250hsa_count_add(BC250HSA_C_CPU_COPY_FALLBACK_BYTES, bytes);
    bc250hsa_count_add(BC250HSA_C_CPU_COPY_FALLBACK_TICKS, ticks);
}

void bc250hsa_count_cpu_copy_exclusion_refusal(void)
{
    bc250hsa_count_add(BC250HSA_C_CPU_COPY_EXCLUSION_REFUSALS, 1u);
}

void bc250hsa_counters_reset(void)
{
    uint32_t i;
    for (i = 0; i < (uint32_t)BC250HSA_C_COUNT; i++) {
#if defined(_MSC_VER)
        (void)_InterlockedExchange64(&g_counters[i], 0);
#else
        g_counters[i] = 0;
#endif
    }
}

/* FreeGpuVirtualAddress permits immediate VA reuse. A timeout or removal sentinel
 * therefore cannot serve as permission to unmap memory a submitted IB may use. */
void bc250hsa_finish_close(const bc250hsa_close_ops* ops)
{
    bc250hsa_status status = ops->flush(ops->ctx);
    if (status == BC250HSA_OK) { status = ops->wait(ops->ctx); }
    if (status == BC250HSA_OK) { ops->release(ops->ctx); }
    else { ops->quarantine(ops->ctx, status); }
}
