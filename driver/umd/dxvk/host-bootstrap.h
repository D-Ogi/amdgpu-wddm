/*
 * Copyright (c) 2026 D-Ogi
 * SPDX-License-Identifier: MIT
 *
 * Private BC250 bootstrap contract, not a registered Vulkan extension.
 * Both endpoints are built together against the same WDK and x64 ABI.
 * Version 2 carries device-scoped runtime operations. Version 3 adds the
 * private import of runtime-owned allocations. Version 4 adds queue progress
 * fences that order hosted rendering against native Present. Version 5 adds
 * the sticky device status and its loss report.
 */
#ifndef BC250_HOST_BOOTSTRAP_H
#define BC250_HOST_BOOTSTRAP_H
#include <stdint.h>
#define BC250_HOST_STYPE 0x42434831u
#define BC250_HOST_VERSION 5u
#define BC250_HOST_IMPORT_STYPE 0x42434832u
struct bc250_host_import {
   uint32_t sType;
   const void *pNext;
   void *identity;
   uint32_t allocation;
   uint64_t va, size;
};
#define BC250_HOST_CREATE_PAGING 1u
#define BC250_HOST_DESTROY_PAGING 2u
#define BC250_HOST_PUBLISH_PROGRESS 3u
#define BC250_HOST_CHECK_STATUS 4u
#define BC250_HOST_REPORT_LOST 5u
struct bc250_host_progress {
   uint32_t context, sync;
   uint64_t value;
   const uint64_t *cpu_address;
};
struct bc250_host_paging {
   uint32_t queue, sync;
   void *cpu_address;
};
struct bc250_host {
   uint32_t sType;
   const void *pNext;
   uint32_t version, size;
   uint64_t adapter_luid;
   void *identity;
   void *userdata;
   int32_t (*dispatch)(void *userdata, uint32_t operation, void *argument);
};
static inline int32_t bc250_host_check_status(const struct bc250_host *host)
{
   return host->dispatch ? host->dispatch(host->userdata, BC250_HOST_CHECK_STATUS, 0) : 0;
}
static inline int bc250_host_fence_valid(const struct bc250_host *host, uint64_t value)
{
   if (host->dispatch && value == UINT64_MAX) {
      host->dispatch(host->userdata, BC250_HOST_REPORT_LOST, 0);
      return 0;
   }
   return bc250_host_check_status(host) >= 0;
}
#define BC250_HOST_CreateAllocation2 16u
#define BC250_HOST_DestroyAllocation2 17u
#define BC250_HOST_ReserveGpuVirtualAddress 18u
#define BC250_HOST_MapGpuVirtualAddress 19u
#define BC250_HOST_FreeGpuVirtualAddress 20u
#define BC250_HOST_MakeResident 21u
#define BC250_HOST_Evict 22u
#define BC250_HOST_Lock2 23u
#define BC250_HOST_Unlock2 24u
#define BC250_HOST_CreateContextVirtual 25u
#define BC250_HOST_DestroyContext 26u
#define BC250_HOST_CreateSynchronizationObject2 27u
#define BC250_HOST_DestroySynchronizationObject 28u
#define BC250_HOST_WaitForSynchronizationObjectFromCpu 29u
#define BC250_HOST_SignalSynchronizationObjectFromCpu 30u
#define BC250_HOST_WaitForSynchronizationObjectFromGpu 31u
#define BC250_HOST_SignalSynchronizationObjectFromGpu 32u
#define BC250_HOST_SignalSynchronizationObjectFromGpu2 33u
#define BC250_HOST_SubmitCommand 34u
#define BC250_HOST_UpdateGpuVirtualAddress 35u
#define BC250_HOST_GetDeviceState 36u
#define BC250_HOST_QueryResourceInfoFromNtHandle 37u
#define BC250_HOST_OpenResourceFromNtHandle 38u
#define BC250_HOST_OpenSyncObjectFromNtHandle2 39u
#define BC250_HOST_CreateHwQueue 40u
#define BC250_HOST_DestroyHwQueue 41u
#define BC250_HOST_SubmitCommandToHwQueue 42u
#define BC250_HOST_SubmitWaitForSyncObjectsToHwQueue 43u
#define BC250_HOST_SubmitSignalSyncObjectsToHwQueue 44u
#define BC250_WDDM_CALL(host, name, argument) \
   ((host)->dispatch ? (host)->dispatch((host)->userdata, BC250_HOST_##name, (void *)(argument)) : WDDM2_DISPATCH(name(argument)))
#endif
