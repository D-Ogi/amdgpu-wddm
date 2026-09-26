/* The kernel-header half of the QueryAdapterInfo host harness.
 *
 * This file is compiled against the WDK's km headers, at the same DXGKDDI_INTERFACE_VERSION as the
 * driver (bc250kmd.h pins 0x5023), and is linked together with the REAL driver\kmd\wddm.c. Not a copy
 * of it, not a transcription: the same file the miniport is built from, compiled a second time without
 * /kernel and with the kernel services replaced by host_stubs.c. wddm.c is not modified in any way and
 * knows nothing about this harness.
 *
 * The way in is WddmBuildTable(): it is the one exported function that hands out the addresses of the
 * static DDIs, so the harness calls DxgkDdiQueryAdapterInfo exactly the way dxgkrnl does, through the
 * table pointer, rather than through a symbol wddm.c would have had to expose for testing.
 *
 * Adapter geometry is supplied explicitly. The default retains the historical 8 GiB host fixture;
 * larger fixtures are synthetic, not measurements or predictions of firmware layout. No discovery
 * or register access is performed by this initializer: Device->Mmio remains NULL.
 */
#include "bc250kmd.h"
#include "qai_bridge.h"

/* ---- the adapter ------------------------------------------------------------------------------- */

static BC250_DEVICE                 g_Device;
static DRIVER_INITIALIZATION_DATA   g_Table;
static BOOLEAN                      g_Started;

/* Geometry belongs to the caller, not to a hidden assumption in the bridge. */
int bc250h_fixture_geometry(unsigned gib, struct bc250h_geometry* geometry)
{
    if (geometry == NULL || (gib != 8 && gib != 12 && gib != 16)) return 0;
    geometry->vram_length = (unsigned long long)gib << 30;
    geometry->vram_physical = gib == 8 ? 0x270000000ull :
                              gib == 12 ? 0x670000000ull : 0x1270000000ull;
    geometry->vram_mc_base = gib == 8 ? 0xF400000000ull :
                             gib == 12 ? 0xE800000000ull : 0xDC00000000ull;
    geometry->fb_width = 1920;
    geometry->fb_height = 1200;
    geometry->fb_pitch = 7680;
    return 1;
}

int bc250h_start(int vram_enabled)
{
    struct bc250h_geometry geometry;
    if (!bc250h_fixture_geometry(8, &geometry)) return 4;
    return bc250h_start_geometry(vram_enabled, &geometry);
}

int bc250h_start_geometry(int vram_enabled, const struct bc250h_geometry* geometry)
{
    if (g_Started) return 1;
    if (geometry == NULL || !geometry->vram_length || !geometry->fb_width ||
        !geometry->fb_height || (geometry->fb_pitch & 3u) ||
        (ULONGLONG)geometry->fb_width * 4 > geometry->fb_pitch ||
        (ULONGLONG)geometry->fb_pitch * geometry->fb_height > geometry->vram_length ||
        geometry->vram_physical > MAXULONGLONG - geometry->vram_length ||
        geometry->vram_mc_base > MAXULONGLONG - geometry->vram_length) return 4;
    RtlZeroMemory(&g_Device, sizeof(g_Device));
    RtlZeroMemory(&g_Table, sizeof(g_Table));

    g_Device.Post.Width = geometry->fb_width;
    g_Device.Post.Height = geometry->fb_height;
    g_Device.Post.Pitch = geometry->fb_pitch;
    g_Device.Post.ColorFormat = D3DDDIFMT_X8R8G8B8;
    g_Device.Post.PhysicAddress.QuadPart = (LONGLONG)geometry->vram_physical;
    g_Device.Post.TargetId = BC250_CHILD_UID;
    g_Device.Post.AcpiId = 0;

    if (vram_enabled)
    {
        g_Device.VramEnabled = TRUE;
        g_Device.VramLength = geometry->vram_length;
        g_Device.VramPhysical.QuadPart = (LONGLONG)geometry->vram_physical;
        g_Device.VramMcBase = geometry->vram_mc_base;
    }

    /* The shims must answer before WddmStart runs: it calls VidMmStart. */
    {
        struct bc250h_gfx_control control;

        bc250h_shim_defaults(&control);
        bc250h_shim_set(&control);
        bc250h_shim_reset_counters();
    }

    /* GuardConsumeSetting is stubbed to answer 2 for EnableFullWddm, so the gate opens. */
    if (!WddmGateOpen()) return 2;
    WddmBuildTable(&g_Table);
    WddmStart(&g_Device);
    if (g_Device.Wddm == NULL) return 3;
    g_Started = TRUE;
    return 0;
}

void bc250h_stop(void)
{
    if (!g_Started) return;
    WddmStop(&g_Device);
    g_Started = FALSE;
}

/* ---- the table --------------------------------------------------------------------------------- */

unsigned bc250h_table_bytes(void)
{
    return (unsigned)sizeof(DRIVER_INITIALIZATION_DATA);
}

/* Counted, not named: every pointer-sized slot of the structure past Version. The structure is nothing
 * but function pointers after its first ULONG, so this is the number of DDIs the table carries. */
unsigned bc250h_table_pointers_set(void)
{
    const UCHAR* base = (const UCHAR*)&g_Table;
    SIZE_T offset;
    unsigned set = 0;

    for (offset = sizeof(PVOID); offset + sizeof(PVOID) <= sizeof(g_Table); offset += sizeof(PVOID))
        if (*(PVOID* const*)(base + offset) != NULL) set++;
    return set;
}

/* The members Learn marks "reserved and should be set to zero", the list wddm.c's own WddmCheckReserved
 * carries. Checked here as well because WddmCheckReserved only writes a log line. */
int bc250h_table_reserved_clean(void)
{
    static const SIZE_T reserved[] = {
        FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, DxgkDdiDescribePageTable),
        FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, DxgkDdiUpdatePageTable),
        FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, DxgkDdiUpdatePageDirectory),
        FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, DxgkDdiMovePageDirectory),
        FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, DxgkDdiSubmitRender),
        FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, DxgkDdiCreateAllocation2),
        FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, Reserved),
        FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, DxgkDdiSetPowerPState),
        FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, Reserved1),
        FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, Reserved2),
        FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, DxgkDdiAcquireSwizzlingRange),
        FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, DxgkDdiReleaseSwizzlingRange),
    };
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(reserved); i++)
        if (*(PVOID* const*)((const UCHAR*)&g_Table + reserved[i]) != NULL) return 0;
    return 1;
}

