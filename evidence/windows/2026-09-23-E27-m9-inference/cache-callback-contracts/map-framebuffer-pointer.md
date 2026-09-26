## DXGKCB_MAPFRAMEBUFFERPOINTER callback function

### Declaration

`d3dkmddi.h` line 9680, WDK 10.0.26100:

```c
typedef
    _Check_return_
    _Function_class_DXGK_(DXGKCB_MAPFRAMEBUFFERPOINTER)
    _IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
(APIENTRY CALLBACK *DXGKCB_MAPFRAMEBUFFERPOINTER)(
    IN_CONST_HANDLE                        hAdapter,
    INOUT_PDXGKARGCB_MAPFRAMEBUFFERPOINTER pMapFrameBufferPointer
    );
```

### Description

**DXGKCB_MAPFRAMEBUFFERPOINTER** obtains a pointer to a subregion of the section object that was created for each physical adapter.

### Parameters

#### Param hAdapter [in]

A handle to a display adapter. The driver provides this handle for the master/lead device in the LDA chain.

#### Param pMapFrameBufferPointer [in/out]

Pointer to [**DXGKARGCB_MAPFRAMEBUFFERPOINTER**](#ns-d3dkmddi-_dxgkargcb_mapframebufferpointer) structure that contains a pointer to the subregion of the section object.

### Returns

**DXGKCB_MAPFRAMEBUFFERPOINTER** returns STATUS_SUCCESS if the operation succeeds. Otherwise, it returns an appropriate NTSTATUS error code.

### Remarks

*DXGKCB_XXX* functions are implemented by *Dxgkrnl*. To use this callback function, set the appropriate members of [**DXGKARGCB_MAPFRAMEBUFFERPOINTER**](#ns-d3dkmddi-_dxgkargcb_mapframebufferpointer) and then call **DxgkCbMapFrameBufferPointer** via the [**DXGKRNL_INTERFACE**](dispmprt.md#ns-dispmprt-_dxgkrnl_interface).

See [IOMMU-based GPU isolation](/windows-hardware/drivers/display/iommu-based-gpu-isolation) for more information.

### See also

[**DXGKCB_UNMAPFRAMEBUFFERPOINTER**](#nc-d3dkmddi-dxgkcb_unmapframebufferpointer)

[**DXGKRNL_INTERFACE**](dispmprt.md#ns-dispmprt-_dxgkrnl_interface)

_Min client: Windows 10, version 1803 (WDDM 2.4) | IRQL: PASSIVE_LEVEL | Header: d3dkmddi.h_


<a id="nc-d3dkmddi-dxgkcb_mapmdltoiommu"></a>

## DXGKCB_MAPMDLTOIOMMU callback function

### Declaration
