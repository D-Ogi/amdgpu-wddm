// Host test of the bound of a native virtual fill or transfer: wddm.c's own WddmNativeAllocation
// (generate_native_bound_test.py puts its text at ACTUAL_SOURCE) with the production size rule and range
// check (paging_window.c, linked).
//
// What it holds: an allocation that was sized for 64 KiB host pages is filled by VidMm in the size VidMm was
// given, and that fill must be admitted to the native path. The first 64 KiB build compared it with the size
// the user-mode driver had asked for, and every grown allocation went to the page by page path (hardware result
// 2026-11-25). The negative control (-Mutation blob-size) puts that comparison back; the test must then fail.
#include <stdio.h>
#include <string.h>

#include "paging_window.h"

typedef unsigned char BOOLEAN;
typedef void* HANDLE;
typedef unsigned long ULONG;
typedef unsigned long long ULONGLONG;
typedef long long LONG64;
#define TRUE 1
#define FALSE 0
#define DISPATCH_LEVEL 2
#define BC250_WDDM_MAGIC_ALLOCATION 0x41u

// The members the function reads, with wddm.c's names and types.
typedef struct { volatile LONG64 PagingOutsideBound; } BC250_WDDM;
typedef struct { void* Wddm; } BC250_DEVICE;
typedef struct _BC250_WDDM_OBJECT {
    BC250_DEVICE* Device;
    BOOLEAN SystemContext;
    ULONGLONG RootPhysical;
    BOOLEAN UmdAlloc;
    ULONGLONG UmdBytes, UmdVidMmBytes;
    struct { ULONGLONG Size; } Allocation;
} BC250_WDDM_OBJECT;
typedef struct {
    ULONGLONG DmaBufferGpuVirtualAddress;
    unsigned MultipassOffset;
    struct { HANDLE hAllocation; ULONGLONG AllocationOffsetInBytes, FillSizeInBytes; } FillVirtual;
    struct { HANDLE hAllocation; ULONGLONG AllocationOffsetInBytes, TransferSizeInBytes; } TransferVirtual;
} DXGKARG_BUILDPAGINGBUFFER;

static unsigned g_Logs, g_Bad, g_Checks;
#define CHECK(x) do { ++g_Checks; if (!(x)) { ++g_Bad; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
static void GuardLog(const char* Format, ...) { (void)Format; ++g_Logs; }
static int KeGetCurrentIrql(void) { return 0; }
static LONG64 InterlockedIncrement64(volatile LONG64* Value) { return ++*Value; }
static BC250_WDDM_OBJECT* WddmObject(HANDLE Handle, unsigned Magic) { (void)Magic; return (BC250_WDDM_OBJECT*)Handle; }

/* ACTUAL_SOURCE */

static BC250_WDDM g_Wddm;
static BC250_DEVICE g_Device = { &g_Wddm };
static BC250_WDDM_OBJECT g_Context = { &g_Device, TRUE, 0x40000, FALSE, 0, 0, { 0 } };

// An allocation as WddmCreateAdmit leaves it: UmdBytes is the request; the bound is the request, or the VidMm
// size when the 64 KiB sizes changed the allocation (Sizes: Device->SystemSizes64K and an aperture placement).
static BC250_WDDM_OBJECT Created(ULONGLONG Request, int Sizes, ULONGLONG* VidMmSize)
{
    BC250_WDDM_OBJECT object;
    unsigned long long size = (Request + 4095) & ~4095ull;     // ROUND_TO_PAGES
    unsigned long alignment = 4096;
    memset(&object, 0, sizeof(object));
    object.Device = &g_Device; object.UmdAlloc = TRUE;
    object.UmdBytes = Request; object.UmdVidMmBytes = Request;
    if (Sizes && size >= PAGING_HOST_PAGE_64K) {
        (void)PagingHostPage64KSize(&size, &alignment);
        object.UmdVidMmBytes = size;
    }
    *VidMmSize = size;
    return object;
}

static BOOLEAN Admit(BC250_WDDM_OBJECT* Object, BOOLEAN Fill, ULONGLONG Offset, ULONGLONG Bytes)
{
    DXGKARG_BUILDPAGINGBUFFER build;
    memset(&build, 0, sizeof(build));
    build.DmaBufferGpuVirtualAddress = 0x100000;
    if (Fill) { build.FillVirtual.hAllocation = Object; build.FillVirtual.AllocationOffsetInBytes = Offset;
                build.FillVirtual.FillSizeInBytes = Bytes; }
    else { build.TransferVirtual.hAllocation = Object; build.TransferVirtual.AllocationOffsetInBytes = Offset;
           build.TransferVirtual.TransferSizeInBytes = Bytes; }
    return WddmNativeAllocation(&g_Device, &g_Context, &build, Fill);
}