unsigned bc250h_drivercaps_size(void)
{
    return (unsigned)sizeof(DXGK_DRIVERCAPS);
}

unsigned bc250h_segment_descriptor_stride(void)
{
    return (unsigned)sizeof(DXGK_SEGMENTDESCRIPTOR4);
}

/* ---- the case table ---------------------------------------------------------------------------- */

#define SEG_PAD1_OFF    (FIELD_OFFSET(DXGK_QUERYSEGMENTOUT4, NbSegment) + sizeof(UINT))
#define SEG_PAD1_LEN    (FIELD_OFFSET(DXGK_QUERYSEGMENTOUT4, pSegmentDescriptor) - SEG_PAD1_OFF)
#define SEG_PAD2_OFF    (FIELD_OFFSET(DXGK_QUERYSEGMENTOUT4, PagingBufferPrivateDataSize) + sizeof(UINT))
#define SEG_PAD2_LEN    (FIELD_OFFSET(DXGK_QUERYSEGMENTOUT4, SegmentDescriptorStride) - SEG_PAD2_OFF)

static const struct bc250h_region g_SegmentCountIgnore[] = {
    { (unsigned)SEG_PAD1_OFF, (unsigned)(sizeof(DXGK_QUERYSEGMENTOUT4) - SEG_PAD1_OFF),
      "pass 1 sets NbSegment and must not touch any other member" },
};

static const struct bc250h_region g_SegmentFillIgnore[] = {
    { (unsigned)SEG_PAD1_OFF, (unsigned)SEG_PAD1_LEN, "padding after NbSegment" },
    { (unsigned)FIELD_OFFSET(DXGK_QUERYSEGMENTOUT4, pSegmentDescriptor), (unsigned)sizeof(BYTE*),
      "in: the descriptor array dxgkrnl allocated" },
    { (unsigned)SEG_PAD2_OFF, (unsigned)SEG_PAD2_LEN, "padding before SegmentDescriptorStride" },
    { (unsigned)FIELD_OFFSET(DXGK_QUERYSEGMENTOUT4, SegmentDescriptorStride), (unsigned)sizeof(SIZE_T),
      "in: the stride dxgkrnl chose" },
};

/* out_size for the refused types: dxgkrnl's real buffer sizes for 15 and 47 are not public, and the
 * driver must not look at the buffer at all, so any size proves the same thing. */
#define BC250H_REFUSED_SIZE 64u

