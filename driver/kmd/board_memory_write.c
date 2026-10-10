// BC-250 ABL memory setting. A runtime HAL self-check gates every transaction.
#include "bc250kmd.h"
#include "board_memory_detect.h"
#include "board_memory_store.h"

// CMOS belongs to the platform, so different adapter instances share this lock.
// HAL supplies OS access serialization. This lock does not exclude SMM/firmware.
static EX_PUSH_LOCK PlatformLock;

static void Enter(void)
{
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&PlatformLock);
}
static void Leave(void)
{
    ExReleasePushLockExclusive(&PlatformLock);
    KeLeaveCriticalRegion();
}
static void Publish(BC250_DEVICE* device, const struct board_memory_state* state)
{
    KIRQL old;
    KeAcquireSpinLock(&device->BoardMemorySnapshotLock, &old);
    device->BoardMemoryState = *state;
    KeReleaseSpinLock(&device->BoardMemorySnapshotLock, old);
}
void BoardMemoryInitialize(BC250_DEVICE* device)
{
    KeInitializeSpinLock(&device->BoardMemorySnapshotLock);
}
void BoardMemoryStart(BC250_DEVICE* device)
{
    struct bc250_uma_transport transport = {0};
    struct bc250_uma_io io;
    struct board_memory_store store;
    struct board_memory_state state = {0};
    unsigned char block[28];
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    Enter();
    device->BoardMemoryStarted = FALSE;
    device->BoardMemoryMapping = BC250_UMA_MAPPING_NONE;
    if (InterlockedCompareExchange(&device->BoardMemoryProviderId, 0, 0) !=
        BC250_BOARD_MEMORY_PROVIDER_BC250_ABL || !device->BoardMemoryBiosId ||
        !InterlockedCompareExchange64(&device->UmaActiveBytes, 0, 0)) goto done;
    if (!BoardMemoryDetect(&transport, block)) goto done;
    device->BoardMemoryMapping = transport.mapping;
    bc250_uma_transport_io(&transport, &io);
    BoardMemoryStoreIo(device, &store);
    BoardMemoryServiceStart(&state, &io, &store);
    if (state.ready && memcmp(state.block, block, sizeof(block))) {
        state.ready = 0;
        state.result = BC250_UMA_STALE;
    }
    device->BoardMemoryStarted = TRUE;
done:
    Publish(device, &state);
    Leave();
}
void BoardMemoryStop(BC250_DEVICE* device)
{
    struct board_memory_state state = {0};
    // Stop waits for the full transaction, including durable journal completion.
    Enter();
    device->BoardMemoryStarted = FALSE;
    device->BoardMemoryMapping = BC250_UMA_MAPPING_NONE;
    Publish(device, &state);
    Leave();
}
void BoardMemoryWriteQuery(BC250_DEVICE* device, BC250_ESCAPE_BOARD_MEMORY* data)
{
    struct board_memory_state state;
    KIRQL old;
    KeAcquireSpinLock(&device->BoardMemorySnapshotLock, &old);
    state = device->BoardMemoryState;
    KeReleaseSpinLock(&device->BoardMemorySnapshotLock, old);
    // The GUI refresh is a software-only snapshot, with no HAL or registry I/O.
    data->ResultCode = state.result;
    if (state.backup_valid) {
        data->Flags |= BC250_BOARD_MEMORY_BACKUP_VALID;
        data->PreviousMiB = state.backup[26] | ((ULONG)state.backup[27] << 8);
    }
    if (state.blocked) {
        data->Reason = BC250_BOARD_MEMORY_REASON_UNKNOWN_STATE;
        return;
    }
    if (!state.ready) return;
    data->Flags |= BC250_BOARD_MEMORY_READ_VALID;
    data->RequestedMiB = state.block[26] | ((ULONG)state.block[27] << 8);
    RtlCopyMemory(data->ObservedBlock, state.block, sizeof(data->ObservedBlock));
    data->Reason = BC250_BOARD_MEMORY_REASON_READY;
    if (InterlockedCompareExchange64(&device->UmaActiveBytes, 0, 0) &&
        !InterlockedCompareExchange(&device->RetainedPowerPhase, 0, 0))
        data->Flags |= BC250_BOARD_MEMORY_WRITE_ALLOWED;
}
static NTSTATUS Change(BC250_DEVICE* device, const BC250_ESCAPE_BOARD_MEMORY* data, int restore)
{
    struct bc250_uma_transport transport = {0};
    struct bc250_uma_io io;
    struct board_memory_store store;
    struct board_memory_state state;
    unsigned char block[28];
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    int result;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;
    Enter();
    state = device->BoardMemoryState; // All writers hold PlatformLock.
    if (!device->BoardMemoryStarted || !state.ready || state.blocked ||
        InterlockedCompareExchange(&device->BoardMemoryProviderId, 0, 0) !=
            BC250_BOARD_MEMORY_PROVIDER_BC250_ABL ||
        InterlockedCompareExchange(&device->RetainedPowerPhase, 0, 0) ||
        !InterlockedCompareExchange64(&device->UmaActiveBytes, 0, 0)) goto done;
    // A cached capability is not permission to use a stale transport or block.
    if (!BoardMemoryDetect(&transport, block) || transport.mapping != device->BoardMemoryMapping) {
        state.ready = 0;
        state.result = BC250_UMA_READ_FAILED;
        goto done;
    }
    if (memcmp(block, data->ObservedBlock, sizeof(block)) || memcmp(block, state.block, sizeof(block))) {
        state.result = BC250_UMA_STALE;
        status = STATUS_RETRY;
        goto done;
    }
    bc250_uma_transport_io(&transport, &io);
    BoardMemoryStoreIo(device, &store);
    result = BoardMemoryServiceChange(&state, &io, &store, data->ObservedBlock, data->RequestedMiB, restore);
    state.result = result;
    status = result >= 0 ? STATUS_SUCCESS :
        (result == BC250_UMA_STALE ? STATUS_RETRY : STATUS_DEVICE_HARDWARE_ERROR);
done:
    Publish(device, &state);
    Leave();
    return status;
}
NTSTATUS AblBoardMemorySet(BC250_DEVICE* device, const BC250_ESCAPE_BOARD_MEMORY* data)
{
    return Change(device, data, 0);
}
NTSTATUS AblBoardMemoryRestore(BC250_DEVICE* device, const BC250_ESCAPE_BOARD_MEMORY* data)
{
    return Change(device, data, 1);
}
