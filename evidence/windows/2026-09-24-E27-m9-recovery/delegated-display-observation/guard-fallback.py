from pathlib import Path
root=Path('P:/bc-250/bc250-win/driver/kmd')
p=root/'wddm.c';s=p.read_text();old='''        ULONGLONG scanned;
        InterlockedIncrement(&Device->DcnVsyncDeferred); // completion deferred, not the vblank
        if (!NT_SUCCESS(DcnReadScanoutAddress(Device, &scanned))) return;
        data.CrtcVsync.PhysicalAddress.QuadPart = (LONGLONG)scanned;''';new='''        ULONGLONG scanned;
        LONG generation;
        InterlockedIncrement(&Device->DcnVsyncDeferred); // completion deferred, not necessarily the vblank
        generation = InterlockedCompareExchange(&wddm->PrimarySequence, 0, 0);
        if (generation & 1) return;
        if (!NT_SUCCESS(DcnReadScanoutAddress(Device, &scanned))) return;
        // If the pending bit outlives the address latch, reporting the requested
        // address would still retire the flip early. Only the distinct previous
        // buffer is safe here; a matching address needs the completed path above.
        if (scanned == (ULONGLONG)InterlockedCompareExchange64(&wddm->PrimaryAddress.QuadPart, 0, 0)) return;
        if (InterlockedCompareExchange(&wddm->PrimarySequence, 0, 0) != generation) return;
        data.CrtcVsync.PhysicalAddress.QuadPart = (LONGLONG)scanned;''';assert old in s;s=s.replace(old,new);p.write_text(s)
p=root/'test/vidpn_flip_test.c';s=p.read_text().replace('CHECK(reports==before+1 && reported==displayed);','CHECK(reports==before);');old='''        before=reports;displayed=20;d.DcnVsyncAcked=1;WddmDcnVsync(&d);
        CHECK(reports==before+1 && reported==20);''';new='''        // Matching address alone cannot override an asserted pending bit.
        before=reports;displayed=20;pending=1;d.DcnVsyncAcked=1;WddmDcnVsync(&d);
        CHECK(reports==before);
        pending=0;d.DcnVsyncAcked=1;WddmDcnVsync(&d);
        CHECK(reports==before+1 && reported==20);''';assert old in s;s=s.replace(old,new);p.write_text(s)
p=root/'bc250kmd.h';s=p.read_text();s=s.replace("// is still pending (dcn.c's DcnFlipPending). A no-op with Device->VidPnFlipEnabled closed.","// is still pending. A pending flip reports the distinct scanned buffer when the\n// generation is stable. A no-op with Device->VidPnFlipEnabled closed.");p.write_text(s)