static const struct bc250h_case g_Cases[] = {
    { "DRIVERCAPS (1)", 1, (unsigned)sizeof(DXGK_DRIVERCAPS), 0,
      BC250H_STATUS_SUCCESS, BC250H_STATUS_BUFFER_TOO_SMALL, BC250H_STATUS_BUFFER_TOO_SMALL, 0,
      BC250H_PREP_NONE, 1, 0, NULL },

    { "NUMPOWERCOMPONENTS (6)", 6, (unsigned)sizeof(UINT), 0,
      BC250H_STATUS_SUCCESS, BC250H_STATUS_BUFFER_TOO_SMALL, BC250H_STATUS_BUFFER_TOO_SMALL, 0,
      BC250H_PREP_NONE, 1, 0, NULL },

    { "HISTORYBUFFERPRECISION (10)", 10, (unsigned)sizeof(DXGKARG_HISTORYBUFFERPRECISION), 0,
      BC250H_STATUS_SUCCESS, BC250H_STATUS_BUFFER_TOO_SMALL, BC250H_STATUS_BUFFER_TOO_SMALL, 0,
      BC250H_PREP_NONE, 1, 0, NULL },

    { "QUERYSEGMENT4 (11) pass 1, count", 11, (unsigned)sizeof(DXGK_QUERYSEGMENTOUT4), 0,
      BC250H_STATUS_SUCCESS, BC250H_STATUS_BUFFER_TOO_SMALL, BC250H_STATUS_BUFFER_TOO_SMALL, 0,
      BC250H_PREP_SEGMENT_COUNT, 0, RTL_NUMBER_OF(g_SegmentCountIgnore), g_SegmentCountIgnore },

    { "QUERYSEGMENT4 (11) pass 2, fill", 11, (unsigned)sizeof(DXGK_QUERYSEGMENTOUT4), 0,
      BC250H_STATUS_SUCCESS, BC250H_STATUS_BUFFER_TOO_SMALL, BC250H_STATUS_BUFFER_TOO_SMALL, 0,
      BC250H_PREP_SEGMENT_FILL, 0, RTL_NUMBER_OF(g_SegmentFillIgnore), g_SegmentFillIgnore, 0 },

    /* dxgkrnl picks the stride; a driver that iterates with sizeof() instead breaks the moment the
     * two disagree, which is exactly what happens when the OS is newer than the driver. Larger must
     * work, smaller must be refused before anything is written. */
    { "QUERYSEGMENT4 (11) pass 2, stride larger than the descriptor", 11,
      (unsigned)sizeof(DXGK_QUERYSEGMENTOUT4), 0,
      BC250H_STATUS_SUCCESS, BC250H_STATUS_BUFFER_TOO_SMALL, BC250H_STATUS_BUFFER_TOO_SMALL, 0,
      BC250H_PREP_SEGMENT_FILL, 0, RTL_NUMBER_OF(g_SegmentFillIgnore), g_SegmentFillIgnore,
      (unsigned)sizeof(DXGK_SEGMENTDESCRIPTOR4) + 32u },

    { "QUERYSEGMENT4 (11) pass 2, stride smaller than the descriptor", 11,
      (unsigned)sizeof(DXGK_QUERYSEGMENTOUT4), 0,
      BC250H_STATUS_INVALID_PARAMETER, BC250H_STATUS_BUFFER_TOO_SMALL, BC250H_STATUS_BUFFER_TOO_SMALL, 0,
      BC250H_PREP_SEGMENT_FILL, 0, RTL_NUMBER_OF(g_SegmentFillIgnore), g_SegmentFillIgnore,
      (unsigned)sizeof(DXGK_SEGMENTDESCRIPTOR4) - 8u },

    /* NbSegment non-zero (so: pass 2) with no array to write into. The pass-1 signal is NbSegment
     * zero, not a NULL pointer, so this combination is an error and must not be treated as pass 1. */
    { "QUERYSEGMENT4 (11) pass 2, pSegmentDescriptor NULL", 11,
      (unsigned)sizeof(DXGK_QUERYSEGMENTOUT4), 0,
      BC250H_STATUS_INVALID_PARAMETER, BC250H_STATUS_BUFFER_TOO_SMALL, BC250H_STATUS_BUFFER_TOO_SMALL, 0,
      BC250H_PREP_SEGMENT_NODESC, 0, RTL_NUMBER_OF(g_SegmentFillIgnore), g_SegmentFillIgnore, 0 },

    { "GPUMMUCAPS (13)", 13, (unsigned)sizeof(DXGK_GPUMMUCAPS), 0,
      BC250H_STATUS_SUCCESS, BC250H_STATUS_BUFFER_TOO_SMALL, BC250H_STATUS_BUFFER_TOO_SMALL, 0,
      BC250H_PREP_NONE, 1, 0, NULL },

    { "PAGETABLELEVELDESC (14) level 0", 14, (unsigned)sizeof(DXGK_PAGE_TABLE_LEVEL_DESC),
      (unsigned)sizeof(DXGK_QUERYPAGETABLELEVELDESCIN),
      BC250H_STATUS_SUCCESS, BC250H_STATUS_BUFFER_TOO_SMALL, BC250H_STATUS_BUFFER_TOO_SMALL, 0,
      BC250H_PREP_NONE, 1, 0, NULL },

    { "PAGETABLELEVELDESC (14) level 3", 14, (unsigned)sizeof(DXGK_PAGE_TABLE_LEVEL_DESC),
      (unsigned)sizeof(DXGK_QUERYPAGETABLELEVELDESCIN),
      BC250H_STATUS_SUCCESS, BC250H_STATUS_BUFFER_TOO_SMALL, BC250H_STATUS_BUFFER_TOO_SMALL, 0,
      BC250H_PREP_NONE, 1, 0, NULL },

    { "PAGETABLELEVELDESC (14) level 4, out of range", 14, (unsigned)sizeof(DXGK_PAGE_TABLE_LEVEL_DESC),
      (unsigned)sizeof(DXGK_QUERYPAGETABLELEVELDESCIN),
      BC250H_STATUS_INVALID_PARAMETER, BC250H_STATUS_BUFFER_TOO_SMALL, BC250H_STATUS_BUFFER_TOO_SMALL, 0,
      BC250H_PREP_NONE, 0, 0, NULL },

    /* Refused on purpose. 15 and 47 are the two the lab's dxgkrnl really asked for (E16 run 003). */
    { "UMDRIVERPRIVATE (0), refused", 0, BC250H_REFUSED_SIZE, 0,
      BC250H_STATUS_NOT_SUPPORTED, BC250H_STATUS_NOT_SUPPORTED, BC250H_STATUS_NOT_SUPPORTED, 0,
      BC250H_PREP_NONE, 0, 0, NULL },
    { "QUERYSEGMENT (2), refused", 2, BC250H_REFUSED_SIZE, 0,
      BC250H_STATUS_NOT_SUPPORTED, BC250H_STATUS_NOT_SUPPORTED, BC250H_STATUS_NOT_SUPPORTED, 0,
      BC250H_PREP_NONE, 0, 0, NULL },
    { "QUERYSEGMENT2 (4), refused", 4, BC250H_REFUSED_SIZE, 0,
      BC250H_STATUS_NOT_SUPPORTED, BC250H_STATUS_NOT_SUPPORTED, BC250H_STATUS_NOT_SUPPORTED, 0,
      BC250H_PREP_NONE, 0, 0, NULL },
    { "QUERYSEGMENT3 (5), refused", 5, BC250H_REFUSED_SIZE, 0,
      BC250H_STATUS_NOT_SUPPORTED, BC250H_STATUS_NOT_SUPPORTED, BC250H_STATUS_NOT_SUPPORTED, 0,
      BC250H_PREP_NONE, 0, 0, NULL },
    { "POWERCOMPONENTINFO (7), refused", 7, BC250H_REFUSED_SIZE, 0,
      BC250H_STATUS_NOT_SUPPORTED, BC250H_STATUS_NOT_SUPPORTED, BC250H_STATUS_NOT_SUPPORTED, 0,
      BC250H_PREP_NONE, 0, 0, NULL },
    { "PHYSICALADAPTERCAPS (15), refused", 15, BC250H_REFUSED_SIZE, 0,
      BC250H_STATUS_NOT_SUPPORTED, BC250H_STATUS_NOT_SUPPORTED, BC250H_STATUS_NOT_SUPPORTED, 0,
      BC250H_PREP_NONE, 0, 0, NULL },
    { "DISPLAY_DRIVERCAPS_EXTENSION (16), refused", 16, BC250H_REFUSED_SIZE, 0,
      BC250H_STATUS_NOT_SUPPORTED, BC250H_STATUS_NOT_SUPPORTED, BC250H_STATUS_NOT_SUPPORTED, 0,
      BC250H_PREP_NONE, 0, 0, NULL },
    { "64BITONLYCAPS (47), refused", 47, BC250H_REFUSED_SIZE, 0,
      BC250H_STATUS_NOT_SUPPORTED, BC250H_STATUS_NOT_SUPPORTED, BC250H_STATUS_NOT_SUPPORTED, 0,
      BC250H_PREP_NONE, 0, 0, NULL },
};

unsigned bc250h_case_count(void)
{
    return RTL_NUMBER_OF(g_Cases);
}

const struct bc250h_case* bc250h_case(unsigned index)
{
    return (index < RTL_NUMBER_OF(g_Cases)) ? &g_Cases[index] : NULL;
}

void bc250h_prepare(int prep, void* out, unsigned out_size, void* descriptors, unsigned nb_segment,
                    unsigned stride)
{
    DXGK_QUERYSEGMENTOUT4* segment = (DXGK_QUERYSEGMENTOUT4*)out;

    if (out == NULL || out_size < sizeof(*segment)) return;
    switch (prep)
    {
    case BC250H_PREP_SEGMENT_COUNT:
        segment->NbSegment = 0;
        break;
    case BC250H_PREP_SEGMENT_FILL:
        /* At least 1: NbSegment = 0 is the pass-1 signal, so handing the real count of 0 (the
         * EnableVram-closed configuration) would turn this case into a pass-1 call and test nothing.
         * The driver's own guard is NbSegment < count, which 1 satisfies when count is 0. */
        segment->NbSegment = (nb_segment != 0) ? nb_segment : 1u;
        segment->pSegmentDescriptor = (BYTE*)descriptors;
        segment->SegmentDescriptorStride =
            (stride != 0) ? (SIZE_T)stride : sizeof(DXGK_SEGMENTDESCRIPTOR4);
        break;
    case BC250H_PREP_SEGMENT_NODESC:
        /* At least 1, so that this is unambiguously pass 2 even when no segment is declared:
         * NbSegment = 0 is the pass-1 signal and would make the case test nothing. */
        segment->NbSegment = (nb_segment != 0) ? nb_segment : 1u;
        segment->pSegmentDescriptor = NULL;
        segment->SegmentDescriptorStride = sizeof(DXGK_SEGMENTDESCRIPTOR4);
        break;
    default:
        break;
    }
}

