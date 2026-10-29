/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef BC250_LAB_GATE_H
#define BC250_LAB_GATE_H
// BD-167. An attestation-signed release driver must not hand user mode a raw read-anything, write-anything or
// submit-anything primitive: a Microsoft-signed driver is trusted by Windows and by anti-cheat, so such a
// primitive is a vulnerable-driver finding and a blocklist candidate. The raw memory, register, GART, PSP,
// submit/fence, SDMA, interrupt-ring and scanout-dump escapes are therefore compiled ONLY into a separate lab
// flavour: driver\kmd\build.ps1 -Lab defines AMDGPU_WDDM_LAB_FLAVOUR, and tools\release\build-release.ps1 never
// passes -Lab, so the released .sys does not contain them at all.
//
// Even in the lab flavour the raw escapes answer only when BOTH hold: the machine runs with test signing on,
// and the operator has written the REG_DWORD Services\bc250kmd\Parameters\LabDiagnostics = 1. The installer
// never writes that value. A production-signed driver on a Secure-Boot machine therefore never reaches the raw
// path even if a lab .sys were somehow installed, because CodeIntegrityOptions would not carry the test-sign bit.
//
// Release build: this symbol is not declared, and every caller sits inside #ifdef AMDGPU_WDDM_LAB_FLAVOUR, so the
// release translation units reference nothing here.
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
BOOLEAN LabDiagnosticsAllowed(void);   // PASSIVE_LEVEL; TRUE iff built -Lab AND test signing on AND LabDiagnostics=1

// One bit per raw escape family. LabEscapeAdmitted logs the first admitted use of a family since the driver
// loaded. Each dispatch arm passes its own literal: tools\quality\flavour_gate.py finds that literal in the lab
// SYS as the arm's marker, and its absence from the release SYS shows that the arm is not compiled there.
typedef enum _BC250_LAB_ESCAPE {
    LabEscapeMemory, LabEscapeRegister, LabEscapeGart, LabEscapePsp, LabEscapeGfx, LabEscapeFence, LabEscapeIh,
    LabEscapeDcn, LabEscapeDcnFlip, LabEscapeFbdump, LabEscapeSdma, LabEscapeConnectorReplug,
    LabEscapeLatchProbe, LabEscapeCount
} BC250_LAB_ESCAPE;
void LabEscapeAdmitted(BC250_LAB_ESCAPE Family, const char* Marker);   // any IRQL the escape runs at
#endif

#endif
