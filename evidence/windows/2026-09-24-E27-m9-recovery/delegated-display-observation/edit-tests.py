from pathlib import Path
p=Path('P:/bc-250/bc250-win/driver/kmd/test/vidpn_flip_test.c');s=p.read_text()
s=s.replace('static LONGLONG programmed,reported;', 'static LONGLONG programmed,scanned,reported;')
s=s.replace('CHECK(reports==before);','CHECK(reports==before+1 && reported==scanned);')
s=s.replace('static BOOLEAN DcnFlipPending(BC250_DEVICE *d)', 'static BOOLEAN DcnFlipPending(BC250_DEVICE *d, ULONGLONG expected)')
s=s.replace('    return pending;\n}', '    return pending || scanned != (LONGLONG)expected;\n}\nstatic NTSTATUS DcnReadScanoutAddress(BC250_DEVICE *d, ULONGLONG *out)\n{ (void)d; *out=(ULONGLONG)scanned; return STATUS_SUCCESS; }')
start=s.index('int main(void)');s=s[:start]+'''int main(void)
{
    int level;
    for(level=0;level<2;level++){
        BC250_WDDM w={0};BC250_DEVICE d={0};
        DXGKARG_SETVIDPNSOURCEADDRESS request={0};
        PHYSICAL_ADDRESS sample={-1};
        unsigned before;
        d.Post.Width=1366;d.Post.Height=768;d.Post.Pitch=5632;w.PrimaryPitch=5632;
        d.Wddm=&w;d.VidPnFlipEnabled=1;w.VSyncEnabled=1;
        w.PrimaryAddress.QuadPart=10;w.PrimarySegment=1;
        request.PrimaryAddress.QuadPart=20;request.PrimarySegment=2;
        irq=level?3:0;hardware=arms=reports=0;pending=inject_update=nested_writer=0;
        hardware_result=STATUS_IO_TIMEOUT;programmed=scanned=10;
        CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_IO_TIMEOUT);
        CHECK(w.PrimaryAddress.QuadPart==10 && w.PrimarySegment==1 && w.Flips==0);
        CHECK(programmed==10 && hardware==1 && arms==0 && !(w.PrimarySequence&1));
        before=reports;d.DcnVsyncAcked=1;WddmDcnVsync(&d);
        CHECK(reports==before+1 && reported==10);
        hardware_result=STATUS_SUCCESS;nested_writer=1;
        CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_SUCCESS);
        CHECK(programmed==20 && w.PrimaryAddress.QuadPart==20 && w.PrimarySegment==2);
        CHECK(w.Flips==1 && hardware==2 && !(w.PrimarySequence&1));
        CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_SUCCESS);
        CHECK(hardware==2 && w.Flips==1);
        // Every vblank reports; neither pending nor cleared-before-inuse retires 20.
        before=reports;pending=1;d.DcnVsyncAcked=1;WddmDcnVsync(&d);
        CHECK(reports==before+1 && reported==10);
        before=reports;pending=0;d.DcnVsyncAcked=1;WddmDcnVsync(&d);
        CHECK(reports==before+1 && reported==10);
        before=reports;scanned=20;d.DcnVsyncAcked=1;WddmDcnVsync(&d);
        CHECK(reports==before+1 && reported==20);
        // A whole new transaction during the observation invalidates completion.
        inject_update=1;
        CHECK(!WddmReadCompletedPrimary(&d,&w,&sample));
        CHECK(sample.QuadPart==-1);
        CHECK(!WddmReadCompletedPrimary(&d,&w,&sample));
        CHECK(sample.QuadPart==-1 && w.PrimaryAddress.QuadPart==30);
        scanned=30;
        CHECK(WddmReadCompletedPrimary(&d,&w,&sample) && sample.QuadPart==30);
        CHECK(w.PrimarySegment==3 && hardware==3);
        CHECK(level ? arms==0 : arms==3);
        request.VidPnSourceId=1;CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_INVALID_PARAMETER);
        CHECK(hardware==3);
        {
            BC250_WDDM_OBJECT allocation={0};
            allocation.Magic=BC250_WDDM_MAGIC_ALLOCATION;
            allocation.Allocation.Width=1366;allocation.Allocation.Height=768;
            allocation.Allocation.Pitch=5888;allocation.Allocation.Format=D3DDDIFMT_A8R8G8B8;
            allocation.Allocation.Size=5888ull*768;
            request.VidPnSourceId=0;request.PrimaryAddress.QuadPart=30;
            request.hAllocation=&allocation;
            hardware_result=STATUS_IO_TIMEOUT;
            CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_IO_TIMEOUT);
            CHECK(w.PrimaryPitch==5632 && hardware==4 && programmed_pitch==5632);
            hardware_result=STATUS_SUCCESS;
            CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_SUCCESS);
            CHECK(w.PrimaryPitch==5888 && hardware==5 && programmed_pitch==5888);
            request.hAllocation=NULL;request.PrimaryAddress.QuadPart=40;
            CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_SUCCESS);
            CHECK(programmed_pitch==5888 && w.PrimaryBytes==5888ull*768);
            request.hAllocation=&allocation;allocation.Allocation.Size--;
            CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_INVALID_PARAMETER);
            CHECK(hardware==6 && !(w.PrimarySequence&1));
        }
    }
    printf("vidpn publication: %u checks, %u failures\\n",checks,failures);
    return failures?1:0;
}
''';p.write_text(s)