/* ---- the call ---------------------------------------------------------------------------------- */

int bc250h_call(unsigned type, void* in, unsigned in_size, void* out, unsigned out_size, long* status)
{
    DXGKARG_QUERYADAPTERINFO arg;

    RtlZeroMemory(&arg, sizeof(arg));
    arg.Type = (DXGK_QUERYADAPTERINFOTYPE)type;
    arg.pInputData = in;
    arg.InputDataSize = in_size;
    arg.pOutputData = out;
    arg.OutputDataSize = out_size;

    if (g_Table.DxgkDdiQueryAdapterInfo == NULL) { *status = BC250H_STATUS_NOT_SUPPORTED; return 2; }

    /* A NULL output buffer is one of the cases this harness is for, and two of the driver's handlers
     * dereference it without a check. Catching the fault here turns a defect into a test result
     * instead of into a dead test process. */
    __try
    {
        *status = (long)g_Table.DxgkDdiQueryAdapterInfo((HANDLE)&g_Device, &arg);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *status = (long)GetExceptionCode();
        return 1;
    }
    return 0;
}

/* ---- DxgkDdiCreateContext ----------------------------------------------------------------------
 *
 * Run 004 died one call after this DDI returned, so its answer gets the same treatment as the
 * QueryAdapterInfo answers: every input combination dxgkrnl can present, and the whole
 * DXGK_CONTEXTINFO that comes back, recorded rather than assumed.
 *
 * The flag combinations are the ones DXGK_CREATECONTEXTFLAGS defines at 0x5023 and that dxgmms2 or a
 * user-mode device can produce. 0x5 (SystemContext | VirtualAddressing) is the one unit A really
 * sent (facts M65); the others are what the same code would be asked on a machine where a node
 * reports no GpuMmu, or where GDI asks for a context of its own.
 */

#define BC250H_CTX_SYSTEM       0x1u    /* DXGK_CREATECONTEXTFLAGS.SystemContext */
#define BC250H_CTX_GDI          0x2u    /* .GdiContext */
#define BC250H_CTX_VIRTUAL      0x4u    /* .VirtualAddressing */

static HANDLE   g_CtxDevice;            /* a real device handle, from DxgkDdiCreateDevice */
static HANDLE   g_CtxProcess;           /* a live handle of the WRONG type, for BC250H_DEV_WRONG */
static HANDLE   g_CtxLast;              /* the context the last successful call created */

static const struct bc250h_ctx_case g_CtxCases[] = {
    /* The one the lab really sent. */
    { "SystemContext | VirtualAddressing, node 0 (E16 run 004)",
      BC250H_CTX_SYSTEM | BC250H_CTX_VIRTUAL, 0, 0, BC250H_DEV_VALID, BC250H_STATUS_SUCCESS, 1 },

    { "VirtualAddressing only, node 0",
      BC250H_CTX_VIRTUAL, 0, 0, BC250H_DEV_VALID, BC250H_STATUS_SUCCESS, 1 },

    { "SystemContext only, node 0 (no virtual addressing)",
      BC250H_CTX_SYSTEM, 0, 0, BC250H_DEV_VALID, BC250H_STATUS_SUCCESS, 1 },

    { "no flags, node 0 (a plain render context)",
      0, 0, 0, BC250H_DEV_VALID, BC250H_STATUS_SUCCESS, 1 },

    { "GdiContext, node 0",
      BC250H_CTX_GDI, 0, 0, BC250H_DEV_VALID, BC250H_STATUS_SUCCESS, 1 },

    { "GdiContext | VirtualAddressing, node 0",
      BC250H_CTX_GDI | BC250H_CTX_VIRTUAL, 0, 0, BC250H_DEV_VALID, BC250H_STATUS_SUCCESS, 1 },

    { "SystemContext | GdiContext | VirtualAddressing, node 0",
      BC250H_CTX_SYSTEM | BC250H_CTX_GDI | BC250H_CTX_VIRTUAL, 0, 0, BC250H_DEV_VALID,
      BC250H_STATUS_SUCCESS, 1 },

    { "engine affinity 1, node 0",
      BC250H_CTX_SYSTEM | BC250H_CTX_VIRTUAL, 0, 1, BC250H_DEV_VALID, BC250H_STATUS_SUCCESS, 1 },

    /* Out of range. One node is declared, so anything but ordinal 0 must be refused - the same
     * bound GetNodeMetadata enforces and DRIVERCAPS announced. */
    { "node 1, out of range",
      BC250H_CTX_SYSTEM | BC250H_CTX_VIRTUAL, 1, 0, BC250H_DEV_VALID,
      BC250H_STATUS_INVALID_PARAMETER, 0 },
    { "node 0xFFFFFFFF, out of range",
      BC250H_CTX_SYSTEM | BC250H_CTX_VIRTUAL, 0xFFFFFFFFu, 0, BC250H_DEV_VALID,
      BC250H_STATUS_INVALID_PARAMETER, 0 },

    /* Bad handles. Not something dxgkrnl does, but the driver's own guard is what is being read. */
    { "hDevice NULL",
      BC250H_CTX_SYSTEM | BC250H_CTX_VIRTUAL, 0, 0, BC250H_DEV_NULL,
      BC250H_STATUS_INVALID_PARAMETER, 0 },
    { "hDevice is a process handle (wrong object type)",
      BC250H_CTX_SYSTEM | BC250H_CTX_VIRTUAL, 0, 0, BC250H_DEV_WRONG,
      BC250H_STATUS_INVALID_PARAMETER, 0 },
};

unsigned bc250h_ctx_case_count(void)
{
    return RTL_NUMBER_OF(g_CtxCases);
}

const struct bc250h_ctx_case* bc250h_ctx_case(unsigned index)
{
    return (index < RTL_NUMBER_OF(g_CtxCases)) ? &g_CtxCases[index] : NULL;
}

