
<a id="ns-d3dkmddi-dxgkargcb_create_physical_memory_object"></a>

## DXGKARGCB_CREATE_PHYSICAL_MEMORY_OBJECT

### Declaration

`d3dkmddi.h` line 10008, WDK 10.0.26100:

```c
typedef struct _DXGKARGCB_CREATE_PHYSICAL_MEMORY_OBJECT
{
    _In_opt_ HANDLE hAdapter;
    _In_ SIZE_T Size;
    _In_opt_ ULONG_PTR Context;
    _In_ DXGK_PHYSICAL_MEMORY_TYPE Type;
    _In_ DXGK_MEMORY_CACHING_TYPE CacheType;
    union
    {
        struct
        {
            _In_ PHYSICAL_ADDRESS LowAddress;
            _In_ PHYSICAL_ADDRESS HighAddress;
            _In_ PHYSICAL_ADDRESS SkipBytes;
            _In_ UINT Flags;
        } Mdl;
        struct
        {
            _In_ PHYSICAL_ADDRESS LowestAcceptableAddress;
            _In_ PHYSICAL_ADDRESS HighestAcceptableAddress;
            _In_ PHYSICAL_ADDRESS BoundaryAddressMultiple;
        } ContiguousMemory;
        struct
        {
            _In_ ACCESS_MASK DesiredAccess;
            _In_ POBJECT_ATTRIBUTES ObjectAttributes;
            _In_ ULONG PageProtection;
            _In_ ULONG AllocationAttributes;
        } Section;
        struct
        {
            _In_ PHYSICAL_ADDRESS BaseAddress;
        } IOSpace;
    };
    _Out_ HANDLE hPhysicalMemoryObject;
    _Out_ HANDLE hAdapterMemoryObject;
} DXGKARGCB_CREATE_PHYSICAL_MEMORY_OBJECT;
```

### Description

