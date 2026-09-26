
`d3dkmddi.h` line 9968, WDK 10.0.26100:

```c
typedef enum _DXGIDDI_PARTITIONING_EVENT_TYPE
{
    DXGK_PARTITION_EVENT_FUNCTION_LEVEL_RESET   = 0,
    DXGK_PARTITION_EVENT_FUNDAMENTAL_WARM_RESET = 1,
    DXGK_PARTITION_EVENT_DRIVER_INTERNAL        = 2,
} DXGIDDI_PARTITIONING_EVENT_TYPE;
```

### Description

**DXGIDDI_PARTITIONING_EVENT_TYPE** identifies the type of ETW event being reported by [**DxgkCbLogEtwEvent**](dispmprt.md#nc-dispmprt-dxgkcb_log_etw_event) when **EventGuid** is GUID_DXGKDDI_AZURE_TRIAGE_EVENT.

### Enum fields

#### Field DXGK_PARTITION_EVENT_FUNCTION_LEVEL_RESET

A driver specifies this type to indicate a function level reset (FLR). A FLR when the virtual function (VF) must be reset and all register mappings are reset to attempt to unblock a detected timeout. This is typically the first attempted step of an adapter-wide reset on a vGPU in a guest.

#### Field DXGK_PARTITION_EVENT_FUNDAMENTAL_WARM_RESET

A driver specifies this type to indicate a fundamental warm reset (FWR). A FWR is the whole-GPU reset that is typically seen on bare metal (non virtualized) adapter resets. A FWR is typically the second and last thing attempted for an adapter reset on a vGPU when the FLR fails to get the adapter executing again. A FWR is disruptive not only to the virtual machine (VM) with the vGPU wanting to be reset, but to all other VMs with assigned vGPUs from that device, so this level of TDR recovery should be avoided wherever possible.

#### Field DXGK_PARTITION_EVENT_DRIVER_INTERNAL

A driver specifies this type to report other event(s) with information that might be helpful for issue triage.

### Remarks

**DXGK_PARTITION_EVENT_FUNCTION_LEVEL_RESET** and **DXGK_PARTITION_EVENT_FUNDAMENTAL_WARM_RESET** provide two separate levels of severity when a driver reports timeout detection and recovery (TDR) events.

See [**DXGKDDICB_PARTITIONING_EVENT_NOTIFICATION**](#ns-d3dkmddi-dxgkddicb_partitioning_event_notification) for additional details.

### See also

[**DxgkCbLogEtwEvent**](dispmprt.md#nc-dispmprt-dxgkcb_log_etw_event)

_Header: d3dkmddi.h_


<a id="ne-d3dkmddi-dxgk_access_mode"></a>

## DXGK_ACCESS_MODE

### Declaration

`d3dkmddi.h` line 10000, WDK 10.0.26100:

```c
typedef enum _DXGK_ACCESS_MODE
{
    DXGK_ACCESS_MODE_KERNEL_MODE,
    DXGK_ACCESS_MODE_USER_MODE
} DXGK_ACCESS_MODE;
```

### Description

**DXGK_ACCESS_MODE** describes the access mode that [**DxgkCbMapPhysicalMemory**](#nc-d3dkmddi-dxgkcb_mapphysicalmemory) will provide when doing a mapping.

### Enum fields

#### Field DXGK_ACCESS_MODE_KERNEL_MODE

The resulting mapping will be a kernel-mode virtual address. If the **hPhysicalMemoryObject** returned by [**DxgkCbCreatePhysicalMemoryObject**](#nc-d3dkmddi-dxgkcb_createphysicalmemoryobject) is of type **DXGK_PHYSICAL_MEMORY_TYPE_IO_SPACE**, then **AccessMode** must be KernelMode.

#### Field DXGK_ACCESS_MODE_USER_MODE

The resulting mapping will be made in the context of the current process. The caller is expected to be in the context of the correct process during both map and unmap.

### Remarks

**DXGK_ACCESS_MODE** is provided in the [**DXGKARGCB_MAP_PHYSICAL_MEMORY**](#ns-d3dkmddi-dxgkargcb_map_physical_memory) structure that is passed to [**DxgkCbMapPhysicalMemory**](#nc-d3dkmddi-dxgkcb_mapphysicalmemory).

### See also

[**DXGKARGCB_MAP_PHYSICAL_MEMORY**](#ns-d3dkmddi-dxgkargcb_map_physical_memory)

[**DxgkCbCreatePhysicalMemoryObject**](#nc-d3dkmddi-dxgkcb_createphysicalmemoryobject)

[**DxgkCbMapPhysicalMemory**](#nc-d3dkmddi-dxgkcb_mapphysicalmemory)

[**DxgkCbUnmapPhysicalMemory**](#nc-d3dkmddi-dxgkcb_unmapphysicalmemory)

_Header: d3dkmddi.h_


<a id="ne-d3dkmddi-dxgk_engine_state"></a>