unsigned bc250h_ctx_arg_size(void)      { return (unsigned)sizeof(DXGKARG_CREATECONTEXT); }
unsigned bc250h_ctx_info_offset(void)   { return (unsigned)FIELD_OFFSET(DXGKARG_CREATECONTEXT, ContextInfo); }
unsigned bc250h_ctx_info_size(void)     { return (unsigned)sizeof(DXGK_CONTEXTINFO); }
unsigned bc250h_ctx_handle_offset(void) { return (unsigned)FIELD_OFFSET(DXGKARG_CREATECONTEXT, hContext); }
unsigned bc250h_ctx_handle_size(void)   { return (unsigned)sizeof(HANDLE); }

int bc250h_ddi_patch_present(void)  { return g_Table.DxgkDdiPatch != NULL; }
int bc250h_ddi_render_present(void) { return g_Table.DxgkDdiRender != NULL; }

int bc250h_ctx_begin(void)
{
    DXGKARG_CREATEDEVICE  device;
    DXGKARG_CREATEPROCESS process;
    NTSTATUS status;

    if (!g_Started) return 1;
    if (g_Table.DxgkDdiCreateDevice == NULL || g_Table.DxgkDdiCreateProcess == NULL) return 2;

    RtlZeroMemory(&device, sizeof(device));
    status = g_Table.DxgkDdiCreateDevice((HANDLE)&g_Device, &device);
    if (!NT_SUCCESS(status) || device.hDevice == NULL) return 3;
    g_CtxDevice = device.hDevice;

    /* A live object of the wrong type. Passing a freed handle or a wild pointer would test the
     * allocator, not the driver; a process handle is exactly what a mixed-up caller would hand over
     * and is what the magic-number check exists for. */
    RtlZeroMemory(&process, sizeof(process));
    status = g_Table.DxgkDdiCreateProcess((HANDLE)&g_Device, &process);
    if (!NT_SUCCESS(status) || process.hKmdProcess == NULL) return 4;
    g_CtxProcess = process.hKmdProcess;
    return 0;
}

void bc250h_ctx_end(void)
{
    bc250h_ctx_destroy_last();
    if (g_CtxProcess != NULL && g_Table.DxgkDdiDestroyProcess != NULL)
        (void)g_Table.DxgkDdiDestroyProcess((HANDLE)&g_Device, g_CtxProcess);
    if (g_CtxDevice != NULL && g_Table.DxgkDdiDestroyDevice != NULL)
        (void)g_Table.DxgkDdiDestroyDevice(g_CtxDevice);
    g_CtxProcess = NULL;
    g_CtxDevice = NULL;
}

void bc250h_ctx_destroy_last(void)
{
    if (g_CtxLast != NULL && g_Table.DxgkDdiDestroyContext != NULL)
        (void)g_Table.DxgkDdiDestroyContext(g_CtxLast);
    g_CtxLast = NULL;
}

int bc250h_ctx_call(const struct bc250h_ctx_case* test_case, void* arg, void* snapshot,
                    struct bc250h_ctx_observed* out, long* status)
{
    DXGKARG_CREATECONTEXT* create = (DXGKARG_CREATECONTEXT*)arg;
    HANDLE device;

    RtlZeroMemory(out, sizeof(*out));
    if (g_Table.DxgkDdiCreateContext == NULL) { *status = BC250H_STATUS_NOT_SUPPORTED; return 2; }

    switch (test_case->device)
    {
    case BC250H_DEV_NULL:  device = NULL;          break;
    case BC250H_DEV_WRONG: device = g_CtxProcess;  break;
    default:               device = g_CtxDevice;   break;
    }

    /* The input members only. Everything else in the argument keeps the caller's fill pattern, so
     * the comparison against the snapshot afterwards shows exactly what the driver wrote. */
    create->hContext = NULL;
    create->Flags.Value = test_case->flags;
    create->NodeOrdinal = test_case->node;
    create->EngineAffinity = test_case->engine_affinity;
    create->pPrivateDriverData = NULL;
    create->PrivateDriverDataSize = 0;
    RtlCopyMemory(snapshot, arg, sizeof(DXGKARG_CREATECONTEXT));

    __try
    {
        *status = (long)g_Table.DxgkDdiCreateContext(device, create);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *status = (long)GetExceptionCode();
        return 1;
    }

    out->dma_buffer_size = create->ContextInfo.DmaBufferSize;
    out->dma_buffer_segment_set = create->ContextInfo.DmaBufferSegmentSet;
    out->dma_buffer_private_data_size = create->ContextInfo.DmaBufferPrivateDataSize;
    out->allocation_list_size = create->ContextInfo.AllocationListSize;
    out->patch_location_list_size = create->ContextInfo.PatchLocationListSize;
    out->caps = create->ContextInfo.Caps.Value;
    out->paging_companion_node_id = create->ContextInfo.PagingCompanionNodeId;
    out->context_handle_set = (create->hContext != NULL);
    if (create->hContext != NULL)
    {
        bc250h_ctx_destroy_last();
        g_CtxLast = create->hContext;
    }
    return 0;
}

/* ---- the stage B and stage C shims --------------------------------------------------------------
 *
 * vidmm.c and gfx.c are not compiled into this harness: they carry the register map and the shim
 * types, and what is under test is wddm.c's side of the contract with them. These replacements answer
 * whatever a test tells them to, which is how the paths that only exist when the ring refuses, is not
 * ready or never fences can be reached on a machine with no GPU.
 */

static struct bc250h_gfx_control    g_Gfx;
static struct bc250h_shim_counters  g_Shim;
static ULONG                        g_GfxSeq;

/* wddm.c keeps BC250_WDDM_SEGMENT_VRAM and BC250_WDDM_OBJECT to itself, which is right: the harness
 * has no business reading the driver's private state. The id below only travels as far as the
 * VidMmRootPhysical shim, which ignores it, and what the driver recorded on the context is read where
 * it is actually used - as the root gfx.c is handed on the next submit. */
#define BC250H_SEGMENT_VRAM 1u

void bc250h_shim_defaults(struct bc250h_gfx_control* out)
{
    out->submit_ready = 1;
    out->submit_ib_status = 0;              /* STATUS_SUCCESS */
    out->fence_arrived = 1;
    out->root_physical_ok = 1;
    out->root_physical = 0x1234000ull;
    out->translate_ok = 1;
    out->translate_system = 0;
    out->translate_physical = 0;
}

void bc250h_shim_set(const struct bc250h_gfx_control* control) { g_Gfx = *control; }
void bc250h_shim_counters(struct bc250h_shim_counters* out) { *out = g_Shim; }
void bc250h_shim_reset_counters(void) { RtlZeroMemory(&g_Shim, sizeof(g_Shim)); }

