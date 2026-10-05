from pathlib import Path
r=Path('P:/bc-250/bc250-win/driver/kmd')
p=r/'dcn.c';s=p.read_text();start=s.index('// The hardware vsync source\'s own enable/ack:');end=s.index('// pnp.c\'s Bc250InterruptRoutine',start)
s=s[:start]+'''// GLOBAL_SYNC_STATUS mixes enable fields and W1C commands. A readback must
// never acknowledge another event as a side effect of the no-lock-vupdate ACK.
static ULONG DcnVsyncAckValue(ULONG Value)
{
    Value &= ~(OTG0_OTG_GLOBAL_SYNC_STATUS__VSTARTUP_EVENT_CLEAR_MASK |
               OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_EVENT_CLEAR_MASK |
               OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_EVENT_CLEAR_MASK |
               OTG0_OTG_GLOBAL_SYNC_STATUS__VREADY_EVENT_CLEAR_MASK);
    return Value | OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_EVENT_CLEAR_MASK;
}

typedef struct _BC250_DCN_VSYNC_CHANGE {
    BC250_DEVICE* Device;
    BOOLEAN On;
    NTSTATUS Status;
} BC250_DCN_VSYNC_CHANGE;

// Runs under the same interrupt lock as DcnVsyncInterrupt. Never acquire the
// WDDM lock here: callers may already hold it while preserving arm/stop order.
static BOOLEAN DcnVsyncEnableSynchronized(_In_ PVOID Context)
{
    BC250_DCN_VSYNC_CHANGE* change=(BC250_DCN_VSYNC_CHANGE*)Context;
    ULONG value;
    change->Status=MmioDcnRead(change->Device,BC250_REG_DMU_OTG0_OTG_GLOBAL_SYNC_STATUS,&value);
    if (!NT_SUCCESS(change->Status)) return FALSE;
    if (change->On) value|=OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_INT_EN_MASK;
    else value&=~OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_INT_EN_MASK;
    change->Status=MmioDcnWriteEx(change->Device,BC250_REG_DMU_OTG0_OTG_GLOBAL_SYNC_STATUS,
                                DcnVsyncAckValue(value),TRUE);
    if (NT_SUCCESS(change->Status))
        InterlockedExchange(&change->Device->DcnVsyncArmed,change->On?1:0);
    return NT_SUCCESS(change->Status);
}

NTSTATUS DcnVsyncEnable(_Inout_ BC250_DEVICE* Device, BOOLEAN On)
{
    BC250_DCN_VSYNC_CHANGE change;
    BOOLEAN returned=FALSE;
    NTSTATUS status;
    if (Device->Mmio==NULL || Device->Dxgk.DxgkCbSynchronizeExecution==NULL) return STATUS_DEVICE_NOT_READY;
    change.Device=Device;change.On=On;change.Status=STATUS_DEVICE_NOT_READY;
    // The miniport has one interrupt message (M38). The graphics callback is
    // available through DISPATCH_LEVEL and owns the actual KINTERRUPT object.
    status=Device->Dxgk.DxgkCbSynchronizeExecution(Device->Dxgk.DeviceHandle,DcnVsyncEnableSynchronized,&change,0,&returned);
    if (!NT_SUCCESS(status)) return status;
    return returned?change.Status:(NT_SUCCESS(change.Status)?STATUS_UNSUCCESSFUL:change.Status);
}

'''+s[end:]
s=s.replace('status | OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_EVENT_CLEAR_MASK, TRUE)))','DcnVsyncAckValue(status), TRUE)))')
s=s.replace('''// makes both legal here - review 16's checklist item for this step. Device->DcnVsyncArmed is read racily, the
// same acceptable race as ih->Active's own (ih.c): at most one interrupt either armed a tick early or found the
// gate already closing.''','''// makes both legal here. The enable callback changes DcnVsyncArmed and the RMW
// under this interrupt's synchronization, so it cannot lose a disable to this ACK.''')
s=s.replace('''    // Ack: OR the clear bit into the value just read, so INT_EN (which must stay set) and every other field go
    // back exactly as read - the same read-modify-write DcnVsyncEnable uses to turn the source on or off.''','''    // Acknowledge only our event, preserving unrelated enable fields. The
    // synchronized enable callback cannot interleave this read-modify-write.''');p.write_text(s)
p=r/'bc250kmd.h';s=p.read_text().replace('NTSTATUS DcnVsyncEnable(_In_ const BC250_DEVICE* Device, BOOLEAN On);','NTSTATUS DcnVsyncEnable(_Inout_ BC250_DEVICE* Device, BOOLEAN On);');p.write_text(s)
p=r/'wddm.c';s=p.read_text();s=s.replace('''        if (On != (Device->DcnVsyncArmed != 0))
        {
            InterlockedExchange(&Device->DcnVsyncArmed, On ? 1 : 0);
            (void)DcnVsyncEnable(Device, On);
            changed = TRUE;
        }''','''        if (On != (Device->DcnVsyncArmed != 0))
            changed = NT_SUCCESS(DcnVsyncEnable(Device, On));''')
s=s.replace('''if (Device->DcnVsyncArmed != 0) { InterlockedExchange(&Device->DcnVsyncArmed, 0); (void)DcnVsyncEnable(Device, FALSE); }''','''if (Device->DcnVsyncArmed != 0) (void)DcnVsyncEnable(Device, FALSE);''')
s=s.replace('''    WddmVSyncArm(Device, Visible);
}''','''    // Visibility controls pixels, not timing. Keep generating requested vsyncs
    // while hidden (DXGKARG_SETVIDPNSOURCEVISIBILITY); stop disarms at teardown.
    if (Visible) WddmVSyncArm(Device, TRUE);
}''')
s=s.replace('''// argument holds for DcnVsyncEnable's register write, which is why it is inside the lock too: MmioDcnRead/
// MmioDcnWrite (mmio.c) take no lock of their own and are legal at DISPATCH_LEVEL, so nesting them under this
// one costs nothing and closes the same race the timer comment describes.''','''// WDDM lock orders decisions against stop; DcnVsyncEnable then uses the graphics
// interrupt-synchronization callback to serialize MMIO and armed state with ISR.
// The synchronized callback never takes this WDDM lock (one-way lock order).''')
p.write_text(s)
