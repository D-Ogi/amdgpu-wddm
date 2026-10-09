/* hipmock_backend.c - the mock bc250hsa backend of the layer-2 tests (M16 route B).
 *
 * It implements bc250hsa.h over host memory. A dispatch is recorded and the fence retires at
 * once: the mock runs no instruction. A code object is read for real, because the test of
 * argument packing needs the measured argument offsets of the metadata: the ELF64 header, the
 * PT_LOAD segments, the R_AMDGPU_RELATIVE64 relocations, the .dynsym symbols, the
 * NT_AMDGPU_METADATA note and its MessagePack map.
 *
 * Scope: this file exists so that amdhip64.dll has tests before layer 1 is built. The
 * authoritative loader, packer and PM4 builder are layer 1's, on branch m16/hip-dispatch, and
 * they have their own tests against the same code objects. Nothing here is evidence about the
 * hardware, and the properties it reports name themselves a mock.
 *
 * Threads: every entry point that touches the state of this file takes one recursive lock, so
 * that the multithreaded host test measures the lock of layer 2 and not a race in its own mock.
 * bc250hsa_wait is the exception that matters: it holds the lock only to read, and never while
 * it sleeps.
 *
 * Environment:
 *   BC250_HIP_MOCK_RECORD=<path>  append every record to this file, one line each
 *   BC250_HIP_MOCK_NO_MAP=1       bc250hsa_map refuses, which exercises the host-memory
 *                                 fallback of hipMalloc
 *   BC250_HIP_MOCK_HOLD_MS=<ms>   a dispatch retires this many milliseconds after its
 *                                 submission instead of at once, so that a wait really waits.
 *                                 It is how a program measures what its other threads can do
 *                                 while one of them waits for the device.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "bc250hsa.h"
#include "hipmock_backend.h"

#if defined(_MSC_VER)
#include <malloc.h>
#define MOCK_ALIGNED_ALLOC(bytes, alignment) _aligned_malloc((bytes), (alignment))
#define MOCK_ALIGNED_FREE(p) _aligned_free(p)
#else
#include <stdlib.h>
#define MOCK_ALIGNED_ALLOC(bytes, alignment) aligned_alloc((alignment), (bytes))
#define MOCK_ALIGNED_FREE(p) free(p)
#endif

/* ------------------------------------------------------------------------------------------
 * State
 * ---------------------------------------------------------------------------------------- */

#define MOCK_MAX_ALLOCATIONS 1024u
#define MOCK_MAX_KERNELS 16u
#define MOCK_MAX_ARGS 64u
#define MOCK_MAX_SYMBOLS 128u
#define MOCK_NAME_MAX 96u

typedef struct mock_allocation {
    int          live;
    void*        raw;
    bc250hsa_mem mem;
} mock_allocation;

/* A dispatch that has not retired yet, when a hold time is in effect. The values retire in the
 * order they were submitted, so the list is a queue. */
#define MOCK_MAX_PENDING_FENCES 64u

typedef struct mock_pending_fence {
    uint64_t value;
    uint64_t due_ms;
} mock_pending_fence;

struct bc250hsa_device {
    uint32_t        magic;
    uint32_t        opens;
    uint64_t        next_va;
    uint64_t        fence;      /* the last submitted value */
    uint64_t        retired;    /* the value the device has reached */
    uint32_t        hold_ms;    /* 0: a dispatch retires at once, as it always did */
    mock_pending_fence pending[MOCK_MAX_PENDING_FENCES];
    uint32_t        pending_count;
    mock_allocation allocations[MOCK_MAX_ALLOCATIONS];
    uint32_t        live_allocations;
    uint64_t        live_bytes;
};

typedef struct mock_symbol {
    char     name[MOCK_NAME_MAX];
    uint64_t va;
    uint64_t bytes;
} mock_symbol;

struct bc250hsa_module {
    bc250hsa_allocator allocator;
    bc250hsa_mem       code;
    uint64_t           base_va;
    uint64_t           span;
    uint32_t           kernel_count;
    bc250hsa_kernel    kernels[MOCK_MAX_KERNELS];
    char               names[MOCK_MAX_KERNELS][MOCK_NAME_MAX];
    char               symbols[MOCK_MAX_KERNELS][MOCK_NAME_MAX];
    bc250hsa_arg       args[MOCK_MAX_KERNELS][MOCK_MAX_ARGS];
    uint32_t           symbol_count;
    mock_symbol        symbol_table[MOCK_MAX_SYMBOLS];
};

#define MOCK_DEVICE_MAGIC 0x4D4F434Bu /* 'MOCK' */

static struct bc250hsa_device g_device;
static bc250hsa_counters      g_counters;
static bc250hsa_mock_record   g_records[BC250HSA_MOCK_RECORD_MAX];
static uint32_t               g_record_count;
static bc250hsa_log_fn        g_log_fn;
static void*                  g_log_ctx;
static uint32_t               g_log_level;
static int32_t                g_os_status;
static FILE*                  g_record_file;
static int                    g_record_file_tried;

/* ------------------------------------------------------------------------------------------
 * The lock of the mock, and the clock
 *
 * One recursive lock. It is recursive because bc250hsa_module_load allocates through the
 * device allocator, which is bc250hsa_alloc, and both take it. The depth and the owner are
 * written only by the thread that holds the lock, and the only comparison a thread makes is
 * against its own thread id, which no other thread ever writes there.
 * ---------------------------------------------------------------------------------------- */

#if defined(_WIN32)
static SRWLOCK       g_lock = SRWLOCK_INIT;
static volatile LONG g_lock_owner;
static uint32_t      g_lock_depth;