NTSTATUS GfxSubmitIb(_Inout_ BC250_DEVICE* Device, ULONG Vmid, ULONGLONG RootPhysical, ULONGLONG GpuAddress,
                     ULONG SizeBytes, _Out_ ULONG* Seq)
{
    UNREFERENCED_PARAMETER(Device);
    g_Shim.submit_ib++;
    g_Shim.last_vmid = Vmid;
    g_Shim.last_root = RootPhysical;
    g_Shim.last_gpu_address = GpuAddress;
    g_Shim.last_size = SizeBytes;
    *Seq = ++g_GfxSeq;
    return (NTSTATUS)g_Gfx.submit_ib_status;
}

BOOLEAN GfxFenceArrived(_Inout_ BC250_DEVICE* Device, ULONG Seq)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Seq);
    g_Shim.fence_arrived++;
    return (BOOLEAN)(g_Gfx.fence_arrived != 0);
}

BOOLEAN GfxSubmitReady(_In_ const BC250_DEVICE* Device)
{
    UNREFERENCED_PARAMETER(Device);
    g_Shim.submit_ready++;
    return (BOOLEAN)(g_Gfx.submit_ready != 0);
}

void GfxSubmitFail(_Inout_ BC250_DEVICE* Device)
{
    UNREFERENCED_PARAMETER(Device);
    g_Shim.submit_fail++;
}

NTSTATUS VidMmStart(_In_ const BC250_DEVICE* Device, ULONGLONG SegmentOffset, ULONGLONG SegmentLength, ULONG VramSegmentId)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(SegmentOffset);
    UNREFERENCED_PARAMETER(SegmentLength);
    UNREFERENCED_PARAMETER(VramSegmentId);
    g_Shim.vidmm_start++;
    return STATUS_SUCCESS;
}

void VidMmStop(void) { g_Shim.vidmm_stop++; }

NTSTATUS VidMmUpdatePageTable(_In_ const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE* Update)
{
    UNREFERENCED_PARAMETER(Update);
    g_Shim.vidmm_update_page_table++;
    return STATUS_SUCCESS;
}

void VidMmSetRootPageTable(_In_ const DXGKARG_SETROOTPAGETABLE* Root)
{
    UNREFERENCED_PARAMETER(Root);
    g_Shim.vidmm_set_root_page_table++;
}

BOOLEAN VidMmRootPhysical(_In_ const D3DGPU_PHYSICAL_ADDRESS* Address, _Out_ ULONGLONG* Physical)
{
    UNREFERENCED_PARAMETER(Address);
    g_Shim.vidmm_root_physical++;
    *Physical = g_Gfx.root_physical;
    return (BOOLEAN)(g_Gfx.root_physical_ok != 0);
}

/* 0.7.16's WddmPresentBlit walks a source allocation's VA. The shim answers from bc250h_gfx_control so a
 * test can make the translation fail or land in system memory, which are the two refusals the blit has to
 * survive; the physical address it hands back is never dereferenced, because nothing in this harness calls
 * the blit yet. */
BOOLEAN VidMmTranslate(ULONGLONG RootPhysical, ULONGLONG Va, _Out_ ULONGLONG* Physical, _Out_ BOOLEAN* System)
{
    UNREFERENCED_PARAMETER(RootPhysical);
    g_Shim.vidmm_translate++;
    *Physical = g_Gfx.translate_physical + Va;
    *System = (BOOLEAN)(g_Gfx.translate_system != 0);
    return (BOOLEAN)(g_Gfx.translate_ok != 0);
}

void VidMmSummary(void) { g_Shim.vidmm_summary++; }

/* ---- the submit matrix --------------------------------------------------------------------------
 *
 * Every row must end in STATUS_SUCCESS. The DDI is one of the four that bugcheck 0x119 if they fail
 * (parameter 1 = 0x2), so "the ring said no" is not an answer it may pass upwards.
 */

#define BC250H_GFX_OK       {1, 0,            1, 1, 0x1234000ull}
#define BC250H_GFX_BUSY     {1, (long)0xC0000012L, 1, 1, 0x1234000ull}   /* STATUS_DEVICE_BUSY */
#define BC250H_GFX_REFUSE   {1, (long)0xC000009AL, 1, 1, 0x1234000ull}   /* STATUS_INSUFFICIENT_RESOURCES */
#define BC250H_GFX_NOTREADY {0, 0,            1, 1, 0x1234000ull}
#define BC250H_GFX_NOFENCE  {1, 0,            0, 1, 0x1234000ull}

static const struct bc250h_submit_case g_SubmitCases[] = {
    { "a normal packet: ring ready, IB taken, fence already there",
      4096, 0xF400100000ull, BC250H_CTXARG_ROOT, 0, BC250H_GFX_OK, 1 },

    { "the ring took it but the fence has not arrived yet",
      4096, 0xF400100000ull, BC250H_CTXARG_ROOT, 0, BC250H_GFX_NOFENCE, 1 },

    /* ADR 0008 point 5, the four ways the ring can decline. None of them may reach the caller. */
    { "GfxSubmitIb answers STATUS_DEVICE_BUSY",
      4096, 0xF400100000ull, BC250H_CTXARG_ROOT, 0, BC250H_GFX_BUSY, 1 },
    { "GfxSubmitIb answers STATUS_INSUFFICIENT_RESOURCES",
      4096, 0xF400100000ull, BC250H_CTXARG_ROOT, 0, BC250H_GFX_REFUSE, 1 },
    { "GfxSubmitReady answers FALSE (gate closed, stage 8 not done, or a previous failure)",
      4096, 0xF400100000ull, BC250H_CTXARG_ROOT, 0, BC250H_GFX_NOTREADY, 0 },
    { "DmaBufferSize 0: an empty buffer has nothing to run",
      0, 0xF400100000ull, BC250H_CTXARG_ROOT, 0, BC250H_GFX_OK, 0 },

    { "the context never saw SetRootPageTable, so it has no page directory",
      4096, 0xF400100000ull, BC250H_CTXARG_NOROOT, 0, BC250H_GFX_OK, 0 },
    { "hContext NULL, as a paging submission arrives",
      4096, 0xF400100000ull, BC250H_CTXARG_NULL, 0, BC250H_GFX_OK, 0 },

    { "size 0 and no root at once",
      0, 0ull, BC250H_CTXARG_NOROOT, 0, BC250H_GFX_OK, 0 },
    { "a large buffer at a high virtual address",
      0x100000, 0xFFFFF00000ull, BC250H_CTXARG_ROOT, 0, BC250H_GFX_OK, 1 },
};

