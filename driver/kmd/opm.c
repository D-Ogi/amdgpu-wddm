// The Output Protection Manager (OPM) interface of the full table (BD-410 stage 1). dxgkrnl gets it through
// DxgkDdiQueryInterface (query_interface.c) with GUID_DEVINTERFACE_OPM, version 1. The answers are in
// opm_policy.h, which says why they are true: a protected output can be created for the one output, and every
// call that needs the OPM certificate is refused with STATUS_GRAPHICS_OPM_NOT_SUPPORTED, because this driver has
// no certificate that Microsoft signed and no protection type is active on any output.
// Nothing here touches the hardware.
#include <ntifs.h>
#include <initguid.h>   // GUID_DEVINTERFACE_OPM of dispmprt.h is defined in this file (state_device.c does the same)
#include "bc250kmd.h"

// The calls whose first occurrence of a driver load is logged (BC250_OPM.Logged). Later calls are only counted in
// OPM_OUTPUTS; the count of each kind is in the line of the first one. At most OPM_LOG_KINDS lines per load.
enum {
    OPM_LOG_INTERFACE, OPM_LOG_CREATE, OPM_LOG_CREATE_REFUSED, OPM_LOG_DESTROY_INVALID, OPM_LOG_CERTIFICATE_SIZE,
    OPM_LOG_CERTIFICATE, OPM_LOG_RANDOM_NUMBER, OPM_LOG_SIGNING_KEY, OPM_LOG_INFORMATION, OPM_LOG_COPP_INFORMATION,
    OPM_LOG_CONFIGURE, OPM_LOG_KINDS
};
static const char* const OpmLogNames[OPM_LOG_KINDS] = {
    "interface", "create", "create-refused", "destroy-invalid", "certificate-size", "certificate", "random-number",
    "signing-key", "information", "copp-information", "configure",
};
C_ASSERT(OPM_LOG_KINDS <= 32);

void OpmInitialize(BC250_DEVICE* Device)
{
    ExInitializeFastMutex(&Device->Opm.Lock);
}

// TRUE once per kind and load; called with Opm.Lock held.
static BOOLEAN OpmFirst(BC250_DEVICE* Device, ULONG Kind)
{
    if (Device->Opm.Logged & (1u << Kind)) return FALSE;
    Device->Opm.Logged |= 1u << Kind;
    return TRUE;
}

static void OpmLogLine(ULONG Kind, LONG Status, const OPM_OUTPUTS* Counts)
{
    GuardLog("opm: first %s -> 0x%08lX (created %u destroyed %u refused %u full %u bad-handle %u)",
        OpmLogNames[Kind], (ULONG)Status, Counts->Created, Counts->Destroyed, Counts->Refused, Counts->Full,
        Counts->BadHandle);
}

// One refused call: the policy's answer, counted, and logged the first time.
static NTSTATUS OpmRefused(PVOID MiniportDeviceContext, ULONG Kind)
{
    BC250_DEVICE* device = (BC250_DEVICE*)MiniportDeviceContext;
    OPM_OUTPUTS counts;
    BOOLEAN first;
    NTSTATUS status;

    ExAcquireFastMutex(&device->Opm.Lock);
    status = (NTSTATUS)OpmRefuse(&device->Opm.Outputs);
    first = OpmFirst(device, Kind);
    counts = device->Opm.Outputs;
    ExReleaseFastMutex(&device->Opm.Lock);
    if (first) OpmLogLine(Kind, status, &counts);
    return status;
}

// The interface lives as long as the device and holds nothing, so the reference calls have nothing to count.
static VOID OpmInterfaceReference(PVOID Context) { UNREFERENCED_PARAMETER(Context); }
static VOID OpmInterfaceDereference(PVOID Context) { UNREFERENCED_PARAMETER(Context); }

static NTSTATUS OpmGetCertificateSize(PVOID MiniportDeviceContext, DXGKMDT_CERTIFICATE_TYPE CertificateType,
                                      PULONG CertificateSize)
{
    UNREFERENCED_PARAMETER(CertificateType);
    UNREFERENCED_PARAMETER(CertificateSize);
    return OpmRefused(MiniportDeviceContext, OPM_LOG_CERTIFICATE_SIZE);
}

