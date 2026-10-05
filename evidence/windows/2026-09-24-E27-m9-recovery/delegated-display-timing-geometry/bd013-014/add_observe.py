from pathlib import Path
root=Path('bc250-win/driver/kmd')
p=root/'bc250kmd_escape.h';s=p.read_text();s=s.replace('#define BC250_KMD_VERSION','#define BC250_ESCAPE_OBSERVE_DCN 20u       // named, read-only scanout and timing observations\n#define BC250_KMD_VERSION',1)
anchor='// Memory commands. The first four fields'
new='''// Read-only diagnostics, not an atomic hardware snapshot. Require administrator,
// HardwareAccess=1 and every other D3DDDI_ESCAPEFLAGS bit zero: Level Two keeps
// BAR mapping alive, but idles GPU scheduling and therefore perturbs the workload.
// Sequence brackets software surface publication only; raster/flip latch can move.
// ValidMask bits follow register field order below (0..21). Timing bits 11..21
// are all set only when the entire shared timing tuple was read successfully.
#define BC250_DCN_OBSERVE_ABI 1u
#define BC250_DCN_OBSERVE_REG_COUNT 22u
#define BC250_DCN_OBSERVE_VALID_ALL ((1u << BC250_DCN_OBSERVE_REG_COUNT) - 1u)
#define BC250_DCN_OBSERVE_TIMING_MASK (BC250_DCN_OBSERVE_VALID_ALL & ~((1u << 11) - 1u))
typedef struct _BC250_ESCAPE_DCN_OBSERVE {
    unsigned long Magic, Command, Status, Version;
    unsigned long NtStatus, AbiVersion, RegisterCount, ValidMask;
    unsigned long PrimaryAddressLow, PrimaryAddressHigh;
    unsigned long EarliestInUseLow, EarliestInUseHigh;
    unsigned long FlipControl, SurfacePitch, OtgStatusPosition, OtgGlobalControl0;
    unsigned long OtgBlankControl, OtgDoubleBufferControl, OtgFrameCount;
    unsigned long TimingControl, TimingHTotal, TimingVTotal, TimingHBlank, TimingVBlank;
    unsigned long TimingPixelControl, TimingPhase, TimingModulo, TimingInterlace;
    unsigned long TimingVTotalControl, TimingReference;
    unsigned long SequenceBefore, SequenceAfter;
} BC250_ESCAPE_DCN_OBSERVE; // 128 bytes on Windows, ABI 1

'''
assert anchor in s;s=s.replace(anchor,new+anchor,1);p.write_text(s)
p=root/'bc250kmd.h';s=p.read_text();anchor='struct _BC250_ESCAPE_DCN;';assert anchor in s;s=s.replace(anchor,'''struct _BC250_ESCAPE_DCN_OBSERVE;
void DcnObserve(_Inout_ BC250_DEVICE* Device, _Inout_ struct _BC250_ESCAPE_DCN_OBSERVE* Data,
    _In_ BOOLEAN Admin, _In_ ULONG EscapeFlags);
'''+anchor,1);p.write_text(s)
p=root/'display.c';s=p.read_text();s=s.replace('    case BC250_ESCAPE_RUN_CLOCK:\n','    case BC250_ESCAPE_OBSERVE_DCN:\n    case BC250_ESCAPE_RUN_CLOCK:\n',1);anchor='    if (data->Command == BC250_ESCAPE_RUN_CLOCK) {';assert anchor in s;s=s.replace(anchor,'''    if (data->Command == BC250_ESCAPE_OBSERVE_DCN) {
        if (Escape->PrivateDriverDataSize != sizeof(BC250_ESCAPE_DCN_OBSERVE)) return STATUS_INVALID_PARAMETER;
        DcnObserve(device,(BC250_ESCAPE_DCN_OBSERVE*)data,CallerIsAdmin(),Escape->Flags.Value);
        return STATUS_SUCCESS; // typed operation status is in the reply
    }
'''+anchor,1);p.write_text(s)