unsigned bc250h_submit_case_count(void) { return RTL_NUMBER_OF(g_SubmitCases); }

const struct bc250h_submit_case* bc250h_submit_case(unsigned index)
{
    return (index < RTL_NUMBER_OF(g_SubmitCases)) ? &g_SubmitCases[index] : NULL;
}

/* Makes the context the case asks for, with or without a root, and hands back its handle. */
static HANDLE bc250h_submit_context(int kind)
{
    DXGKARG_CREATECONTEXT create;
    DXGKARG_SETROOTPAGETABLE root;
    NTSTATUS status;

    if (kind == BC250H_CTXARG_NULL) return NULL;

    RtlZeroMemory(&create, sizeof(create));
    create.Flags.SystemContext = 1;
    create.Flags.VirtualAddressing = 1;
    status = g_Table.DxgkDdiCreateContext(g_CtxDevice, &create);
    if (!NT_SUCCESS(status) || create.hContext == NULL) return NULL;

    if (kind == BC250H_CTXARG_ROOT && g_Table.DxgkDdiSetRootPageTable != NULL)
    {
        RtlZeroMemory(&root, sizeof(root));
        root.hContext = create.hContext;
        root.Address.SegmentId = BC250H_SEGMENT_VRAM;
        root.Address.SegmentOffset = 0x2000;
        root.NumEntries = 512;
        g_Table.DxgkDdiSetRootPageTable((HANDLE)&g_Device, &root);
    }
    return create.hContext;
}

int bc250h_submit_call(const struct bc250h_submit_case* test_case, long* status, int* reached_ring)
{
    DXGKARG_SUBMITCOMMANDVIRTUAL submit;
    struct bc250h_gfx_control saved = g_Gfx;
    unsigned before;
    HANDLE context;
    int faulted = 0;

    *reached_ring = 0;
    if (g_Table.DxgkDdiSubmitCommandVirtual == NULL) { *status = BC250H_STATUS_NOT_SUPPORTED; return 2; }

    /* The root has to be handed over while VidMmRootPhysical still answers, whatever the case then
     * makes the ring do; only after that does the case's control take effect. */
    bc250h_shim_defaults(&g_Gfx);
    context = bc250h_submit_context(test_case->context);
    g_Gfx = test_case->control;

    RtlZeroMemory(&submit, sizeof(submit));
    submit.hContext = context;
    submit.NodeOrdinal = test_case->node;
    submit.SubmissionFenceId = 0x4242;
    submit.DmaBufferVirtualAddress = (D3DGPU_VIRTUAL_ADDRESS)test_case->dma_virtual_address;
    submit.DmaBufferSize = test_case->dma_size;

    before = g_Shim.submit_ib;
    __try
    {
        *status = (long)g_Table.DxgkDdiSubmitCommandVirtual((HANDLE)&g_Device, &submit);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *status = (long)GetExceptionCode();
        faulted = 1;
    }
    *reached_ring = (g_Shim.submit_ib != before);

    /* Leave nothing in flight for the next case: let the fence arrive, then drop the context. */
    g_Gfx.fence_arrived = 1;
    WddmGpuFence(&g_Device);
    if (context != NULL && g_Table.DxgkDdiDestroyContext != NULL) (void)g_Table.DxgkDdiDestroyContext(context);
    g_Gfx = saved;
    return faulted;
}

/* ---- BuildPagingBuffer, SetRootPageTable, GetRootPageTableSize ---------------------------------- */

int bc250h_paging_call(unsigned operation, void* dma_buffer, unsigned dma_size, unsigned multipass_offset,
                       long* status, int* vidmm_called)
{
    DXGKARG_BUILDPAGINGBUFFER build;
    unsigned before = g_Shim.vidmm_update_page_table;
    int faulted = 0;

    *vidmm_called = 0;
    if (g_Table.DxgkDdiBuildPagingBuffer == NULL) { *status = BC250H_STATUS_NOT_SUPPORTED; return 2; }

    RtlZeroMemory(&build, sizeof(build));
    build.pDmaBuffer = dma_buffer;
    build.DmaSize = dma_size;
    build.MultipassOffset = multipass_offset;
    build.Operation = (DXGK_BUILDPAGINGBUFFER_OPERATION)operation;

    __try
    {
        *status = (long)g_Table.DxgkDdiBuildPagingBuffer((HANDLE)&g_Device, &build);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *status = (long)GetExceptionCode();
        faulted = 1;
    }
    *vidmm_called = (g_Shim.vidmm_update_page_table != before);
    return faulted;
}

/* SetRootPageTable, then one submit, and the root that reached the ring is the answer. Reading the
 * driver's private context structure would be the shorter way and the wrong one: what the contract is
 * about is the value gfx.c is handed, and that is exactly what this measures. */
int bc250h_set_root_call(unsigned segment_id, unsigned long long segment_offset, unsigned entries,
                         unsigned long long* recorded_root)
{
    DXGKARG_CREATECONTEXT create;
    DXGKARG_SETROOTPAGETABLE root;
    DXGKARG_SUBMITCOMMANDVIRTUAL submit;
    NTSTATUS status;
    int faulted = 0;

    *recorded_root = 0;
    if (g_Table.DxgkDdiSetRootPageTable == NULL || g_Table.DxgkDdiCreateContext == NULL) return 2;

    RtlZeroMemory(&create, sizeof(create));
    create.Flags.SystemContext = 1;
    create.Flags.VirtualAddressing = 1;
    status = g_Table.DxgkDdiCreateContext(g_CtxDevice, &create);
    if (!NT_SUCCESS(status) || create.hContext == NULL) return 3;

    RtlZeroMemory(&root, sizeof(root));
    root.hContext = create.hContext;
    root.Address.SegmentId = segment_id;
    root.Address.SegmentOffset = segment_offset;
    root.NumEntries = entries;
    __try
    {
        g_Table.DxgkDdiSetRootPageTable((HANDLE)&g_Device, &root);

        RtlZeroMemory(&submit, sizeof(submit));
        submit.hContext = create.hContext;
        submit.SubmissionFenceId = 0x4243;
        submit.DmaBufferVirtualAddress = (D3DGPU_VIRTUAL_ADDRESS)0xF400200000ull;
        submit.DmaBufferSize = 4096;
        g_Shim.last_root = 0;
        (void)g_Table.DxgkDdiSubmitCommandVirtual((HANDLE)&g_Device, &submit);
        *recorded_root = g_Shim.last_root;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        faulted = 1;
    }
    g_Gfx.fence_arrived = 1;
    WddmGpuFence(&g_Device);
    (void)g_Table.DxgkDdiDestroyContext(create.hContext);
    return faulted;
}