int main(void)
{
    BC250_WDDM_OBJECT object;
    ULONGLONG vidmm;
    int fill;

    for (fill = 0; fill < 2; fill++) {
        // 1. A 68 KiB request on a start with the 64 KiB sizes: 128 KiB to VidMm. VidMm's operation on the whole
        //    allocation is native, and so is its last page. Nothing is counted or logged.
        memset(&g_Wddm, 0, sizeof(g_Wddm)); g_Logs = 0;
        object = Created(0x11000, 1, &vidmm);
        CHECK(vidmm == 0x20000 && object.UmdBytes == 0x11000);
        CHECK(Admit(&object, (BOOLEAN)fill, 0, vidmm));
        CHECK(Admit(&object, (BOOLEAN)fill, vidmm - 4096, 4096));
        CHECK(Admit(&object, (BOOLEAN)fill, 0x10000, 0x10000));
        CHECK(g_Wddm.PagingOutsideBound == 0 && g_Logs == 0);
        // Past the VidMm size it is not, and that is counted and logged.
        CHECK(!Admit(&object, (BOOLEAN)fill, 0, vidmm + 4096));
        CHECK(!Admit(&object, (BOOLEAN)fill, vidmm, 4096));
        CHECK(!Admit(&object, (BOOLEAN)fill, 0, 0));
        CHECK(g_Wddm.PagingOutsideBound == 3 && g_Logs == 3);

        // 2. A request that is already a multiple of 64 KiB (the 14745600 byte heaps of Time Spy): the same size
        //    with and without the sizes, native in both.
        object = Created(0xE10000, 1, &vidmm);
        CHECK(vidmm == 0xE10000 && Admit(&object, (BOOLEAN)fill, 0, vidmm));
        object = Created(0xE10000, 0, &vidmm);
        CHECK(vidmm == 0xE10000 && Admit(&object, (BOOLEAN)fill, 0, vidmm));

        // 3. Without the sizes nothing grows, and the bound is the request, as on every earlier driver.
        memset(&g_Wddm, 0, sizeof(g_Wddm));
        object = Created(0x11000, 0, &vidmm);
        CHECK(vidmm == 0x11000 && object.UmdVidMmBytes == 0x11000);
        CHECK(Admit(&object, (BOOLEAN)fill, 0, 0x11000) && !Admit(&object, (BOOLEAN)fill, 0, 0x12000));
        CHECK(g_Wddm.PagingOutsideBound == 1);

        // 4. Below 64 KiB the sizes change nothing.
        object = Created(0xF000, 1, &vidmm);
        CHECK(vidmm == 0xF000 && Admit(&object, (BOOLEAN)fill, 0, 0xF000) && !Admit(&object, (BOOLEAN)fill, 0, 0x10000));

        // 5. An allocation that is not a UMD one keeps its own size as the bound.
        memset(&object, 0, sizeof(object));
        object.Device = &g_Device; object.Allocation.Size = 0x30000; object.UmdVidMmBytes = 0x1000;
        CHECK(Admit(&object, (BOOLEAN)fill, 0, 0x30000) && !Admit(&object, (BOOLEAN)fill, 0, 0x31000));
    }
    // 6. The other conditions of the function are as they were: no context, another device, no DMA address.
    {
        DXGKARG_BUILDPAGINGBUFFER build;
        BC250_DEVICE other = { NULL };
        object = Created(0x11000, 1, &vidmm);
        memset(&build, 0, sizeof(build));
        build.FillVirtual.hAllocation = &object; build.FillVirtual.FillSizeInBytes = vidmm;
        CHECK(!WddmNativeAllocation(&g_Device, &g_Context, &build, TRUE));        // no DMA address
        build.DmaBufferGpuVirtualAddress = 0x100000;
        CHECK(WddmNativeAllocation(&g_Device, &g_Context, &build, TRUE));
        CHECK(!WddmNativeAllocation(&g_Device, NULL, &build, TRUE));
        CHECK(!WddmNativeAllocation(&other, &g_Context, &build, TRUE));
        build.MultipassOffset = 0x40000000u;
        CHECK(!WddmNativeAllocation(&g_Device, &g_Context, &build, TRUE));
        build.MultipassOffset = 0; build.FillVirtual.hAllocation = NULL;
        CHECK(!WddmNativeAllocation(&g_Device, &g_Context, &build, TRUE));
    }
    printf("native fill and transfer bound: %u checks, %u failures\n", g_Checks, g_Bad);
    return g_Bad ? 1 : 0;
}