The **DXGKARGCB_CREATE_PHYSICAL_MEMORY_OBJECT** structure contains the information used by the [**DXGKCB_CREATEPHYSICALMEMORYOBJECT**](#nc-d3dkmddi-dxgkcb_createphysicalmemoryobject) callback function to create physical memory.

### Struct fields

#### Field hAdapter

The adapter for which this physical memory will be associated. This parameter is optional and can be NULL. If it is NULL, the driver must call [**DXGKCB_OPENPHYSICALMEMORYOBJECT**](#nc-d3dkmddi-dxgkcb_openphysicalmemoryobject) before creating an address descriptor list (ADL). See Remarks for more information.

#### Field Size

The size, in bytes, of the physical memory being requested. If **Type** is **DXGK_PHYSICAL_MEMORY_TYPE_IO_SPACE**, **Size** refers to the size of the IO space region provided by the driver.

#### Field Context

A pointer-size piece of context data that *Dxgkrnl* will store alongside the physical memory object for debugging purposes. This value is never directly used or dereferenced by *Dxgkrnl* in any way. This may be any value of the driver's choosing, such as the memory address to a driver-owned object that owns this physical memory object.

#### Field Type

A [**DXGK_PHYSICAL_MEMORY_TYPE**](#ne-d3dkmddi-dxgk_physical_memory_type) value that specifies the type of physical memory to create. If **Type** is **DXGK_PHYSICAL_MEMORY_TYPE_SECTION**, then the allocation attributes of the section object are always SEC_COMMIT (PF-mapped section), and the cache type is determines by **CacheType**.

#### Field CacheType

The cache type of the pages. If **Type** is **DXGK_PHYSICAL_MEMORY_TYPE_SECTION**, **CacheType** must be either cached, or write-combined.

#### Field Mdl

Used only when **Type** is **DXGK_PHYSICAL_MEMORY_TYPE_MDL**.

#### Field Mdl.LowAddress

Used only when **Type** is **DXGK_PHYSICAL_MEMORY_TYPE_MDL**. See [**MmAllocatePagesForMdlEx**](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-mmallocatepagesformdlex) for details.

#### Field Mdl.HighAddress

Used only when **Type** is **DXGK_PHYSICAL_MEMORY_TYPE_MDL**. See [**MmAllocatePagesForMdlEx**](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-mmallocatepagesformdlex) for details.

#### Field Mdl.SkipBytes

Used only when **Type** is **DXGK_PHYSICAL_MEMORY_TYPE_MDL**. See [**MmAllocatePagesForMdlEx**](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-mmallocatepagesformdlex) for details.

#### Field Mdl.Flags

Used only when **Type** is **DXGK_PHYSICAL_MEMORY_TYPE_MDL**. See [**MmAllocatePagesForMdlEx**](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-mmallocatepagesformdlex) for details.

#### Field ContiguousMemory

Used only when **Type** is **DXGK_PHYSICAL_MEMORY_TYPE_CONTIGUOUS_MEMORY**.

#### Field ContiguousMemory.LowestAcceptableAddress

Used only when **Type** is **DXGK_PHYSICAL_MEMORY_TYPE_CONTIGUOUS_MEMORY**. See [**MmAllocateContiguousMemorySpecifyCache**](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/nf-ntddk-mmallocatecontiguousmemoryspecifycache) for details.

#### Field ContiguousMemory.HighestAcceptableAddress

Used only when **Type** is **DXGK_PHYSICAL_MEMORY_TYPE_CONTIGUOUS_MEMORY**. See [**MmAllocateContiguousMemorySpecifyCache**](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/nf-ntddk-mmallocatecontiguousmemoryspecifycache) for details.

#### Field ContiguousMemory.BoundaryAddressMultiple

Used only when **Type** is **DXGK_PHYSICAL_MEMORY_TYPE_CONTIGUOUS_MEMORY**. See [**MmAllocateContiguousMemorySpecifyCache**](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/nf-ntddk-mmallocatecontiguousmemoryspecifycache) for details.

#### Field Section

Used only when **Type** is **DXGK_PHYSICAL_MEMORY_TYPE_SECTION**.

#### Field Section.DesiredAccess

Used only when **Type** is **DXGK_PHYSICAL_MEMORY_TYPE_SECTION**. See [**ZwCreateSection**](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-zwcreatesection) for details.

#### Field Section.ObjectAttributes

Used only when **Type** is **DXGK_PHYSICAL_MEMORY_TYPE_SECTION**. See [**ZwCreateSection**](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-zwcreatesection) for details.

#### Field Section.PageProtection

Used only when **Type** is **DXGK_PHYSICAL_MEMORY_TYPE_SECTION**. This value should be one of the following values: PAGE_READONLY, PAGE_READWRITE, PAGE_EXECUTE, or PAGE_WRITECOPY. Do not specify cache attributes (e.g. SEC_WRITECOMBINED) in this field. The allocated attributes of the section object are always SEC_COMMIT (PF-mapped section), and the cache type is determines by **CacheType**. See [**ZwCreateSection**](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-zwcreatesection) for details.

#### Field Section.AllocationAttributes

Used only when **Type** is **DXGK_PHYSICAL_MEMORY_TYPE_SECTION**. See [**ZwCreateSection**](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-zwcreatesection) for details.

#### Field IOSpace

Used only when **Type** is **DXGK_PHYSICAL_MEMORY_TYPE_IO_SPACE**.

#### Field IOSpace.BaseAddress

Used only when **Type** is **DXGK_PHYSICAL_MEMORY_TYPE_IO_SPACE**. Specifies the starting physical address of the IO space region. This value must be aligned to a multiple of PAGE_SIZE.

#### Field hPhysicalMemoryObject

On a successful call to [**DXGKCB_CREATEPHYSICALMEMORYOBJECT**](#nc-d3dkmddi-dxgkcb_createphysicalmemoryobject), this is an opaque handle back to a *Dxgkrnl*-managed physical memory object. The physical memory object can be provided to other documented memory management functions to map a virtual address for CPU access. This handle can be closed by calling [**DXGKCB_DESTROYPHYSICALMEMORYOBJECT**](#nc-d3dkmddi-dxgkcb_destroyphysicalmemoryobject).

#### Field hAdapterMemoryObject

On a successful call to [**DXGKCB_CREATEPHYSICALMEMORYOBJECT**](#nc-d3dkmddi-dxgkcb_createphysicalmemoryobject), this is an opaque handle back to a *Dxgkrnl*-managed adapter memory object. The adapter memory object can be provided to other documented memory management functions to generate an ADL for GPU access. If **hAdapter** is NULL, this value will be NULL and the driver must call [**DXGKCB_OPENPHYSICALMEMORYOBJECT**](#nc-d3dkmddi-dxgkcb_openphysicalmemoryobject) to create this handle. This handle can be closed by calling [**DXGKCB_CLOSEPHYSICALMEMORYOBJECT**](#nc-d3dkmddi-dxgkcb_closephysicalmemoryobject), or may be provided to [**DXGKCB_DESTROYPHYSICALMEMORYOBJECT**](#nc-d3dkmddi-dxgkcb_destroyphysicalmemoryobject) (but not both).

### Remarks

The *hAdapter* field is optional when creating a physical memory object, but that object must be opened against an adapter in a call to [**DXGKCB_OPENPHYSICALMEMORYOBJECT**](#nc-d3dkmddi-dxgkcb_openphysicalmemoryobject) before an [ADL can be created](#nc-d3dkmddi-dxgkcb_allocateadl). This is because an ADL represents logical memory, and each logical adapter has a unique domain. It does not matter which physical adapter the memory is created against. It will be opened by the logical adapter that the physical adapter belongs to and will be mapped to all linked physical adapters.

See [IOMMU DMA remapping](/windows-hardware/drivers/display/iommu-dma-remapping) for more information.

### See also

[**DXGK_PHYSICAL_MEMORY_TYPE**](#ne-d3dkmddi-dxgk_physical_memory_type)

[**DXGKCB_ALLOCATEADL**](#nc-d3dkmddi-dxgkcb_allocateadl)

[**DXGKCB_CLOSEPHYSICALMEMORYOBJECT**](#nc-d3dkmddi-dxgkcb_closephysicalmemoryobject)