int bc250h_root_size_call(unsigned requested_ptes, unsigned* answered_ptes, unsigned long long* bytes)
{
    DXGKARG_GETROOTPAGETABLESIZE args;

    *answered_ptes = 0;
    *bytes = 0;
    if (g_Table.DxgkDdiGetRootPageTableSize == NULL) return 2;

    RtlZeroMemory(&args, sizeof(args));
    args.NumberOfPte = requested_ptes;
    __try
    {
        *bytes = (unsigned long long)g_Table.DxgkDdiGetRootPageTableSize((HANDLE)&g_Device, &args);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 1;
    }
    *answered_ptes = args.NumberOfPte;
    return 0;
}

/* ---- reading the answers back ------------------------------------------------------------------ */

void bc250h_caps_observed(const void* caps_buffer, struct bc250h_caps_observed* out)
{
    const DXGK_DRIVERCAPS* caps = (const DXGK_DRIVERCAPS*)caps_buffer;

    RtlZeroMemory(out, sizeof(*out));
    out->wddm_version = (unsigned)caps->WDDMVersion;
    out->scheduling_caps = caps->SchedulingCaps.Value;
    out->memory_management_caps = caps->MemoryManagementCaps.Value;
    out->flip_caps = caps->FlipCaps.Value;
    out->presentation_caps = caps->PresentationCaps.Value;
    out->support_non_vga = caps->SupportNonVGA ? 1u : 0u;
    out->support_smooth_rotation = caps->SupportSmoothRotation ? 1u : 0u;
    out->support_per_engine_tdr = caps->SupportPerEngineTDR ? 1u : 0u;
    out->support_direct_flip = caps->SupportDirectFlip ? 1u : 0u;
    out->support_surprise_removal = caps->SupportSurpriseRemoval ? 1u : 0u;
    out->nb_asymetric_processing_nodes = caps->GpuEngineTopology.NbAsymetricProcessingNodes;
    out->max_queued_flip_on_vsync = caps->MaxQueuedFlipOnVSync;
    out->max_allocation_list_slot_id = caps->MaxAllocationListSlotId;
    out->number_of_swizzling_ranges = caps->NumberOfSwizzlingRanges;
    out->interrupt_message_number = caps->InterruptMessageNumber;
    out->graphics_preemption_granularity = (unsigned)caps->PreemptionCaps.GraphicsPreemptionGranularity;
    out->compute_preemption_granularity = (unsigned)caps->PreemptionCaps.ComputePreemptionGranularity;
    out->highest_acceptable_address = (unsigned long long)caps->HighestAcceptableAddress.QuadPart;
    out->internal_gpu_va_start = caps->InternalGpuVirtualAddressRangeStart;
    out->internal_gpu_va_end = caps->InternalGpuVirtualAddressRangeEnd;
}

unsigned bc250h_segment_count(void)
{
    DXGKARG_QUERYADAPTERINFO arg;
    DXGK_QUERYSEGMENTOUT4 out;

    if (g_Table.DxgkDdiQueryAdapterInfo == NULL) return 0;
    RtlZeroMemory(&arg, sizeof(arg));
    RtlZeroMemory(&out, sizeof(out));
    arg.Type = DXGKQAITYPE_QUERYSEGMENT4;
    arg.pOutputData = &out;
    arg.OutputDataSize = sizeof(out);
    out.NbSegment = 0;                          /* the pass-1 signal */
    if (!NT_SUCCESS(g_Table.DxgkDdiQueryAdapterInfo((HANDLE)&g_Device, &arg))) return 0;
    return out.NbSegment;
}

/* stride is the SegmentDescriptorStride the array was written with, which is dxgkrnl's to choose and
 * need not be sizeof(DXGK_SEGMENTDESCRIPTOR4). Indexing with sizeof would read the gap between
 * descriptors whenever the two differ. */
unsigned bc250h_segment_flags(const void* descriptors, unsigned stride, unsigned count, unsigned id)
{
    const DXGK_SEGMENTDESCRIPTOR4* descriptor;

    if (descriptors == NULL || id == 0 || id > count) return 0;
    if (stride == 0) stride = (unsigned)sizeof(DXGK_SEGMENTDESCRIPTOR4);
    descriptor = (const DXGK_SEGMENTDESCRIPTOR4*)((const UCHAR*)descriptors + (id - 1) * stride);
    return descriptor->Flags.Value;
}

/* VIDMM_GLOBAL::VerifySegmentSet, as dxgmms2 codes it (facts M66): set 0 passes unconditionally, and
 * every bit N-1 that is set must name a declared segment N whose Flags.Aperture is 1.
 * Returns 0 when the set is legal, otherwise the id of the first segment that fails; *not_declared
 * says whether that segment is missing entirely or merely not an aperture. The wording is left to
 * qai_test.c, which has the CRT this file must not see. */
unsigned bc250h_segment_set_bad_id(const void* descriptors, unsigned stride, unsigned count, unsigned set, int* not_declared)
{
    unsigned bit;

    *not_declared = 0;
    for (bit = 0; bit < 32; bit++)
    {
        unsigned id = bit + 1;

        if (((set >> bit) & 1u) == 0) continue;
        if (id > count) { *not_declared = 1; return id; }
        if ((bc250h_segment_flags(descriptors, stride, count, id) & 1u) == 0) return id;  /* bit 0 is Aperture */
    }
    return 0;
}

void bc250h_segment_observed(const void* out_buffer, const void* descriptor,
                             struct bc250h_segment_observed* out)
{
    const DXGK_QUERYSEGMENTOUT4* segment = (const DXGK_QUERYSEGMENTOUT4*)out_buffer;
    const DXGK_SEGMENTDESCRIPTOR4* desc = (const DXGK_SEGMENTDESCRIPTOR4*)descriptor;

    RtlZeroMemory(out, sizeof(*out));
    out->nb_segment = segment->NbSegment;
    out->paging_buffer_segment_id = segment->PagingBufferSegmentId;
    out->paging_buffer_size = segment->PagingBufferSize;
    out->paging_buffer_private_data_size = segment->PagingBufferPrivateDataSize;
    if (desc == NULL || segment->NbSegment == 0) return;
    out->flags = desc->Flags.Value;
    out->base_address = (unsigned long long)desc->BaseAddress.QuadPart;
    out->cpu_translated_address = (unsigned long long)desc->CpuTranslatedAddress.QuadPart;
    out->size = (unsigned long long)desc->Size;
}
