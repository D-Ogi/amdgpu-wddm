from pathlib import Path
r=Path('P:/bc-250/bc250-win/driver/kmd')
p=r/'test/post_display_test.c';s=p.read_text();s=s.replace('    ULONG pitch,flip,surface,high;','    ULONG pitch,flip,surface,high,blank,dbuf;');s=s.replace('''    switch(reg){
    case BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK:*out''','''    switch(reg){
    case BC250_REG_DMU_OTG0_OTG_BLANK_CONTROL:*out=model.blank;break;
    case BC250_REG_DMU_OTG0_OTG_DOUBLE_BUFFER_CONTROL:*out=model.dbuf;break;
    case BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK:*out''')
s=s.replace('''    switch(reg){
    case BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK:model.locked''','''    switch(reg){
    case BC250_REG_DMU_OTG0_OTG_BLANK_CONTROL:
        model.blank=value&~OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK;
        if(value&OTG0_OTG_BLANK_CONTROL__OTG_BLANK_DATA_EN_MASK)model.blank|=OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK;
        break;
    case BC250_REG_DMU_OTG0_OTG_DOUBLE_BUFFER_CONTROL:model.dbuf=value;break;
    case BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK:model.locked''')
needle='''    // Untouched basic-display path needs no MMIO or programming.
''';replacement='''    // A hidden source must become visible for bugcheck and POST handover.
    init(&d,fb);d.DcnBlanked=TRUE;
    model.blank=OTG0_OTG_BLANK_CONTROL__OTG_BLANK_DATA_EN_MASK|OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK;
    CHECK(Bc250SystemDisplayEnable(&d,BC250_CHILD_UID,NULL,&width,&height,&fmt)==STATUS_SUCCESS);
    CHECK(d.SystemDisplayReady && !d.DcnBlanked && !(model.blank&OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK));
    CHECK(model.scanned==d.DcnFirmwareAddress);
    // Visibility can change before the first flip; that path must also unblank.
    init(&d,fb);d.DcnFirmwareKnown=FALSE;d.DcnDiverged=FALSE;d.DcnBlanked=TRUE;
    model.blank=OTG0_OTG_BLANK_CONTROL__OTG_BLANK_DATA_EN_MASK|OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK;
    CHECK(Bc250SystemDisplayEnable(&d,BC250_CHILD_UID,NULL,&width,&height,&fmt)==STATUS_SUCCESS);
    CHECK(d.SystemDisplayReady && !d.DcnBlanked && model.writes==1);
'''+needle;assert needle in s;s=s.replace(needle,replacement);p.write_text(s)
# Snapshot isolated inputs for negative controls.
out=Path('P:/bc-250/scratch/m9/bd013-014')
for mutant in ['unsynchronized','other-event-ack','clear-primary','hide-stops-vsync','no-restore-unblank']:
    d=out/mutant;d.mkdir(exist_ok=True)
    for f in ['dcn.c','display.c','wddm.c']:(d/f).write_bytes((r/f).read_bytes())
p=out/'unsynchronized/dcn.c';s=p.read_text();needle='status=Device->Dxgk.DxgkCbSynchronizeExecution(Device->Dxgk.DeviceHandle,DcnVsyncEnableSynchronized,&change,0,&returned);';assert needle in s;s=s.replace(needle,'returned=DcnVsyncEnableSynchronized(&change);status=STATUS_SUCCESS; /* negative: bypass IRQ lock */');p.write_text(s)
p=out/'other-event-ack/dcn.c';s=p.read_text();start=s.index('    Value &= ~(');end=s.index('    return Value |',start);s=s[:start]+'    /* negative: retain unrelated W1C bits */\n'+s[end:];p.write_text(s)
p=out/'clear-primary/display.c';s=p.read_text().replace('status=DcnSetVisibility(device,Visibility->Visible);','RtlZeroMemory(device->Framebuffer,device->FramebufferLength);status=STATUS_SUCCESS;');p.write_text(s)
p=out/'hide-stops-vsync/wddm.c';s=p.read_text().replace('    if (Visible) WddmVSyncArm(Device, TRUE);','    WddmVSyncArm(Device, Visible);');p.write_text(s)
p=out/'no-restore-unblank/dcn.c';s=p.read_text().replace('status=DcnSetVisibility(Device,TRUE);','status=STATUS_SUCCESS; /* negative: leave hidden */');p.write_text(s)