static NTSTATUS OpmGetCertificate(PVOID MiniportDeviceContext, DXGKMDT_CERTIFICATE_TYPE CertificateType,
                                  ULONG CertificateSize, PVOID CertificateBuffer)
{
    UNREFERENCED_PARAMETER(CertificateType);
    UNREFERENCED_PARAMETER(CertificateSize);
    UNREFERENCED_PARAMETER(CertificateBuffer);
    return OpmRefused(MiniportDeviceContext, OPM_LOG_CERTIFICATE);
}

static NTSTATUS OpmCreateProtectedOutput(PVOID MiniportDeviceContext, D3DDDI_VIDEO_PRESENT_TARGET_ID VidPnTargetId,
                                         DXGKMDT_OPM_VIDEO_OUTPUT_SEMANTICS NewVideoOutputSemantics,
                                         PHANDLE NewProtectedOutputHandle)
{
    BC250_DEVICE* device = (BC250_DEVICE*)MiniportDeviceContext;
    unsigned long long handle = 0;
    OPM_OUTPUTS counts;
    BOOLEAN first;
    NTSTATUS status;
    ULONG kind;

    ExAcquireFastMutex(&device->Opm.Lock);
    status = (NTSTATUS)OpmCreate(&device->Opm.Outputs, (unsigned)VidPnTargetId, BC250_CHILD_UID,
                                 (unsigned)NewVideoOutputSemantics, &handle);
    kind = NT_SUCCESS(status) ? OPM_LOG_CREATE : OPM_LOG_CREATE_REFUSED;
    first = OpmFirst(device, kind);
    counts = device->Opm.Outputs;
    ExReleaseFastMutex(&device->Opm.Lock);
    if (NT_SUCCESS(status)) *NewProtectedOutputHandle = (HANDLE)(ULONG_PTR)handle;
    if (first) {
        GuardLog("opm: target 0x%lX semantics %lu", (ULONG)VidPnTargetId, (ULONG)NewVideoOutputSemantics);
        OpmLogLine(kind, status, &counts);
    }
    return status;
}

static NTSTATUS OpmGetRandomNumber(PVOID MiniportDeviceContext, HANDLE ProtectedOutputHandle,
                                   PDXGKMDT_OPM_RANDOM_NUMBER RandomNumber)
{
    UNREFERENCED_PARAMETER(ProtectedOutputHandle);
    UNREFERENCED_PARAMETER(RandomNumber);
    return OpmRefused(MiniportDeviceContext, OPM_LOG_RANDOM_NUMBER);
}

static NTSTATUS OpmSetSigningKeyAndSequenceNumbers(PVOID MiniportDeviceContext, HANDLE ProtectedOutputHandle,
                                                   CONST DXGKMDT_OPM_ENCRYPTED_PARAMETERS* EncryptedParameters)
{
    UNREFERENCED_PARAMETER(ProtectedOutputHandle);
    UNREFERENCED_PARAMETER(EncryptedParameters);
    return OpmRefused(MiniportDeviceContext, OPM_LOG_SIGNING_KEY);
}

static NTSTATUS OpmGetInformation(PVOID MiniportDeviceContext, HANDLE ProtectedOutputHandle,
                                  CONST DXGKMDT_OPM_GET_INFO_PARAMETERS* Parameters,
                                  PDXGKMDT_OPM_REQUESTED_INFORMATION RequestedInformation)
{
    UNREFERENCED_PARAMETER(ProtectedOutputHandle);
    UNREFERENCED_PARAMETER(Parameters);
    UNREFERENCED_PARAMETER(RequestedInformation);
    return OpmRefused(MiniportDeviceContext, OPM_LOG_INFORMATION);
}

static NTSTATUS OpmGetCoppCompatibleInformation(PVOID MiniportDeviceContext, HANDLE ProtectedOutputHandle,
                                                CONST DXGKMDT_OPM_COPP_COMPATIBLE_GET_INFO_PARAMETERS* Parameters,
                                                PDXGKMDT_OPM_REQUESTED_INFORMATION RequestedInformation)
{
    UNREFERENCED_PARAMETER(ProtectedOutputHandle);
    UNREFERENCED_PARAMETER(Parameters);
    UNREFERENCED_PARAMETER(RequestedInformation);
    return OpmRefused(MiniportDeviceContext, OPM_LOG_COPP_INFORMATION);
}