static void mock_lock(void) {
    const LONG self = (LONG)GetCurrentThreadId();
    if (g_lock_owner == self) {
        g_lock_depth++;
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    InterlockedExchange(&g_lock_owner, self);
    g_lock_depth = 1u;
}

static void mock_unlock(void) {
    if (--g_lock_depth != 0u) {
        return;
    }
    InterlockedExchange(&g_lock_owner, 0);
    ReleaseSRWLockExclusive(&g_lock);
}

static uint64_t mock_now_ms(void) { return (uint64_t)GetTickCount64(); }
static void     mock_sleep_ms(uint32_t ms) { Sleep(ms); }
#else
/* The host tests of this component build on Windows only. A build elsewhere gets a single
 * threaded mock, which is what it had before. */
static void     mock_lock(void) {}
static void     mock_unlock(void) {}
static uint64_t mock_now_ms(void) { return 0u; }
static void     mock_sleep_ms(uint32_t ms) { (void)ms; }
#endif

/* ------------------------------------------------------------------------------------------
 * Small helpers
 * ---------------------------------------------------------------------------------------- */

static uint64_t align_up(uint64_t value, uint64_t alignment) {
    if (alignment <= 1u) {
        return value;
    }
    return (value + alignment - 1u) / alignment * alignment;
}

static uint16_t rd16(const unsigned char* p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static uint32_t rd32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const unsigned char* p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static void copy_name(char* dst, size_t dst_bytes, const char* src, size_t src_bytes) {
    size_t n = src_bytes;
    if (n >= dst_bytes) {
        n = dst_bytes - 1u;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static const char* record_kind_names[] = {"alloc",    "free", "module_load", "module_unload",
                                          "dispatch", "wait", "copy_to",     "copy_from"};

const char* bc250hsa_mock_kind_name(uint32_t kind) {
    if (kind >= sizeof(record_kind_names) / sizeof(record_kind_names[0])) {
        return "unknown";
    }
    return record_kind_names[kind];
}

static void record_line(const bc250hsa_mock_record* record) {
    if (!g_record_file_tried) {
        const char* path = NULL;
        g_record_file_tried = 1;
#if defined(_MSC_VER)
        size_t needed = 0;
        static char buffer[1024];
        if (getenv_s(&needed, buffer, sizeof(buffer), "BC250_HIP_MOCK_RECORD") == 0 &&
            needed > 1u) {
            path = buffer;
        }
#else
        path = getenv("BC250_HIP_MOCK_RECORD");
#endif
        if (path != NULL && path[0] != '\0') {
#if defined(_MSC_VER)
            if (fopen_s(&g_record_file, path, "w") != 0) {
                g_record_file = NULL;
            }
#else
            g_record_file = fopen(path, "w");
#endif
        }
    }
    if (g_record_file == NULL) {
        return;
    }
    fprintf(g_record_file, "%s kernel=%s grid=%u,%u,%u block=%u,%u,%u lds=%u va=0x%llx bytes=%llu value=%llu wait=%u/%u kernarg=%u",
            bc250hsa_mock_kind_name(record->kind), record->kernel[0] != '\0' ? record->kernel : "-",
            record->grid[0], record->grid[1], record->grid[2], record->block[0], record->block[1],
            record->block[2], record->dynamic_group_bytes, (unsigned long long)record->va,
            (unsigned long long)record->bytes, (unsigned long long)record->value,
            record->wait_slice_ms, record->wait_total_ms, record->kernarg_bytes);
    if (record->kernarg_bytes != 0u) {
        uint32_t n = record->kernarg_bytes;
        uint32_t i;
        if (n > 64u) {
            n = 64u;
        }
        fprintf(g_record_file, " bytes=");
        for (i = 0; i < n; ++i) {
            fprintf(g_record_file, "%02x", record->kernarg[i]);
        }
    }
    fprintf(g_record_file, "\n");
    fflush(g_record_file);
}

static bc250hsa_mock_record* record_new(uint32_t kind) {
    bc250hsa_mock_record* record;
    if (g_record_count >= BC250HSA_MOCK_RECORD_MAX) {
        return NULL;
    }
    record = &g_records[g_record_count++];
    memset(record, 0, sizeof(*record));
    record->kind = kind;
    return record;
}

/* A number of milliseconds from the operating system's environment. It reads the environment
 * block and not getenv for the same measured reason as layer 2 does: a program that links the
 * static C runtime and a DLL that links the dynamic one have one environment copy each, and a
 * _putenv_s of the program never reaches a getenv of the DLL. */
static uint32_t mock_env_ms(const char* name) {
#if defined(_WIN32)
    char        text[32];
    const DWORD bytes = GetEnvironmentVariableA(name, text, (DWORD)sizeof(text));
    if (bytes == 0u || bytes >= (DWORD)sizeof(text)) {
        return 0u;
    }
    return (uint32_t)strtoul(text, NULL, 10);
#else
    (void)name;
    return 0u;
#endif
}

/* Retires every pending fence value whose hold time has passed. The caller holds the lock. */
static void mock_retire_due(bc250hsa_device* dev) {
    const uint64_t now = mock_now_ms();
    uint32_t       done = 0;
    while (done < dev->pending_count && dev->pending[done].due_ms <= now) {
        dev->retired = dev->pending[done].value;
        done++;
    }
    if (done != 0u) {
        uint32_t i;
        for (i = 0; i + done < dev->pending_count; ++i) {
            dev->pending[i] = dev->pending[i + done];
        }
        dev->pending_count -= done;
    }
}

static void log_line(uint32_t level, const char* message) {
    if (g_log_fn != NULL && level <= g_log_level) {
        g_log_fn(g_log_ctx, level, message);
    }
}

/* ------------------------------------------------------------------------------------------
 * Status, version, log, counters
 * ---------------------------------------------------------------------------------------- */

uint32_t bc250hsa_abi_version_major(void) { return BC250HSA_ABI_VERSION_MAJOR; }
uint32_t bc250hsa_abi_version_minor(void) { return BC250HSA_ABI_VERSION_MINOR; }

const char* bc250hsa_status_string(bc250hsa_status status) {
    switch (status) {
    case BC250HSA_OK:                 return "ok";
    case BC250HSA_EINVAL:             return "invalid parameter";
    case BC250HSA_ENOMEM:             return "out of memory";
    case BC250HSA_ENODEV:             return "no device";
    case BC250HSA_ETIMEOUT:           return "the bounded wait ran out";
    case BC250HSA_EDEVICELOST:        return "the device is lost";
    case BC250HSA_EUNSUPPORTED:       return "not supported by this build";
    case BC250HSA_EOS:                return "a Windows call failed";
    case BC250HSA_ENOTFOUND:          return "not found";
    case BC250HSA_EBUSY:              return "no free ring slot";
    case BC250HSA_EBADELF:            return "not an AMDGPU ELF64 code object";
    case BC250HSA_EWRONGTARGET:       return "the code object names another processor";
    case BC250HSA_EBADABIVERSION:     return "unsupported code object ABI version";
    case BC250HSA_EBADRELOC:          return "unsupported relocation";
    case BC250HSA_EBADMETADATA:       return "the metadata note does not parse";
    case BC250HSA_EBADBUNDLE:         return "the offload bundle does not parse";
    case BC250HSA_ECOMPRESSEDBUNDLE:  return "a compressed offload bundle";
    default:                          break;
    }
    return "unknown status";
}

int32_t bc250hsa_last_os_status(void) { return g_os_status; }

void bc250hsa_set_log(bc250hsa_log_fn fn, void* ctx, uint32_t max_level) {
    g_log_fn = fn;
    g_log_ctx = ctx;
    g_log_level = max_level;
}

bc250hsa_status bc250hsa_counters_read(bc250hsa_counters* out) {
    if (out == NULL || out->struct_bytes != (uint32_t)sizeof(*out)) {
        return BC250HSA_EINVAL;
    }
    g_counters.struct_bytes = (uint32_t)sizeof(g_counters);
    memcpy(out, &g_counters, sizeof(*out));
    return BC250HSA_OK;
}

void bc250hsa_counters_reset(void) {
    memset(&g_counters, 0, sizeof(g_counters));
    g_counters.struct_bytes = (uint32_t)sizeof(g_counters);
}

/* ------------------------------------------------------------------------------------------
 * Device
 * ---------------------------------------------------------------------------------------- */

bc250hsa_status bc250hsa_open(const bc250hsa_open_params* params, bc250hsa_device** out) {
    if (out == NULL) {
        return BC250HSA_EINVAL;
    }
    if (params != NULL && params->struct_bytes != (uint32_t)sizeof(*params)) {
        return BC250HSA_EINVAL;
    }
    mock_lock();
    if (g_device.magic != MOCK_DEVICE_MAGIC) {
        memset(&g_device, 0, sizeof(g_device));
        g_device.magic = MOCK_DEVICE_MAGIC;
        /* A synthetic GPU address window, far from any host pointer. */
        g_device.next_va = 0x0000400000000000ull;
        g_device.hold_ms = mock_env_ms("BC250_HIP_MOCK_HOLD_MS");
    }
    g_device.opens++;
    *out = &g_device;
    mock_unlock();
    log_line(BC250HSA_LOG_INFO, "mock device open");
    return BC250HSA_OK;
}

void bc250hsa_close(bc250hsa_device* dev) {
    uint32_t i;
    if (dev == NULL || dev->magic != MOCK_DEVICE_MAGIC) {
        return;
    }
    mock_lock();
    if (dev->opens > 1u) {
        dev->opens--;
        mock_unlock();
        return;
    }
    for (i = 0; i < MOCK_MAX_ALLOCATIONS; ++i) {
        if (dev->allocations[i].live) {
            MOCK_ALIGNED_FREE(dev->allocations[i].raw);
            dev->allocations[i].live = 0;
        }
    }
    dev->live_allocations = 0;
    dev->live_bytes = 0;
    dev->opens = 0;
    mock_unlock();
}

bc250hsa_status bc250hsa_props_read(bc250hsa_device* dev, bc250hsa_props* out) {
    if (dev == NULL || dev->magic != MOCK_DEVICE_MAGIC || out == NULL) {
        return BC250HSA_EINVAL;
    }
    if (out->struct_bytes != (uint32_t)sizeof(*out)) {
        return BC250HSA_EINVAL;
    }
    memset(out->name, 0, sizeof(out->name));
    /* The name says "mock" on purpose: no number of this structure is a measurement. */
    {
        static const char mock_name[] = "AMD BC-250 (mock device)";
        copy_name(out->name, sizeof(out->name), mock_name, sizeof(mock_name) - 1u);
    }
    out->gfx_ip_major = 10u;
    out->gfx_ip_minor = 1u;
    out->gfx_ip_rev = 3u;
    out->pci_bus = 0u;
    out->pci_device = 0u;
    out->pci_function = 0u;
    out->wave_size = 32u;
    out->cu_count = 40u;
    out->se_count = 2u;
    out->max_workgroup_size = 1024u;
    out->max_workgroups_per_dim = 0x7FFFFFFFu;
    out->lds_bytes_per_workgroup = 65536u;
    out->waves_per_cu = 32u;
    out->vram_bytes = 8ull * 1024ull * 1024ull * 1024ull;
    out->visible_vram_bytes = out->vram_bytes;
    out->gtt_bytes = 4ull * 1024ull * 1024ull * 1024ull;
    out->gfx_clock_khz = 1000000u;
    out->mem_clock_khz = 1000000u;
    out->mem_bus_width = 128u;
    out->kmd_version[0] = 0u;
    out->kmd_version[1] = 0u;
    out->kmd_version[2] = 0u;
    out->kmd_version[3] = 0u;
    return BC250HSA_OK;
}

/* ------------------------------------------------------------------------------------------
 * Memory
 * ---------------------------------------------------------------------------------------- */

static int mock_no_map(void) {
#if defined(_MSC_VER)
    size_t needed = 0;
    char buffer[8];
    if (getenv_s(&needed, buffer, sizeof(buffer), "BC250_HIP_MOCK_NO_MAP") == 0 && needed > 1u) {
        return buffer[0] == '1';
    }
    return 0;
#else
    const char* value = getenv("BC250_HIP_MOCK_NO_MAP");
    return value != NULL && value[0] == '1';
#endif
}

static mock_allocation* allocation_of(bc250hsa_device* dev, const bc250hsa_mem* mem) {
    uint32_t i;
    for (i = 0; i < MOCK_MAX_ALLOCATIONS; ++i) {
        if (dev->allocations[i].live && dev->allocations[i].mem.va == mem->va) {
            return &dev->allocations[i];
        }
    }
    return NULL;
}

static bc250hsa_status alloc_locked(bc250hsa_device* dev, uint64_t bytes, uint64_t alignment,
                                    uint32_t flags, bc250hsa_mem* out) {
    uint32_t slot;
    uint64_t aligned_bytes;
    void* raw;
    if (dev == NULL || dev->magic != MOCK_DEVICE_MAGIC || out == NULL || bytes == 0u) {
        return BC250HSA_EINVAL;
    }
    if (alignment == 0u) {
        alignment = (flags & BC250HSA_MEM_EXEC) != 0u ? 4096u : 256u;
    }
    if ((alignment & (alignment - 1u)) != 0u) {
        return BC250HSA_EINVAL;
    }
    for (slot = 0; slot < MOCK_MAX_ALLOCATIONS; ++slot) {
        if (!dev->allocations[slot].live) {
            break;
        }
    }
    if (slot == MOCK_MAX_ALLOCATIONS) {
        return BC250HSA_ENOMEM;
    }
    aligned_bytes = align_up(bytes, alignment);
    raw = MOCK_ALIGNED_ALLOC((size_t)aligned_bytes, (size_t)alignment);
    if (raw == NULL) {
        return BC250HSA_ENOMEM;
    }
    if ((flags & BC250HSA_MEM_ZERO) != 0u || (flags & BC250HSA_MEM_EXEC) != 0u) {
        memset(raw, 0, (size_t)aligned_bytes);
    }
    memset(out, 0, sizeof(*out));
    dev->next_va = align_up(dev->next_va, alignment < 4096u ? 4096u : alignment);
    out->va = dev->next_va;
    dev->next_va += align_up(aligned_bytes, 4096u);
    /* A device-local allocation reports no host pointer, as the contract says. The host path
     * and the executable range report one. */
    out->host = (flags == BC250HSA_MEM_DEVICE || (flags & BC250HSA_MEM_MAPPABLE) != 0u) ? NULL
                                                                                        : raw;
    out->bytes = aligned_bytes;
    out->flags = flags;
    out->opaque = &dev->allocations[slot];

    dev->allocations[slot].live = 1;
    dev->allocations[slot].raw = raw;
    dev->allocations[slot].mem = *out;
    dev->live_allocations++;
    dev->live_bytes += aligned_bytes;

    {
        bc250hsa_mock_record* record = record_new(BC250HSA_MOCK_ALLOC);
        if (record != NULL) {
            record->va = out->va;
            record->bytes = aligned_bytes;
            record->value = flags;
            record_line(record);
        }
    }
    return BC250HSA_OK;
}

bc250hsa_status bc250hsa_alloc(bc250hsa_device* dev, uint64_t bytes, uint64_t alignment,
                               uint32_t flags, bc250hsa_mem* out) {
    bc250hsa_status result;
    mock_lock();
    result = alloc_locked(dev, bytes, alignment, flags, out);
    mock_unlock();
    return result;
}

static bc250hsa_status free_locked(bc250hsa_device* dev, bc250hsa_mem* mem) {
    mock_allocation* allocation;
    if (dev == NULL || dev->magic != MOCK_DEVICE_MAGIC || mem == NULL) {
        return BC250HSA_EINVAL;
    }
    allocation = allocation_of(dev, mem);
    if (allocation == NULL) {
        return BC250HSA_EINVAL;
    }
    {
        bc250hsa_mock_record* record = record_new(BC250HSA_MOCK_FREE);
        if (record != NULL) {
            record->va = allocation->mem.va;
            record->bytes = allocation->mem.bytes;
            record_line(record);
        }
    }
    MOCK_ALIGNED_FREE(allocation->raw);
    dev->live_allocations--;
    dev->live_bytes -= allocation->mem.bytes;
    allocation->live = 0;
    allocation->raw = NULL;
    memset(mem, 0, sizeof(*mem));
    return BC250HSA_OK;
}

bc250hsa_status bc250hsa_free(bc250hsa_device* dev, bc250hsa_mem* mem) {
    bc250hsa_status result;
    mock_lock();
    result = free_locked(dev, mem);
    mock_unlock();
    return result;
}

static bc250hsa_status map_locked(bc250hsa_device* dev, bc250hsa_mem* mem, void** out) {
    mock_allocation* allocation;
    if (dev == NULL || dev->magic != MOCK_DEVICE_MAGIC || mem == NULL || out == NULL) {
        return BC250HSA_EINVAL;
    }
    if (mem->flags == BC250HSA_MEM_DEVICE) {
        return BC250HSA_EUNSUPPORTED;
    }
    if (mock_no_map()) {
        /* The answer of open question 1 of the design may be no. This path lets the test run
         * the fallback of hipMalloc. */
        return BC250HSA_EUNSUPPORTED;
    }
    allocation = allocation_of(dev, mem);
    if (allocation == NULL) {
        return BC250HSA_EINVAL;
    }
    /* Only *out, which is the one result that bc250hsa.h promises of this call. The handle of
     * the caller stays as it was on purpose: a caller that reads mem->host after a map instead
     * of *out must fail here and not on the lab. */
    *out = allocation->raw;
    return BC250HSA_OK;
}

bc250hsa_status bc250hsa_map(bc250hsa_device* dev, bc250hsa_mem* mem, void** out) {
    bc250hsa_status result;
    mock_lock();
    result = map_locked(dev, mem, out);
    mock_unlock();
    return result;
}

static bc250hsa_status unmap_locked(bc250hsa_device* dev, bc250hsa_mem* mem) {
    mock_allocation* allocation;
    if (dev == NULL || dev->magic != MOCK_DEVICE_MAGIC || mem == NULL) {
        return BC250HSA_EINVAL;
    }
    allocation = allocation_of(dev, mem);
    if (allocation == NULL) {
        return BC250HSA_EINVAL;
    }
    mem->host = NULL;
    allocation->mem.host = NULL;
    return BC250HSA_OK;
}

bc250hsa_status bc250hsa_unmap(bc250hsa_device* dev, bc250hsa_mem* mem) {
    bc250hsa_status result;
    mock_lock();
    result = unmap_locked(dev, mem);
    mock_unlock();
    return result;
}

void bc250hsa_write_barrier(void) { /* nothing to drain in host memory */ }

static bc250hsa_status copy_locked(bc250hsa_device* dev, const bc250hsa_mem* mem, uint64_t offset,
                                   void* host_side, uint64_t bytes, int to_device) {
    mock_allocation* allocation;
    unsigned char* base;
    if (dev == NULL || dev->magic != MOCK_DEVICE_MAGIC || mem == NULL || host_side == NULL) {
        return BC250HSA_EINVAL;
    }
    allocation = allocation_of(dev, mem);
    if (allocation == NULL) {
        return BC250HSA_EINVAL;
    }
    if (mem->host == NULL && allocation->mem.host == NULL) {
        return BC250HSA_EUNSUPPORTED;
    }
    if (offset > allocation->mem.bytes || bytes > allocation->mem.bytes - offset) {
        return BC250HSA_EINVAL;
    }
    base = (unsigned char*)allocation->raw + offset;
    if (to_device) {
        memcpy(base, host_side, (size_t)bytes);
    } else {
        memcpy(host_side, base, (size_t)bytes);
    }
    {
        bc250hsa_mock_record* record = record_new(to_device ? BC250HSA_MOCK_COPY_TO_DEVICE
                                                            : BC250HSA_MOCK_COPY_FROM_DEVICE);
        if (record != NULL) {
            record->va = allocation->mem.va + offset;
            record->bytes = bytes;
            record_line(record);
        }
    }
    return BC250HSA_OK;
}

static bc250hsa_status mock_copy(bc250hsa_device* dev, const bc250hsa_mem* mem, uint64_t offset,
                                 void* host_side, uint64_t bytes, int to_device) {
    bc250hsa_status result;
    mock_lock();
    result = copy_locked(dev, mem, offset, host_side, bytes, to_device);
    mock_unlock();
    return result;
}

bc250hsa_status bc250hsa_copy_to_device(bc250hsa_device* dev, const bc250hsa_mem* dst,
                                        uint64_t dst_offset, const void* src, uint64_t bytes) {
    /* The mock copies in host memory, so the const of the source is removed one time here. */
    return mock_copy(dev, dst, dst_offset, (void*)(uintptr_t)src, bytes, 1);
}

bc250hsa_status bc250hsa_copy_from_device(bc250hsa_device* dev, void* dst,
                                          const bc250hsa_mem* src, uint64_t src_offset,
                                          uint64_t bytes) {
    return mock_copy(dev, src, src_offset, dst, bytes, 0);
}

static bc250hsa_status device_alloc_shim(void* ctx, uint64_t bytes, uint64_t alignment,
                                         uint32_t flags, bc250hsa_mem* out) {
    return bc250hsa_alloc((bc250hsa_device*)ctx, bytes, alignment, flags, out);
}

static void device_free_shim(void* ctx, bc250hsa_mem* mem) {
    (void)bc250hsa_free((bc250hsa_device*)ctx, mem);
}

bc250hsa_status bc250hsa_device_allocator(bc250hsa_device* dev, bc250hsa_allocator* out) {
    if (dev == NULL || dev->magic != MOCK_DEVICE_MAGIC || out == NULL) {
        return BC250HSA_EINVAL;
    }
    out->ctx = dev;
    out->alloc = device_alloc_shim;
    out->free = device_free_shim;
    return BC250HSA_OK;
}

/* ------------------------------------------------------------------------------------------
 * The offload bundle reader
 * ---------------------------------------------------------------------------------------- */

static const char kBundleMagic[] = "__CLANG_OFFLOAD_BUNDLE__";
static const char kCompressedMagic[] = "CCOB";

bc250hsa_status bc250hsa_unbundle(const void* fatbin, size_t fatbin_bytes, const char* target_id,
                                  const void** image, size_t* image_bytes) {
    const unsigned char* p = (const unsigned char*)fatbin;
    const size_t magic_bytes = sizeof(kBundleMagic) - 1u;
    uint64_t count;
    uint64_t at;
    uint64_t i;
    const void* best = NULL;
    size_t best_bytes = 0;
    int best_exact = 0;

    if (fatbin == NULL || image == NULL || image_bytes == NULL) {
        return BC250HSA_EINVAL;
    }
    if (target_id == NULL) {
        target_id = "gfx1013";
    }
    if (fatbin_bytes >= 4u && memcmp(p, kCompressedMagic, 4u) == 0) {
        return BC250HSA_ECOMPRESSEDBUNDLE;
    }
    if (fatbin_bytes < magic_bytes + 8u || memcmp(p, kBundleMagic, magic_bytes) != 0) {
        return BC250HSA_EBADBUNDLE;
    }
    count = rd64(p + magic_bytes);
    if (count == 0u || count > 64u) {
        return BC250HSA_EBADBUNDLE;
    }
    at = magic_bytes + 8u;
    for (i = 0; i < count; ++i) {
        uint64_t offset;
        uint64_t size;
        uint64_t id_bytes;
        const char* id;
        if (at + 24u > fatbin_bytes) {
            return BC250HSA_EBADBUNDLE;
        }
        offset = rd64(p + at);
        size = rd64(p + at + 8u);
        id_bytes = rd64(p + at + 16u);
        at += 24u;
        if (at + id_bytes > fatbin_bytes) {
            return BC250HSA_EBADBUNDLE;
        }
        id = (const char*)(p + at);
        at += id_bytes;
        /* MEASURED trap: the host entry has size 0 and both entries may name the same offset. */
        if (size == 0u || offset + size > fatbin_bytes) {
            continue;
        }
        if (id_bytes >= 4u && memcmp(id, "host", 4u) == 0) {
            continue;
        }
        if (id_bytes >= strlen(target_id) &&
            memcmp(id + id_bytes - strlen(target_id), target_id, strlen(target_id)) == 0) {
            best = p + offset;
            best_bytes = (size_t)size;
            best_exact = 1;
            break;
        }
        if (!best_exact && id_bytes > 7u && memcmp(id, "hipv", 4u) == 0) {
            /* A generic processor, such as gfx10-1-generic. An exact match wins over it. */
            best = p + offset;
            best_bytes = (size_t)size;
        }
    }
    if (best == NULL) {
        return BC250HSA_EWRONGTARGET;
    }
    *image = best;
    *image_bytes = best_bytes;
    return BC250HSA_OK;
}

/* ------------------------------------------------------------------------------------------
 * A MessagePack reader, for the NT_AMDGPU_METADATA note
 * ---------------------------------------------------------------------------------------- */

typedef struct mp_cursor {
    const unsigned char* p;
    size_t               n;
    size_t               at;
} mp_cursor;

static int mp_take(mp_cursor* m, size_t bytes, const unsigned char** out) {
    if (m->at + bytes > m->n) {
        return 0;
    }
    *out = m->p + m->at;
    m->at += bytes;
    return 1;
}

static int mp_be(mp_cursor* m, size_t bytes, uint64_t* out) {
    const unsigned char* p;
    size_t i;
    uint64_t v = 0;
    if (!mp_take(m, bytes, &p)) {
        return 0;
    }
    for (i = 0; i < bytes; ++i) {
        v = (v << 8) | (uint64_t)p[i];
    }
    *out = v;
    return 1;
}

static int mp_tag(mp_cursor* m, unsigned char* tag) {
    const unsigned char* p;
    if (!mp_take(m, 1u, &p)) {
        return 0;
    }
    *tag = *p;
    return 1;
}

static int mp_map(mp_cursor* m, uint64_t* len) {
    unsigned char tag;
    if (!mp_tag(m, &tag)) {
        return 0;
    }
    if ((tag & 0xF0u) == 0x80u) {
        *len = tag & 0x0Fu;
        return 1;
    }
    if (tag == 0xDEu) {
        return mp_be(m, 2u, len);
    }
    if (tag == 0xDFu) {
        return mp_be(m, 4u, len);
    }
    return 0;
}

static int mp_array(mp_cursor* m, uint64_t* len) {
    unsigned char tag;
    if (!mp_tag(m, &tag)) {
        return 0;
    }
    if ((tag & 0xF0u) == 0x90u) {
        *len = tag & 0x0Fu;
        return 1;
    }
    if (tag == 0xDCu) {
        return mp_be(m, 2u, len);
    }
    if (tag == 0xDDu) {
        return mp_be(m, 4u, len);
    }
    return 0;
}

static int mp_str(mp_cursor* m, const char** s, size_t* len) {
    unsigned char tag;
    uint64_t size = 0;
    const unsigned char* p;
    if (!mp_tag(m, &tag)) {
        return 0;
    }
    if ((tag & 0xE0u) == 0xA0u) {
        size = tag & 0x1Fu;
    } else if (tag == 0xD9u) {
        if (!mp_be(m, 1u, &size)) {
            return 0;
        }
    } else if (tag == 0xDAu) {
        if (!mp_be(m, 2u, &size)) {
            return 0;
        }
    } else if (tag == 0xDBu) {
        if (!mp_be(m, 4u, &size)) {
            return 0;
        }
    } else {
        return 0;
    }
    if (!mp_take(m, (size_t)size, &p)) {
        return 0;
    }
    *s = (const char*)p;
    *len = (size_t)size;
    return 1;
}

/* Reads an unsigned integer, and also a boolean, which the metadata uses for
 * .uses_dynamic_stack and .workgroup_processor_mode. */
static int mp_uint(mp_cursor* m, uint64_t* out) {
    unsigned char tag;
    if (!mp_tag(m, &tag)) {
        return 0;
    }
    if (tag <= 0x7Fu) {
        *out = tag;
        return 1;
    }
    if (tag == 0xC2u) {
        *out = 0;
        return 1;
    }
    if (tag == 0xC3u) {
        *out = 1;
        return 1;
    }
    if (tag == 0xCCu) {
        return mp_be(m, 1u, out);
    }
    if (tag == 0xCDu) {
        return mp_be(m, 2u, out);
    }
    if (tag == 0xCEu) {
        return mp_be(m, 4u, out);
    }
    if (tag == 0xCFu) {
        return mp_be(m, 8u, out);
    }
    return 0;
}

static int mp_skip(mp_cursor* m) {
    unsigned char tag;
    uint64_t size = 0;
    const unsigned char* p;
    size_t saved = m->at;
    if (!mp_tag(m, &tag)) {
        return 0;
    }
    if (tag <= 0x7Fu || tag >= 0xE0u) {
        return 1; /* fixint */
    }
    if ((tag & 0xF0u) == 0x80u || tag == 0xDEu || tag == 0xDFu) {
        uint64_t i;
        m->at = saved;
        if (!mp_map(m, &size)) {
            return 0;
        }
        for (i = 0; i < size; ++i) {
            if (!mp_skip(m) || !mp_skip(m)) {
                return 0;
            }
        }
        return 1;
    }
    if ((tag & 0xF0u) == 0x90u || tag == 0xDCu || tag == 0xDDu) {
        uint64_t i;
        m->at = saved;
        if (!mp_array(m, &size)) {
            return 0;
        }
        for (i = 0; i < size; ++i) {
            if (!mp_skip(m)) {
                return 0;
            }
        }
        return 1;
    }
    if ((tag & 0xE0u) == 0xA0u || tag == 0xD9u || tag == 0xDAu || tag == 0xDBu) {
        const char* s;
        size_t len;
        m->at = saved;
        return mp_str(m, &s, &len);
    }
    switch (tag) {
    case 0xC0u: /* nil */
    case 0xC2u:
    case 0xC3u:
        return 1;
    case 0xCCu: case 0xD0u: return mp_take(m, 1u, &p);
    case 0xCDu: case 0xD1u: return mp_take(m, 2u, &p);
    case 0xCEu: case 0xD2u: case 0xCAu: return mp_take(m, 4u, &p);
    case 0xCFu: case 0xD3u: case 0xCBu: return mp_take(m, 8u, &p);
    case 0xC4u: if (!mp_be(m, 1u, &size)) { return 0; } return mp_take(m, (size_t)size, &p);
    case 0xC5u: if (!mp_be(m, 2u, &size)) { return 0; } return mp_take(m, (size_t)size, &p);
    case 0xC6u: if (!mp_be(m, 4u, &size)) { return 0; } return mp_take(m, (size_t)size, &p);
    default:    break;
    }
    return 0;
}

static int key_is(const char* key, size_t key_bytes, const char* want) {
    return key_bytes == strlen(want) && memcmp(key, want, key_bytes) == 0;
}

static uint16_t arg_kind_of(const char* s, size_t n) {
    static const struct { const char* name; uint16_t kind; } table[] = {
        {"by_value", BC250HSA_ARG_BY_VALUE},
        {"global_buffer", BC250HSA_ARG_GLOBAL_BUFFER},
        {"dynamic_shared_pointer", BC250HSA_ARG_DYNAMIC_SHARED_POINTER},
        {"sampler", BC250HSA_ARG_SAMPLER},
        {"image", BC250HSA_ARG_IMAGE},
        {"pipe", BC250HSA_ARG_PIPE},
        {"queue", BC250HSA_ARG_QUEUE},
        {"hidden_block_count_x", BC250HSA_ARG_HIDDEN_BLOCK_COUNT_X},
        {"hidden_block_count_y", BC250HSA_ARG_HIDDEN_BLOCK_COUNT_Y},
        {"hidden_block_count_z", BC250HSA_ARG_HIDDEN_BLOCK_COUNT_Z},
        {"hidden_group_size_x", BC250HSA_ARG_HIDDEN_GROUP_SIZE_X},
        {"hidden_group_size_y", BC250HSA_ARG_HIDDEN_GROUP_SIZE_Y},
        {"hidden_group_size_z", BC250HSA_ARG_HIDDEN_GROUP_SIZE_Z},
        {"hidden_remainder_x", BC250HSA_ARG_HIDDEN_REMAINDER_X},
        {"hidden_remainder_y", BC250HSA_ARG_HIDDEN_REMAINDER_Y},
        {"hidden_remainder_z", BC250HSA_ARG_HIDDEN_REMAINDER_Z},
        {"hidden_global_offset_x", BC250HSA_ARG_HIDDEN_GLOBAL_OFFSET_X},
        {"hidden_global_offset_y", BC250HSA_ARG_HIDDEN_GLOBAL_OFFSET_Y},
        {"hidden_global_offset_z", BC250HSA_ARG_HIDDEN_GLOBAL_OFFSET_Z},
        {"hidden_grid_dims", BC250HSA_ARG_HIDDEN_GRID_DIMS},
        {"hidden_dynamic_lds_size", BC250HSA_ARG_HIDDEN_DYNAMIC_LDS_SIZE},
        {"hidden_printf_buffer", BC250HSA_ARG_HIDDEN_PRINTF_BUFFER},
        {"hidden_hostcall_buffer", BC250HSA_ARG_HIDDEN_HOSTCALL_BUFFER},
        {"hidden_heap_v1", BC250HSA_ARG_HIDDEN_HEAP_V1},
        {"hidden_default_queue", BC250HSA_ARG_HIDDEN_DEFAULT_QUEUE},
        {"hidden_completion_action", BC250HSA_ARG_HIDDEN_COMPLETION_ACTION},
        {"hidden_multigrid_sync_arg", BC250HSA_ARG_HIDDEN_MULTIGRID_SYNC_ARG},
        {"hidden_queue_ptr", BC250HSA_ARG_HIDDEN_QUEUE_PTR},
        {"hidden_private_base", BC250HSA_ARG_HIDDEN_PRIVATE_BASE},
        {"hidden_shared_base", BC250HSA_ARG_HIDDEN_SHARED_BASE},
    };
    size_t i;
    for (i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        if (key_is(s, n, table[i].name)) {
            return table[i].kind;
        }
    }
    return BC250HSA_ARG_HIDDEN_OTHER;
}

static uint8_t address_space_of(const char* s, size_t n) {
    if (key_is(s, n, "global")) {
        return BC250HSA_AS_GLOBAL;
    }
    if (key_is(s, n, "private")) {
        return BC250HSA_AS_PRIVATE;
    }
    if (key_is(s, n, "local") || key_is(s, n, "group")) {
        return BC250HSA_AS_LOCAL;
    }
    if (key_is(s, n, "constant")) {
        return BC250HSA_AS_CONSTANT;
    }
    if (key_is(s, n, "generic")) {
        return BC250HSA_AS_GENERIC;
    }
    return BC250HSA_AS_NONE;
}

static int is_explicit_kind(uint16_t kind) { return kind <= BC250HSA_ARG_QUEUE; }

static int parse_args(mp_cursor* m, struct bc250hsa_module* mod, uint32_t index) {
    uint64_t count = 0;
    uint64_t i;
    bc250hsa_kernel* kernel = &mod->kernels[index];
    if (!mp_array(m, &count)) {
        return 0;
    }
    if (count > MOCK_MAX_ARGS) {
        return 0;
    }
    for (i = 0; i < count; ++i) {
        uint64_t fields = 0;
        uint64_t f;
        bc250hsa_arg* arg = &mod->args[index][i];
        memset(arg, 0, sizeof(*arg));
        arg->kind = BC250HSA_ARG_HIDDEN_OTHER;
        if (!mp_map(m, &fields)) {
            return 0;
        }
        for (f = 0; f < fields; ++f) {
            const char* key;
            size_t key_bytes;
            if (!mp_str(m, &key, &key_bytes)) {
                return 0;
            }
            if (key_is(key, key_bytes, ".offset")) {
                uint64_t v = 0;
                if (!mp_uint(m, &v)) {
                    return 0;
                }
                arg->offset = (uint32_t)v;
            } else if (key_is(key, key_bytes, ".size")) {
                uint64_t v = 0;
                if (!mp_uint(m, &v)) {
                    return 0;
                }
                arg->size = (uint32_t)v;
            } else if (key_is(key, key_bytes, ".value_align")) {
                uint64_t v = 0;
                if (!mp_uint(m, &v)) {
                    return 0;
                }
                arg->value_align = (uint8_t)(v > 255u ? 255u : v);
            } else if (key_is(key, key_bytes, ".value_kind")) {
                const char* s;
                size_t n;
                if (!mp_str(m, &s, &n)) {
                    return 0;
                }
                arg->kind = arg_kind_of(s, n);
            } else if (key_is(key, key_bytes, ".address_space")) {
                const char* s;
                size_t n;
                if (!mp_str(m, &s, &n)) {
                    return 0;
                }
                arg->address_space = address_space_of(s, n);
            } else if (!mp_skip(m)) {
                return 0;
            }
        }
        if (is_explicit_kind(arg->kind)) {
            kernel->explicit_arg_count++;
        } else {
            kernel->hidden_arg_count++;
        }
    }
    kernel->arg_count = (uint32_t)count;
    kernel->args = mod->args[index];
    return 1;
}

static int parse_kernel(mp_cursor* m, struct bc250hsa_module* mod, uint32_t index) {
    uint64_t fields = 0;
    uint64_t f;
    bc250hsa_kernel* kernel = &mod->kernels[index];
    memset(kernel, 0, sizeof(*kernel));
    mod->names[index][0] = '\0';
    mod->symbols[index][0] = '\0';
    if (!mp_map(m, &fields)) {
        return 0;
    }
    for (f = 0; f < fields; ++f) {
        const char* key;
        size_t key_bytes;
        uint64_t v = 0;
        if (!mp_str(m, &key, &key_bytes)) {
            return 0;
        }
        if (key_is(key, key_bytes, ".name")) {
            const char* s;
            size_t n;
            if (!mp_str(m, &s, &n)) {
                return 0;
            }
            copy_name(mod->names[index], MOCK_NAME_MAX, s, n);
        } else if (key_is(key, key_bytes, ".symbol")) {
            const char* s;
            size_t n;
            if (!mp_str(m, &s, &n)) {
                return 0;
            }
            copy_name(mod->symbols[index], MOCK_NAME_MAX, s, n);
        } else if (key_is(key, key_bytes, ".args")) {
            if (!parse_args(m, mod, index)) {
                return 0;
            }
        } else if (key_is(key, key_bytes, ".kernarg_segment_size")) {
            if (!mp_uint(m, &v)) { return 0; }
            kernel->kernarg_bytes = (uint32_t)v;
        } else if (key_is(key, key_bytes, ".kernarg_segment_align")) {
            if (!mp_uint(m, &v)) { return 0; }
            /* DECIDED in the design: max(.kernarg_segment_align, 16). */
            kernel->kernarg_align = (uint32_t)(v < 16u ? 16u : v);
        } else if (key_is(key, key_bytes, ".group_segment_fixed_size")) {
            if (!mp_uint(m, &v)) { return 0; }
            kernel->group_segment_bytes = (uint32_t)v;
        } else if (key_is(key, key_bytes, ".private_segment_fixed_size")) {
            if (!mp_uint(m, &v)) { return 0; }
            kernel->private_segment_bytes = (uint32_t)v;
        } else if (key_is(key, key_bytes, ".max_flat_workgroup_size")) {
            if (!mp_uint(m, &v)) { return 0; }
            kernel->max_flat_workgroup_size = (uint32_t)v;
        } else if (key_is(key, key_bytes, ".sgpr_count")) {
            if (!mp_uint(m, &v)) { return 0; }
            kernel->sgpr_count = (uint16_t)v;
        } else if (key_is(key, key_bytes, ".vgpr_count")) {
            if (!mp_uint(m, &v)) { return 0; }
            kernel->vgpr_count = (uint16_t)v;
        } else if (key_is(key, key_bytes, ".wavefront_size")) {
            if (!mp_uint(m, &v)) { return 0; }
            kernel->wave_size = (uint8_t)v;
        } else if (key_is(key, key_bytes, ".workgroup_processor_mode")) {
            if (!mp_uint(m, &v)) { return 0; }
            kernel->workgroup_processor_mode = (uint8_t)v;
        } else if (key_is(key, key_bytes, ".uses_dynamic_stack")) {
            if (!mp_uint(m, &v)) { return 0; }
            kernel->uses_dynamic_stack = (uint8_t)v;
        } else if (!mp_skip(m)) {
            return 0;
        }
    }
    if (mod->names[index][0] == '\0' || mod->symbols[index][0] == '\0') {
        return 0;
    }
    if (kernel->kernarg_align == 0u) {
        kernel->kernarg_align = 16u;
    }
    if (kernel->wave_size == 0u) {
        kernel->wave_size = 32u;
    }
    kernel->name = mod->names[index];
    return 1;
}

static bc250hsa_status parse_metadata(const unsigned char* note, size_t note_bytes,
                                      struct bc250hsa_module* mod) {
    mp_cursor m;
    uint64_t fields = 0;
    uint64_t f;
    m.p = note;
    m.n = note_bytes;
    m.at = 0;
    if (!mp_map(&m, &fields)) {
        return BC250HSA_EBADMETADATA;
    }
    for (f = 0; f < fields; ++f) {
        const char* key;
        size_t key_bytes;
        if (!mp_str(&m, &key, &key_bytes)) {
            return BC250HSA_EBADMETADATA;
        }
        if (key_is(key, key_bytes, "amdhsa.kernels")) {
            uint64_t count = 0;
            uint64_t i;
            if (!mp_array(&m, &count)) {
                return BC250HSA_EBADMETADATA;
            }
            if (count > MOCK_MAX_KERNELS) {
                return BC250HSA_EBADMETADATA;
            }
            for (i = 0; i < count; ++i) {
                if (!parse_kernel(&m, mod, (uint32_t)i)) {
                    return BC250HSA_EBADMETADATA;
                }
            }
            mod->kernel_count = (uint32_t)count;
        } else if (!mp_skip(&m)) {
            return BC250HSA_EBADMETADATA;
        }
    }
    return mod->kernel_count != 0u ? BC250HSA_OK : BC250HSA_EBADMETADATA;
}

/* ------------------------------------------------------------------------------------------
 * The code object loader
 * ---------------------------------------------------------------------------------------- */

#define ELF_MACHINE_AMDGPU 0xE0u
#define ELF_OSABI_AMDGPU_HSA 64u
#define ELF_MACH_GFX1013 0x42u
#define ELF_MACH_GFX10_1_GENERIC 0x52u
#define PT_LOAD 1u
#define PT_NOTE 4u
#define SHT_RELA 4u
#define SHT_DYNSYM 11u
#define NT_AMDGPU_METADATA 32u
#define R_AMDGPU_RELATIVE64 13u
#define KD_ENTRY_OFFSET 16u
#define KD_RSRC3_OFFSET 44u
#define KD_RSRC1_OFFSET 48u
#define KD_RSRC2_OFFSET 52u
#define KD_PROPERTIES_OFFSET 56u

static const mock_symbol* symbol_by_name(const struct bc250hsa_module* mod, const char* name) {
    uint32_t i;
    for (i = 0; i < mod->symbol_count; ++i) {
        if (strcmp(mod->symbol_table[i].name, name) == 0) {
            return &mod->symbol_table[i];
        }
    }
    return NULL;
}

static bc250hsa_status load_image(const bc250hsa_allocator* allocator, const void* image,
                                  size_t image_bytes, struct bc250hsa_module** out) {
    const unsigned char* p = (const unsigned char*)image;
    struct bc250hsa_module* mod;
    uint64_t phoff;
    uint64_t shoff;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint32_t flags;
    uint64_t min_va = UINT64_MAX;
    uint64_t max_va = 0;
    const unsigned char* note = NULL;
    size_t note_bytes = 0;
    uint16_t i;
    bc250hsa_status status;

    if (allocator == NULL || allocator->alloc == NULL || image == NULL || out == NULL) {
        return BC250HSA_EINVAL;
    }
    if (image_bytes < 64u || memcmp(p, "\177ELF", 4u) != 0) {
        return BC250HSA_EBADELF;
    }
    if (p[4] != 2u || p[5] != 1u || p[7] != ELF_OSABI_AMDGPU_HSA) {
        return BC250HSA_EBADELF;
    }
    if (p[8] != 3u && p[8] != 4u) {
        return BC250HSA_EBADABIVERSION; /* 3 is code object v5, 4 is v6 */
    }
    if (rd16(p + 18) != ELF_MACHINE_AMDGPU) {
        return BC250HSA_EBADELF;
    }
    flags = rd32(p + 48);
    if ((flags & 0xFFu) != ELF_MACH_GFX1013 && (flags & 0xFFu) != ELF_MACH_GFX10_1_GENERIC) {
        return BC250HSA_EWRONGTARGET;
    }
    phoff = rd64(p + 32);
    shoff = rd64(p + 40);
    phentsize = rd16(p + 54);
    phnum = rd16(p + 56);
    shentsize = rd16(p + 58);
    shnum = rd16(p + 60);
    if (phoff + (uint64_t)phentsize * phnum > image_bytes ||
        shoff + (uint64_t)shentsize * shnum > image_bytes) {
        return BC250HSA_EBADELF;
    }

    for (i = 0; i < phnum; ++i) {
        const unsigned char* ph = p + phoff + (uint64_t)i * phentsize;
        const uint32_t type = rd32(ph);
        const uint64_t offset = rd64(ph + 8);
        const uint64_t vaddr = rd64(ph + 16);
        const uint64_t filesz = rd64(ph + 32);
        const uint64_t memsz = rd64(ph + 40);
        if (type == PT_LOAD) {
            if (offset + filesz > image_bytes) {
                return BC250HSA_EBADELF;
            }
            if (vaddr < min_va) {
                min_va = vaddr;
            }
            if (vaddr + memsz > max_va) {
                max_va = vaddr + memsz;
            }
        } else if (type == PT_NOTE) {
            uint64_t at = offset;
            const uint64_t end = offset + filesz;
            if (end > image_bytes) {
                return BC250HSA_EBADELF;
            }
            while (at + 12u <= end) {
                const uint32_t namesz = rd32(p + at);
                const uint32_t descsz = rd32(p + at + 4u);
                const uint32_t ntype = rd32(p + at + 8u);
                const uint64_t name_at = at + 12u;
                const uint64_t desc_at = name_at + align_up(namesz, 4u);
                if (desc_at + descsz > end) {
                    break;
                }
                if (ntype == NT_AMDGPU_METADATA) {
                    note = p + desc_at;
                    note_bytes = descsz;
                }
                at = desc_at + align_up(descsz, 4u);
            }
        }
    }
    if (min_va == UINT64_MAX || max_va <= min_va) {
        return BC250HSA_EBADELF;
    }
    if (note == NULL) {
        return BC250HSA_EBADMETADATA;
    }

    mod = (struct bc250hsa_module*)calloc(1u, sizeof(*mod));
    if (mod == NULL) {
        return BC250HSA_ENOMEM;
    }
    mod->allocator = *allocator;
    mod->span = max_va - min_va;
    status = allocator->alloc(allocator->ctx, mod->span, 4096u,
                              BC250HSA_MEM_EXEC | BC250HSA_MEM_ZERO, &mod->code);
    if (status != BC250HSA_OK) {
        free(mod);
        return status;
    }
    if (mod->code.host == NULL) {
        /* The loader writes the code, so an allocation with no host mapping is useless. An
         * allocator of BC250HSA_MEM_EXEC memory must report one. */
        allocator->free(allocator->ctx, &mod->code);
        free(mod);
        return BC250HSA_EUNSUPPORTED;
    }
    mod->base_va = mod->code.va;

    for (i = 0; i < phnum; ++i) {
        const unsigned char* ph = p + phoff + (uint64_t)i * phentsize;
        if (rd32(ph) != PT_LOAD) {
            continue;
        }
        {
            const uint64_t offset = rd64(ph + 8);
            const uint64_t vaddr = rd64(ph + 16);
            const uint64_t filesz = rd64(ph + 32);
            memcpy((unsigned char*)mod->code.host + (vaddr - min_va), p + offset, (size_t)filesz);
        }
    }

    /* Relocations and symbols, from the section headers. */
    for (i = 0; i < shnum; ++i) {
        const unsigned char* sh = p + shoff + (uint64_t)i * shentsize;
        const uint32_t type = rd32(sh + 4);
        const uint64_t sh_offset = rd64(sh + 24);
        const uint64_t sh_size = rd64(sh + 32);
        const uint32_t sh_link = rd32(sh + 40);
        const uint64_t sh_entsize = rd64(sh + 56);
        if (sh_offset + sh_size > image_bytes) {
            continue;
        }
        if (type == SHT_RELA && sh_entsize >= 24u) {
            uint64_t at;
            for (at = 0; at + 24u <= sh_size; at += sh_entsize) {
                const unsigned char* rela = p + sh_offset + at;
                const uint64_t r_offset = rd64(rela);
                const uint64_t r_info = rd64(rela + 8);
                const uint64_t r_addend = rd64(rela + 16);
                const uint32_t r_type = (uint32_t)(r_info & 0xFFFFFFFFu);
                if (r_type != R_AMDGPU_RELATIVE64) {
                    /* Every other type names a symbol, which this build does not resolve. */
                    bc250hsa_module_unload(mod);
                    return BC250HSA_EBADRELOC;
                }
                if (r_offset < min_va || r_offset + 8u > max_va) {
                    bc250hsa_module_unload(mod);
                    return BC250HSA_EBADRELOC;
                }
                {
                    const uint64_t value = mod->base_va + r_addend;
                    unsigned char* target = (unsigned char*)mod->code.host + (r_offset - min_va);
                    memcpy(target, &value, sizeof(value));
                }
            }
        } else if (type == SHT_DYNSYM && sh_entsize >= 24u) {
            const unsigned char* strtab = NULL;
            uint64_t strtab_bytes = 0;
            uint64_t at;
            if (sh_link < shnum) {
                const unsigned char* sh_str = p + shoff + (uint64_t)sh_link * shentsize;
                strtab = p + rd64(sh_str + 24);
                strtab_bytes = rd64(sh_str + 32);
            }
            if (strtab == NULL) {
                continue;
            }
            for (at = 0; at + 24u <= sh_size; at += sh_entsize) {
                const unsigned char* sym = p + sh_offset + at;
                const uint32_t st_name = rd32(sym);
                const uint64_t st_value = rd64(sym + 8);
                const uint64_t st_size = rd64(sym + 16);
                if (st_name == 0u || st_name >= strtab_bytes) {
                    continue;
                }
                if (mod->symbol_count >= MOCK_MAX_SYMBOLS) {
                    break;
                }
                {
                    const char* name = (const char*)strtab + st_name;
                    mock_symbol* entry = &mod->symbol_table[mod->symbol_count++];
                    copy_name(entry->name, MOCK_NAME_MAX, name, strlen(name));
                    entry->va = mod->base_va + (st_value - min_va);
                    entry->bytes = st_size;
                }
            }
        }
    }

    status = parse_metadata(note, note_bytes, mod);
    if (status != BC250HSA_OK) {
        bc250hsa_module_unload(mod);
        return status;
    }

    for (i = 0; i < (uint16_t)mod->kernel_count; ++i) {
        bc250hsa_kernel* kernel = &mod->kernels[i];
        const mock_symbol* symbol = symbol_by_name(mod, mod->symbols[i]);
        const unsigned char* kd;
        if (symbol == NULL) {
            bc250hsa_module_unload(mod);
            return BC250HSA_ENOTFOUND;
        }
        kernel->descriptor_va = symbol->va;
        kd = (const unsigned char*)mod->code.host + (symbol->va - mod->base_va);
        kernel->entry_va = symbol->va + rd64(kd + KD_ENTRY_OFFSET);
        kernel->compute_pgm_rsrc3 = rd32(kd + KD_RSRC3_OFFSET);
        kernel->compute_pgm_rsrc1 = rd32(kd + KD_RSRC1_OFFSET);
        kernel->compute_pgm_rsrc2 = rd32(kd + KD_RSRC2_OFFSET);
        kernel->kernel_code_properties = rd16(kd + KD_PROPERTIES_OFFSET);
        kernel->user_sgpr_count = (uint8_t)((kernel->compute_pgm_rsrc2 >> 1) & 0x1Fu);
    }

    g_counters.modules_loaded++;
    {
        bc250hsa_mock_record* record = record_new(BC250HSA_MOCK_MODULE_LOAD);
        if (record != NULL) {
            record->va = mod->base_va;
            record->bytes = mod->span;
            record->value = mod->kernel_count;
            record_line(record);
        }
    }
    *out = mod;
    return BC250HSA_OK;
}

bc250hsa_status bc250hsa_module_load(bc250hsa_device* dev, const void* image, size_t image_bytes,
                                     bc250hsa_module** out) {
    bc250hsa_allocator allocator;
    const bc250hsa_status status = bc250hsa_device_allocator(dev, &allocator);
    if (status != BC250HSA_OK) {
        return status;
    }
    return load_image(&allocator, image, image_bytes, out);
}

bc250hsa_status bc250hsa_module_load_alloc(const bc250hsa_allocator* alloc, const void* image,
                                           size_t image_bytes, bc250hsa_module** out) {
    return load_image(alloc, image, image_bytes, out);
}

void bc250hsa_module_unload(bc250hsa_module* mod) {
    if (mod == NULL) {
        return;
    }
    {
        bc250hsa_mock_record* record = record_new(BC250HSA_MOCK_MODULE_UNLOAD);
        if (record != NULL) {
            record->va = mod->base_va;
            record->bytes = mod->span;
            record_line(record);
        }
    }
    if (mod->allocator.free != NULL && mod->code.va != 0u) {
        mod->allocator.free(mod->allocator.ctx, &mod->code);
    }
    free(mod);
}

uint32_t bc250hsa_module_kernel_count(const bc250hsa_module* mod) {
    return mod != NULL ? mod->kernel_count : 0u;
}

const bc250hsa_kernel* bc250hsa_module_kernel_at(const bc250hsa_module* mod, uint32_t index) {
    if (mod == NULL || index >= mod->kernel_count) {
        return NULL;
    }
    return &mod->kernels[index];
}

const bc250hsa_kernel* bc250hsa_module_kernel_by_name(const bc250hsa_module* mod,
                                                      const char* name) {
    uint32_t i;
    if (mod == NULL || name == NULL) {
        return NULL;
    }
    for (i = 0; i < mod->kernel_count; ++i) {
        if (strcmp(mod->names[i], name) == 0) {
            return &mod->kernels[i];
        }
    }
    return NULL;
}

bc250hsa_status bc250hsa_module_symbol(const bc250hsa_module* mod, const char* name, uint64_t* va,
                                       uint64_t* bytes) {
    const mock_symbol* symbol;
    if (mod == NULL || name == NULL) {
        return BC250HSA_EINVAL;
    }
    symbol = symbol_by_name(mod, name);
    if (symbol == NULL) {
        return BC250HSA_ENOTFOUND;
    }
    if (va != NULL) {
        *va = symbol->va;
    }
    if (bytes != NULL) {
        *bytes = symbol->bytes;
    }
    return BC250HSA_OK;
}

bc250hsa_status bc250hsa_module_range(const bc250hsa_module* mod, uint64_t* va, uint64_t* bytes) {
    if (mod == NULL) {
        return BC250HSA_EINVAL;
    }
    if (va != NULL) {
        *va = mod->base_va;
    }
    if (bytes != NULL) {
        *bytes = mod->span;
    }
    return BC250HSA_OK;
}

/* ------------------------------------------------------------------------------------------
 * Kernel arguments
 * ---------------------------------------------------------------------------------------- */

bc250hsa_status bc250hsa_kernarg_requirements(const bc250hsa_kernel* kernel, uint32_t* bytes,
                                              uint32_t* alignment) {
    if (kernel == NULL || bytes == NULL || alignment == NULL) {
        return BC250HSA_EINVAL;
    }
    *bytes = kernel->kernarg_bytes;
    *alignment = kernel->kernarg_align != 0u ? kernel->kernarg_align : 16u;
    return BC250HSA_OK;
}

static void write_u16(unsigned char* at, uint32_t value) {
    const uint16_t v = (uint16_t)value;
    memcpy(at, &v, sizeof(v));
}

static void write_u32(unsigned char* at, uint32_t value) { memcpy(at, &value, sizeof(value)); }

bc250hsa_status bc250hsa_kernarg_pack(const bc250hsa_kernel* kernel,
                                      const bc250hsa_launch* launch, void* const* args,
                                      uint32_t arg_count, void* kernarg, uint32_t kernarg_bytes,
                                      bc250hsa_pack_result* result) {
    uint32_t i;
    uint32_t explicit_index = 0;
    unsigned char* base = (unsigned char*)kernarg;
    if (kernel == NULL || launch == NULL || kernarg == NULL || result == NULL) {
        return BC250HSA_EINVAL;
    }
    if (launch->struct_bytes != (uint32_t)sizeof(*launch) ||
        result->struct_bytes != (uint32_t)sizeof(*result)) {
        return BC250HSA_EINVAL;
    }
    if (kernarg_bytes < kernel->kernarg_bytes) {
        return BC250HSA_EINVAL;
    }
    memset(base, 0, kernel->kernarg_bytes);
    result->bytes_written = kernel->kernarg_bytes;
    result->explicit_args_written = 0;
    result->hidden_args_zeroed = 0;
    result->unknown_arg_kinds = 0;
    result->hostcall_buffer_requested = 0;
    result->first_unknown_kind[0] = '\0';

    for (i = 0; i < kernel->arg_count; ++i) {
        const bc250hsa_arg* arg = &kernel->args[i];
        unsigned char* at = base + arg->offset;
        if (arg->offset + arg->size > kernel->kernarg_bytes) {
            return BC250HSA_EBADMETADATA;
        }
        if (is_explicit_kind(arg->kind)) {
            if (explicit_index >= arg_count || args == NULL || args[explicit_index] == NULL) {
                return BC250HSA_EINVAL;
            }
            memcpy(at, args[explicit_index], arg->size);
            explicit_index++;
            result->explicit_args_written++;
            continue;
        }
        switch (arg->kind) {
        case BC250HSA_ARG_HIDDEN_BLOCK_COUNT_X: write_u32(at, launch->grid[0]); break;
        case BC250HSA_ARG_HIDDEN_BLOCK_COUNT_Y: write_u32(at, launch->grid[1]); break;
        case BC250HSA_ARG_HIDDEN_BLOCK_COUNT_Z: write_u32(at, launch->grid[2]); break;
        case BC250HSA_ARG_HIDDEN_GROUP_SIZE_X: write_u16(at, launch->block[0]); break;
        case BC250HSA_ARG_HIDDEN_GROUP_SIZE_Y: write_u16(at, launch->block[1]); break;
        case BC250HSA_ARG_HIDDEN_GROUP_SIZE_Z: write_u16(at, launch->block[2]); break;
        case BC250HSA_ARG_HIDDEN_REMAINDER_X:
        case BC250HSA_ARG_HIDDEN_REMAINDER_Y:
        case BC250HSA_ARG_HIDDEN_REMAINDER_Z:
            /* The grid is a whole number of workgroups here, so every remainder is 0. */
            write_u16(at, 0u);
            break;
        case BC250HSA_ARG_HIDDEN_GRID_DIMS:
            write_u16(at, launch->grid[2] > 1u ? 3u : (launch->grid[1] > 1u ? 2u : 1u));
            break;
        case BC250HSA_ARG_HIDDEN_DYNAMIC_LDS_SIZE:
            write_u32(at, launch->dynamic_group_bytes);
            break;
        case BC250HSA_ARG_HIDDEN_HOSTCALL_BUFFER:
            result->hostcall_buffer_requested = 1u;
            g_counters.hostcall_buffer_requests++;
            result->hidden_args_zeroed++;
            g_counters.hidden_args_zeroed++;
            break;
        case BC250HSA_ARG_HIDDEN_OTHER:
            result->unknown_arg_kinds++;
            g_counters.unknown_arg_kinds++;
            result->hidden_args_zeroed++;
            break;
        default:
            result->hidden_args_zeroed++;
            g_counters.hidden_args_zeroed++;
            break;
        }
    }
    if (explicit_index != arg_count) {
        return BC250HSA_EINVAL;
    }
    return BC250HSA_OK;
}

/* ------------------------------------------------------------------------------------------
 * Dispatch, fence and wait
 * ---------------------------------------------------------------------------------------- */

uint32_t bc250hsa_lds_size_field(uint32_t group_segment_bytes, uint32_t dynamic_group_bytes) {
    const uint64_t total = (uint64_t)group_segment_bytes + (uint64_t)dynamic_group_bytes;
    return (uint32_t)(align_up(total, 512u) / 512u);
}

bc250hsa_status bc250hsa_buffer_resource(uint64_t va, uint64_t bytes, uint32_t out_dwords[4]) {
    (void)va;
    (void)bytes;
    (void)out_dwords;
    /* The buffer resource belongs to the real dispatch path, which the mock does not have. */
    return BC250HSA_EUNSUPPORTED;
}

bc250hsa_status bc250hsa_plan_user_sgprs(const bc250hsa_kernel* kernel, uint64_t kernarg_va,
                                         const uint32_t private_segment_rsrc[4],
                                         bc250hsa_user_sgpr_plan* out) {
    (void)kernel;
    (void)kernarg_va;
    (void)private_segment_rsrc;
    (void)out;
    return BC250HSA_EUNSUPPORTED;
}

bc250hsa_status bc250hsa_pm4_build_dispatch(const bc250hsa_dispatch* dispatch,
                                            const bc250hsa_pm4_env* env, uint32_t* dwords,
                                            uint32_t dword_capacity, uint32_t* dwords_written) {
    (void)dispatch;
    (void)env;
    (void)dwords;
    (void)dword_capacity;
    (void)dwords_written;
    /* The golden PM4 test belongs to layer 1. The mock writes no packet. */
    return BC250HSA_EUNSUPPORTED;
}

bc250hsa_status bc250hsa_dispatch_submit(bc250hsa_device* dev, const bc250hsa_dispatch* dispatch,
                                         uint64_t* fence_value_out) {
    const bc250hsa_kernel* kernel;
    uint64_t block_product;
    if (dev == NULL || dev->magic != MOCK_DEVICE_MAGIC || dispatch == NULL ||
        fence_value_out == NULL) {
        return BC250HSA_EINVAL;
    }
    if (dispatch->struct_bytes != (uint32_t)sizeof(*dispatch) ||
        dispatch->launch.struct_bytes != (uint32_t)sizeof(dispatch->launch)) {
        return BC250HSA_EINVAL;
    }
    kernel = dispatch->kernel;
    if (kernel == NULL) {
        return BC250HSA_EINVAL;
    }
    if (dispatch->launch.grid[0] == 0u || dispatch->launch.grid[1] == 0u ||
        dispatch->launch.grid[2] == 0u || dispatch->launch.block[0] == 0u ||
        dispatch->launch.block[1] == 0u || dispatch->launch.block[2] == 0u) {
        return BC250HSA_EINVAL;
    }
    block_product = (uint64_t)dispatch->launch.block[0] * dispatch->launch.block[1] *
                    dispatch->launch.block[2];
    if (kernel->max_flat_workgroup_size != 0u &&
        block_product > kernel->max_flat_workgroup_size) {
        return BC250HSA_EINVAL;
    }
    if ((uint64_t)kernel->group_segment_bytes + dispatch->launch.dynamic_group_bytes > 65536u) {
        return BC250HSA_EINVAL;
    }
    if (kernel->uses_dynamic_stack != 0u) {
        /* Open question 4 of the design: a spilling kernel needs a scratch ring that no trial
         * has measured yet. */
        g_counters.dynamic_stack_refusals++;
        g_counters.submissions_refused++;
        return BC250HSA_EUNSUPPORTED;
    }
    if (kernel->kernarg_bytes != 0u &&
        (dispatch->kernarg_va == 0u ||
         (kernel->kernarg_align != 0u && (dispatch->kernarg_va % kernel->kernarg_align) != 0u))) {
        return BC250HSA_EINVAL;
    }

    mock_lock();
    g_counters.dispatches_built++;
    g_counters.submissions++;
    dev->fence++;
    *fence_value_out = dev->fence;
    mock_retire_due(dev);
    if (dev->hold_ms == 0u || dev->pending_count >= MOCK_MAX_PENDING_FENCES) {
        dev->retired = dev->fence;
    } else {
        dev->pending[dev->pending_count].value = dev->fence;
        dev->pending[dev->pending_count].due_ms = mock_now_ms() + dev->hold_ms;
        dev->pending_count++;
    }

    {
        bc250hsa_mock_record* record = record_new(BC250HSA_MOCK_DISPATCH);
        if (record != NULL) {
            uint32_t i;
            copy_name(record->kernel, sizeof(record->kernel),
                      kernel->name != NULL ? kernel->name : "?",
                      kernel->name != NULL ? strlen(kernel->name) : 1u);
            for (i = 0; i < 3u; ++i) {
                record->grid[i] = dispatch->launch.grid[i];
                record->block[i] = dispatch->launch.block[i];
            }
            record->dynamic_group_bytes = dispatch->launch.dynamic_group_bytes;
            record->va = dispatch->kernarg_va;
            record->value = dev->fence;
            record->kernarg_bytes = kernel->kernarg_bytes < BC250HSA_MOCK_KERNARG_MAX
                                        ? kernel->kernarg_bytes
                                        : BC250HSA_MOCK_KERNARG_MAX;
            if (record->kernarg_bytes != 0u) {
                bc250hsa_mem probe;
                mock_allocation* allocation;
                memset(&probe, 0, sizeof(probe));
                probe.va = dispatch->kernarg_va;
                allocation = allocation_of(dev, &probe);
                if (allocation == NULL) {
                    /* An interior address of a pooled buffer: find the allocation that holds it. */
                    uint32_t slot;
                    for (slot = 0; slot < MOCK_MAX_ALLOCATIONS; ++slot) {
                        mock_allocation* candidate = &dev->allocations[slot];
                        if (candidate->live && dispatch->kernarg_va >= candidate->mem.va &&
                            dispatch->kernarg_va < candidate->mem.va + candidate->mem.bytes) {
                            allocation = candidate;
                            break;
                        }
                    }
                }
                if (allocation != NULL) {
                    const uint64_t offset = dispatch->kernarg_va - allocation->mem.va;
                    memcpy(record->kernarg, (const unsigned char*)allocation->raw + offset,
                           record->kernarg_bytes);
                } else {
                    record->kernarg_bytes = 0u;
                }
            }
            record_line(record);
        }
    }
    mock_unlock();
    return BC250HSA_OK;
}

uint64_t bc250hsa_fence_read(bc250hsa_device* dev) {
    uint64_t retired;
    if (dev == NULL || dev->magic != MOCK_DEVICE_MAGIC) {
        return UINT64_MAX;
    }
    mock_lock();
    mock_retire_due(dev);
    retired = dev->retired;
    mock_unlock();
    /* Without a hold time a recorded dispatch retires at once, so this is the last value. */
    return retired;
}

uint64_t bc250hsa_fence_last_submitted(bc250hsa_device* dev) {
    uint64_t submitted;
    if (dev == NULL || dev->magic != MOCK_DEVICE_MAGIC) {
        return 0u;
    }
    mock_lock();
    submitted = dev->fence;
    mock_unlock();
    return submitted;
}

bc250hsa_status bc250hsa_wait(bc250hsa_device* dev, uint64_t value, uint32_t slice_ms,
                              uint32_t total_ms) {
    uint64_t deadline_ms;
    uint32_t hold_ms;
    uint64_t submitted;
    uint64_t retired;
    if (dev == NULL || dev->magic != MOCK_DEVICE_MAGIC) {
        return BC250HSA_EINVAL;
    }
    mock_lock();
    g_counters.waits++;
    mock_retire_due(dev);
    retired = dev->retired;
    submitted = dev->fence;
    hold_ms = dev->hold_ms;
    if (value <= retired) {
        g_counters.waits_fast++;
    }
    {
        /* The bound is recorded as well as obeyed. A test reads it to see that layer 2 passes
         * its own wait policy into every wait. */
        bc250hsa_mock_record* record = record_new(BC250HSA_MOCK_WAIT);
        if (record != NULL) {
            record->value = value;
            record->wait_slice_ms = slice_ms;
            record->wait_total_ms = total_ms;
            record_line(record);
        }
    }
    mock_unlock();

    if (value <= retired) {
        return BC250HSA_OK;
    }
    if (hold_ms == 0u || value > submitted) {
        /* Nothing can raise the fence here, so a value above it is a test mistake. */
        mock_lock();
        g_counters.waits_timed_out++;
        mock_unlock();
        return BC250HSA_ETIMEOUT;
    }

    /* A hold time is in effect, so the value is in flight and this wait really waits. The lock
     * is not held while it sleeps: a mock that blocked every other thread here would hide
     * exactly what the multithreaded test measures. */
    deadline_ms = mock_now_ms() + (total_ms != 0u ? total_ms : 120000u);
    for (;;) {
        mock_sleep_ms(1u);
        mock_lock();
        mock_retire_due(dev);
        retired = dev->retired;
        mock_unlock();
        if (value <= retired) {
            return BC250HSA_OK;
        }
        if (mock_now_ms() >= deadline_ms) {
            mock_lock();
            g_counters.waits_timed_out++;
            mock_unlock();
            return BC250HSA_ETIMEOUT;
        }
    }
}

bc250hsa_status bc250hsa_query_fault(bc250hsa_device* dev, bc250hsa_fault* out) {
    if (dev == NULL || dev->magic != MOCK_DEVICE_MAGIC || out == NULL) {
        return BC250HSA_EINVAL;
    }
    if (out->struct_bytes != (uint32_t)sizeof(*out)) {
        return BC250HSA_EINVAL;
    }
    out->device_lost = 0u;
    out->faulted_va = 0u;
    out->general_error = 0u;
    out->device_error = 0u;
    out->fault_flags = 0u;
    out->pipeline_stage = 0u;
    out->fence_value = dev->fence;
    out->fence_expected = dev->fence;
    return BC250HSA_OK;
}

bc250hsa_status bc250hsa_last_ib(bc250hsa_device* dev, const uint32_t** dwords, uint32_t* count) {
    (void)dev;
    (void)dwords;
    (void)count;
    /* The mock builds no indirect buffer. */
    return BC250HSA_EUNSUPPORTED;
}

/* ------------------------------------------------------------------------------------------
 * The control interface of the mock
 * ---------------------------------------------------------------------------------------- */

void bc250hsa_mock_reset(void) {
    mock_lock();
    g_record_count = 0u;
    memset(&g_counters, 0, sizeof(g_counters));
    g_counters.struct_bytes = (uint32_t)sizeof(g_counters);
    mock_unlock();
}

void bc250hsa_mock_set_hold_ms(uint32_t hold_ms) {
    mock_lock();
    /* The device may not be open yet. The field is part of the device, and bc250hsa_open keeps
     * it when the device already exists, so an order of calls either way works. */
    if (g_device.magic != MOCK_DEVICE_MAGIC) {
        memset(&g_device, 0, sizeof(g_device));
        g_device.magic = MOCK_DEVICE_MAGIC;
        g_device.next_va = 0x0000400000000000ull;
    }
    g_device.hold_ms = hold_ms;
    mock_unlock();
}

uint32_t bc250hsa_mock_hold_ms(void) {
    uint32_t hold_ms;
    mock_lock();
    hold_ms = g_device.hold_ms;
    mock_unlock();
    return hold_ms;
}

uint32_t bc250hsa_mock_record_count(void) { return g_record_count; }

const bc250hsa_mock_record* bc250hsa_mock_record_at(uint32_t index) {
    if (index >= g_record_count) {
        return NULL;
    }
    return &g_records[index];
}

uint32_t bc250hsa_mock_live_allocations(void) { return g_device.live_allocations; }

uint64_t bc250hsa_mock_live_bytes(void) { return g_device.live_bytes; }

uint32_t bc250hsa_mock_dispatch_count(const char* kernel) {
    uint32_t i;
    uint32_t count = 0;
    for (i = 0; i < g_record_count; ++i) {
        if (g_records[i].kind != (uint32_t)BC250HSA_MOCK_DISPATCH) {
            continue;
        }
        if (kernel == NULL || strcmp(g_records[i].kernel, kernel) == 0) {
            count++;
        }
    }
    return count;
}

int bc250hsa_mock_write_record(const char* path) {
    FILE* file = NULL;
    uint32_t i;
    if (path == NULL) {
        return 0;
    }
#if defined(_MSC_VER)
    if (fopen_s(&file, path, "w") != 0) {
        file = NULL;
    }
#else
    file = fopen(path, "w");
#endif
    if (file == NULL) {
        return 0;
    }
    for (i = 0; i < g_record_count; ++i) {
        const bc250hsa_mock_record* record = &g_records[i];
        fprintf(file, "%s kernel=%s grid=%u,%u,%u block=%u,%u,%u value=%llu\n",
                bc250hsa_mock_kind_name(record->kind),
                record->kernel[0] != '\0' ? record->kernel : "-", record->grid[0], record->grid[1],
                record->grid[2], record->block[0], record->block[1], record->block[2],
                (unsigned long long)record->value);
    }
    fclose(file);
    return 1;
}
