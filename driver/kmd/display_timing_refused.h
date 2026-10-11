// GitHub #38 (BD-122): a refused inherited timing names its registers in the ring and in the start record, so a
// tester's Code 43 shows which rule of DisplayDecodeTiming refused (OTG0 not enabled, pixel clock not on DP_DTO0,
// active size not the POST size, zero clock) and whether the firmware drove OTG1 instead. Read only: the reads
// DisplayTimingSnapshot already makes, plus OTG1_OTG_CONTROL from the same DCN read table.
#pragma once
#include "display_timing_snapshot.h"

static void DisplayLogRefusedTiming(_In_ const BC250_DEVICE* Device, NTSTATUS Refusal)
{
    ULONG v[TimingCount], otg1 = 0xFFFFFFFFu;
    NTSTATUS read = STATUS_DEVICE_NOT_READY;

    if (Device->Mmio != NULL && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        read = DisplayTimingSnapshot(Device, v);
        if (NT_SUCCESS(read) && !NT_SUCCESS(MmioDcnRead(Device, BC250_REG_DMU_OTG1_OTG_CONTROL, &otg1)))
            otg1 = 0xFFFFFFFFu;
    }
    if (!NT_SUCCESS(read)) {
        GuardLog("display: inherited timing refused 0x%08X, post %ux%u, registers not read 0x%08X", Refusal,
                 Device->Post.Width, Device->Post.Height, read);
        GuardStartDetail(L"timing 0x%08X post %ux%u registers not read 0x%08X", Refusal, Device->Post.Width,
                         Device->Post.Height, read);
        return;
    }
    GuardLog("display: inherited timing refused 0x%08X, post %ux%u pitch %u", Refusal, Device->Post.Width,
             Device->Post.Height, Device->Post.Pitch);
    GuardLog("display: otg0 control %08X htotal %08X vtotal %08X hblank %08X vblank %08X il %08X vtc %08X",
             v[TimingControl], v[TimingHTotal], v[TimingVTotal], v[TimingHBlank], v[TimingVBlank], v[TimingInterlace],
             v[TimingVTotalControl]);
    GuardLog("display: pixel rate control %08X dto phase %08X modulo %08X reference %08X, otg1 control %08X",
             v[TimingPixelControl], v[TimingPhase], v[TimingModulo], v[TimingReference], otg1);
    GuardStartDetail(L"timing 0x%08X post %ux%u pitch %u otg0 ctl %08X ht %08X vt %08X hb %08X vb %08X il %08X "
                     L"vtc %08X prc %08X dto %08X/%08X ref %08X otg1 ctl %08X",
                     Refusal, Device->Post.Width, Device->Post.Height, Device->Post.Pitch, v[TimingControl],
                     v[TimingHTotal], v[TimingVTotal], v[TimingHBlank], v[TimingVBlank], v[TimingInterlace],
                     v[TimingVTotalControl], v[TimingPixelControl], v[TimingPhase], v[TimingModulo],
                     v[TimingReference], otg1);
}