static NTSTATUS OpmConfigureProtectedOutput(PVOID MiniportDeviceContext, HANDLE ProtectedOutputHandle,
                                            CONST DXGKMDT_OPM_CONFIGURE_PARAMETERS* Parameters,
                                            ULONG AdditionalParametersSize, CONST VOID* AdditionalParameters)
{
    UNREFERENCED_PARAMETER(ProtectedOutputHandle);
    UNREFERENCED_PARAMETER(Parameters);
    UNREFERENCED_PARAMETER(AdditionalParametersSize);
    UNREFERENCED_PARAMETER(AdditionalParameters);
    return OpmRefused(MiniportDeviceContext, OPM_LOG_CONFIGURE);
}

static NTSTATUS OpmDestroyProtectedOutput(PVOID MiniportDeviceContext, HANDLE ProtectedOutputHandle)
{
    BC250_DEVICE* device = (BC250_DEVICE*)MiniportDeviceContext;
    OPM_OUTPUTS counts;
    BOOLEAN first = FALSE;
    NTSTATUS status;

    ExAcquireFastMutex(&device->Opm.Lock);
    status = (NTSTATUS)OpmDestroy(&device->Opm.Outputs, (unsigned long long)(ULONG_PTR)ProtectedOutputHandle);
    if (!NT_SUCCESS(status)) first = OpmFirst(device, OPM_LOG_DESTROY_INVALID);
    counts = device->Opm.Outputs;
    ExReleaseFastMutex(&device->Opm.Lock);
    if (first) OpmLogLine(OPM_LOG_DESTROY_INVALID, status, &counts);
    return status;
}

NTSTATUS OpmQueryInterface(BC250_DEVICE* Device, PQUERY_INTERFACE QueryInterface)
{
    DXGK_OPM_INTERFACE* opm;
    OPM_OUTPUTS counts;
    BOOLEAN first;
    NTSTATUS status = (NTSTATUS)OpmQueryInterfaceAnswer(QueryInterface->InterfaceType, &GUID_DEVINTERFACE_OPM,
        QueryInterface->Size, QueryInterface->Version, QueryInterface->Interface, sizeof(DXGK_OPM_INTERFACE),
        DXGK_OPM_INTERFACE_VERSION_1);

    if (!NT_SUCCESS(status)) return status;
    opm = (DXGK_OPM_INTERFACE*)QueryInterface->Interface;
    RtlZeroMemory(opm, sizeof(*opm));
    opm->Size = sizeof(*opm);
    opm->Version = DXGK_OPM_INTERFACE_VERSION_1;
    opm->Context = Device;
    opm->InterfaceReference = OpmInterfaceReference;
    opm->InterfaceDereference = OpmInterfaceDereference;
    opm->DxgkDdiOPMGetCertificateSize = OpmGetCertificateSize;
    opm->DxgkDdiOPMGetCertificate = OpmGetCertificate;
    opm->DxgkDdiOPMCreateProtectedOutput = OpmCreateProtectedOutput;
    opm->DxgkDdiOPMGetRandomNumber = OpmGetRandomNumber;
    opm->DxgkDdiOPMSetSigningKeyAndSequenceNumbers = OpmSetSigningKeyAndSequenceNumbers;
    opm->DxgkDdiOPMGetInformation = OpmGetInformation;
    opm->DxgkDdiOPMGetCOPPCompatibleInformation = OpmGetCoppCompatibleInformation;
    opm->DxgkDdiOPMConfigureProtectedOutput = OpmConfigureProtectedOutput;
    opm->DxgkDdiOPMDestroyProtectedOutput = OpmDestroyProtectedOutput;
    // The INTERFACE convention: the provider takes the reference that the caller later gives back.
    opm->InterfaceReference(opm->Context);

    ExAcquireFastMutex(&Device->Opm.Lock);
    first = OpmFirst(Device, OPM_LOG_INTERFACE);
    counts = Device->Opm.Outputs;
    ExReleaseFastMutex(&Device->Opm.Lock);
    if (first) OpmLogLine(OPM_LOG_INTERFACE, status, &counts);
    return status;
}
