// The Output Protection Manager (OPM) answers of the miniport (BD-410 stage 1). Header only, with no WDK type, so
// that the host test (test/opm_policy_test.c) and the kernel (opm.c) run the same text; run_opm_policy.ps1 also
// compiles a probe that binds every constant below to the WDK's own name.
//
// The contract (documented, ref/windows-driver-docs and the WDK 10.0.26100 headers):
//   - dxgkrnl asks DxgkDdiQueryInterface for GUID_DEVINTERFACE_OPM, version DXGK_OPM_INTERFACE_VERSION_1, size
//     sizeof(DXGK_OPM_INTERFACE) (retrieving-the-opm-ddi.md:13-66). A miniport that does not support an interface
//     must return STATUS_NOT_SUPPORTED (nc-dispmprt-dxgkddi_query_interface.md:59).
//   - The sequence of a protected output is Create, then GetCertificateSize and GetCertificate, then the random
//     number and the signing key, then the information and configuration calls (using-the-opm-ddi.md:21-56).
//   - STATUS_GRAPHICS_OPM_NOT_SUPPORTED means that "the hardware vendor never signed the OPM license agreement or
//     the miniport driver's graphics hardware does not comply with OPM rules"
//     (nc-dispmprt-dxgkddi_opm_get_certificate_size.md:70, ..._create_protected_output.md:82).
//
// What this driver says with it: the interface is there, a protected output can be created for the one output,
// and every call that needs the OPM certificate is refused with STATUS_GRAPHICS_OPM_NOT_SUPPORTED. That is true:
// this driver has no certificate that Microsoft signed, and no protection type is active on any output. The
// driver claims no protection at all. It never answers success for a certificate, a random number, a signing key,
// an information request or a configuration.
#pragma once

#define OPM_STATUS_SUCCESS             ((long)0x00000000L)  // STATUS_SUCCESS
#define OPM_STATUS_NOT_SUPPORTED       ((long)0xC00000BBL)  // STATUS_NOT_SUPPORTED
#define OPM_STATUS_NO_MEMORY           ((long)0xC0000017L)  // STATUS_NO_MEMORY
#define OPM_STATUS_INVALID_TARGET      ((long)0xC01E0305L)  // STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET
#define OPM_STATUS_OPM_NOT_SUPPORTED   ((long)0xC01E0500L)  // STATUS_GRAPHICS_OPM_NOT_SUPPORTED
#define OPM_STATUS_OPM_INVALID_HANDLE  ((long)0xC01E050CL)  // STATUS_GRAPHICS_OPM_INVALID_HANDLE

#define OPM_SEMANTICS_COPP 0u  // DXGKMDT_OPM_VOS_COPP_SEMANTICS
#define OPM_SEMANTICS_OPM  1u  // DXGKMDT_OPM_VOS_OPM_SEMANTICS

// The protected outputs that can be open at the same time. One per IOPMVideoOutput object of a process; 16 is
// above what one player and the HLK test hold together. When all are in use, Create answers STATUS_NO_MEMORY, the
// documented code for "cannot allocate memory required for it to complete".
#define OPM_OUTPUT_SLOTS 16u

typedef struct _OPM_OUTPUTS {
    unsigned char Live[OPM_OUTPUT_SLOTS];
    unsigned Generation[OPM_OUTPUT_SLOTS];  // advances at each destroy, so that an old handle never matches again
    unsigned Created, Destroyed, Refused, Full, BadHandle;
} OPM_OUTPUTS;

// 16 bytes of two GUIDs. A byte loop, so that the header needs neither a CRT nor a kernel routine.
static __inline int OpmGuidEqual(const void* A, const void* B)
{
    const unsigned char* a = (const unsigned char*)A;
    const unsigned char* b = (const unsigned char*)B;
    unsigned i;
    for (i = 0; i < 16u; ++i) if (a[i] != b[i]) return 0;
    return 1;
}

