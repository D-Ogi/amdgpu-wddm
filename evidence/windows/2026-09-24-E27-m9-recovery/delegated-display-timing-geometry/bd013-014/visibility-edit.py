from pathlib import Path
r=Path('P:/bc-250/bc250-win/driver/kmd')
p=r/'dcn.c';s=p.read_text();pos=s.index('NTSTATUS DcnRestorePostDisplay(')
body='''// PROVENANCE: Linux AMD display (MIT), dcn201_tg_funcs uses optc1_set_blank
// and optc1_set_blank_data_double_buffer. Keep the inherited blank color and
// timing running. This affects output pixels, never framebuffer contents.
// Quiet, allocation/lock-free and bounded so restore can also use it at bugcheck.
NTSTATUS DcnSetVisibility(_Inout_ BC250_DEVICE* Device, BOOLEAN Visible)
{
    ULONG value, waited=0;
    NTSTATUS status;
    if (Visible && !Device->DcnBlanked) return STATUS_SUCCESS;
    status=MmioDcnRead(Device,BC250_REG_DMU_OTG0_OTG_BLANK_CONTROL,&value);
    if (!NT_SUCCESS(status)) return status;
    value &= ~(OTG0_OTG_BLANK_CONTROL__OTG_BLANK_DATA_EN_MASK |
               OTG0_OTG_BLANK_CONTROL__OTG_BLANK_DE_MODE_MASK);
    if (!Visible) value|=OTG0_OTG_BLANK_CONTROL__OTG_BLANK_DATA_EN_MASK;
    status=MmioDcnWriteEx(Device,BC250_REG_DMU_OTG0_OTG_BLANK_CONTROL,value,TRUE);
    if (!NT_SUCCESS(status)) return status;
    if (!Visible)
    {
        Device->DcnBlanked=TRUE; // undo remains required even if confirmation fails
        status=MmioDcnRead(Device,BC250_REG_DMU_OTG0_OTG_DOUBLE_BUFFER_CONTROL,&value);
        if (!NT_SUCCESS(status)) return status;
        status=MmioDcnWriteEx(Device,BC250_REG_DMU_OTG0_OTG_DOUBLE_BUFFER_CONTROL,
            value&~OTG0_OTG_DOUBLE_BUFFER_CONTROL__OTG_BLANK_DATA_DOUBLE_BUFFER_EN_MASK,TRUE);
        if (!NT_SUCCESS(status)) return status;
    }
    for (;;)
    {
        status=MmioDcnRead(Device,BC250_REG_DMU_OTG0_OTG_BLANK_CONTROL,&value);
        if (!NT_SUCCESS(status)) return status;
        if (((value&OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK)!=0)==(!Visible)) break;
        if (waited>=BC250_DCNFLIP_POLL_MAX_US) return STATUS_IO_TIMEOUT;
        KeStallExecutionProcessor(BC250_DCNFLIP_POLL_STEP_US);
        waited+=BC250_DCNFLIP_POLL_STEP_US;
    }
    Device->DcnBlanked=!Visible;
    return STATUS_SUCCESS;
}

'''
s=s[:pos]+body+s[pos:]
s=s.replace('''        return Device->DcnDiverged ? STATUS_DEVICE_NOT_READY : STATUS_SUCCESS;
''','''        return Device->DcnDiverged ? STATUS_DEVICE_NOT_READY : DcnSetVisibility(Device,TRUE);
''')
needle='''    Device->DcnCurrentAddress=Device->DcnFirmwareAddress;
    Device->DcnCurrentPitch=Device->DcnFirmwarePitch;''';replacement='''    status=DcnSetVisibility(Device,TRUE);
    if (!NT_SUCCESS(status)) return status;
'''+needle;assert s.count(needle)==1;s=s.replace(needle,replacement);p.write_text(s)
p=r/'bc250kmd.h';s=p.read_text();s=s.replace('    BOOLEAN DcnDiverged;','    BOOLEAN DcnDiverged;\n    BOOLEAN DcnBlanked;                // this driver requested blank; restore must unblank');s=s.replace('NTSTATUS DcnRestorePostDisplay(_Inout_ BC250_DEVICE* Device);', 'NTSTATUS DcnSetVisibility(_Inout_ BC250_DEVICE* Device, BOOLEAN Visible);\nNTSTATUS DcnRestorePostDisplay(_Inout_ BC250_DEVICE* Device);');p.write_text(s)
p=r/'display.c';s=p.read_text();needle='''    if (Visibility->VidPnSourceId != 0) return STATUS_INVALID_PARAMETER;
    if (!Visibility->Visible && device->SourceVisible && device->Framebuffer != NULL)
        RtlZeroMemory(device->Framebuffer, device->FramebufferLength);      // blank: there is no plane to disable in M3
''';replacement='''    NTSTATUS status;
    if (Visibility->VidPnSourceId != 0) return STATUS_INVALID_PARAMETER;
    if (device->VidPnFlipEnabled)
    {
        status=DcnSetVisibility(device,Visibility->Visible);
        if (!NT_SUCCESS(status)) return status;
    }
    else if (!Visibility->Visible && device->SourceVisible && device->Framebuffer != NULL)
        RtlZeroMemory(device->Framebuffer, device->FramebufferLength); // legacy no-MMIO display-only fallback
''';assert s.count(needle)==1;s=s.replace(needle,replacement);p.write_text(s)
# Restore fixture gains the real visibility helper to cover the newly shared call.
p=r/'test/generate_post_display_test.py';s=p.read_text().replace("'NTSTATUS DcnRestorePostDisplay('","'NTSTATUS DcnSetVisibility(', 'NTSTATUS DcnRestorePostDisplay('");p.write_text(s)
p=r/'test/post_display_test.c';s=p.read_text().replace('BOOLEAN DcnWriteEnabled,DcnFirmwareKnown,DcnDiverged,SystemDisplayReady;', 'BOOLEAN DcnWriteEnabled,DcnFirmwareKnown,DcnDiverged,SystemDisplayReady,DcnBlanked;');p.write_text(s)