// The answer of DxgkDdiQueryInterface for one request: OPM_STATUS_SUCCESS only for GUID_DEVINTERFACE_OPM with
// version 1, a buffer that holds the version 1 structure and an Interface pointer; STATUS_NOT_SUPPORTED for every
// other request. OpmGuid, V1Size and V1Version are the WDK's GUID_DEVINTERFACE_OPM, sizeof(DXGK_OPM_INTERFACE) and
// DXGK_OPM_INTERFACE_VERSION_1, passed in by the caller.
// The interfaces GUID_DEVINTERFACE_OPM_2, _OPM_2_JTP and _OPM_3 have GUIDs of their own and are refused: their
// additions (the non-local display output and the SRM list) are not needed for an output that refuses every
// certificate call.
static __inline long OpmQueryInterfaceAnswer(const void* InterfaceType, const void* OpmGuid, unsigned Size,
                                             unsigned Version, const void* Interface, unsigned V1Size,
                                             unsigned V1Version)
{
    if (InterfaceType == 0 || !OpmGuidEqual(InterfaceType, OpmGuid)) return OPM_STATUS_NOT_SUPPORTED;
    if (Version != V1Version || Size < V1Size || Interface == 0) return OPM_STATUS_NOT_SUPPORTED;
    return OPM_STATUS_SUCCESS;
}

// The handle of slot S: the slot number plus 1 in the low byte (never 0) and 24 bits of the slot's generation.
static __inline unsigned long long OpmHandleOf(const OPM_OUTPUTS* Outputs, unsigned Slot)
{
    return ((unsigned long long)(Outputs->Generation[Slot] & 0xFFFFFFu) << 8) | (unsigned long long)(Slot + 1u);
}

// The slot of a live handle, or OPM_OUTPUT_SLOTS for a handle that this table did not give or that is destroyed.
static __inline unsigned OpmSlotOf(const OPM_OUTPUTS* Outputs, unsigned long long Handle)
{
    unsigned slot;
    if ((Handle & 0xFFull) == 0 || (Handle >> 32) != 0) return OPM_OUTPUT_SLOTS;
    slot = (unsigned)(Handle & 0xFFull) - 1u;
    if (slot >= OPM_OUTPUT_SLOTS || !Outputs->Live[slot] || OpmHandleOf(Outputs, slot) != Handle)
        return OPM_OUTPUT_SLOTS;
    return slot;
}

// DxgkDdiOPMCreateProtectedOutput. TargetId must be the output's VidPN target (OutputTarget). The driver has one
// target with one physical connector, so the spanning and theater statuses of the DDI do not apply. COPP and OPM
// semantics are both accepted: neither leads to a certificate. Other semantics (the indirect display one) are
// refused with STATUS_GRAPHICS_OPM_NOT_SUPPORTED. *Handle changes only on success, as the DDI page says.
static __inline long OpmCreate(OPM_OUTPUTS* Outputs, unsigned TargetId, unsigned OutputTarget, unsigned Semantics,
                               unsigned long long* Handle)
{
    unsigned slot;
    if (TargetId != OutputTarget) return OPM_STATUS_INVALID_TARGET;
    if (Semantics != OPM_SEMANTICS_COPP && Semantics != OPM_SEMANTICS_OPM) return OPM_STATUS_OPM_NOT_SUPPORTED;
    for (slot = 0; slot < OPM_OUTPUT_SLOTS; ++slot) {
        if (Outputs->Live[slot]) continue;
        Outputs->Live[slot] = 1;
        Outputs->Created++;
        *Handle = OpmHandleOf(Outputs, slot);
        return OPM_STATUS_SUCCESS;
    }
    Outputs->Full++;
    return OPM_STATUS_NO_MEMORY;
}

// DxgkDdiOPMDestroyProtectedOutput. A handle that is not live (never given, or destroyed already) changes nothing
// and is answered with STATUS_GRAPHICS_OPM_INVALID_HANDLE.
static __inline long OpmDestroy(OPM_OUTPUTS* Outputs, unsigned long long Handle)
{
    unsigned slot = OpmSlotOf(Outputs, Handle);
    if (slot == OPM_OUTPUT_SLOTS) {
        Outputs->BadHandle++;
        return OPM_STATUS_OPM_INVALID_HANDLE;
    }
    Outputs->Live[slot] = 0;
    Outputs->Generation[slot]++;
    Outputs->Destroyed++;
    return OPM_STATUS_SUCCESS;
}

// Every other OPM function: GetCertificateSize, GetCertificate, GetRandomNumber, SetSigningKeyAndSequenceNumbers,
// GetInformation, GetCOPPCompatibleInformation and ConfigureProtectedOutput. No OPM certificate, so no call that
// needs one can succeed, whatever its handle.
static __inline long OpmRefuse(OPM_OUTPUTS* Outputs)
{
    Outputs->Refused++;
    return OPM_STATUS_OPM_NOT_SUPPORTED;
}
