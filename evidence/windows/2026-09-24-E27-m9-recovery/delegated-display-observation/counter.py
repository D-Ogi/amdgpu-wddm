from pathlib import Path
root=Path('P:/bc-250/bc250-win/driver/kmd')
p=root/'bc250kmd.h';s=p.read_text().replace('    volatile LONG DcnVsyncDeferred;      // a hardware tick postponed one DPC because the flip was still pending', '    volatile LONG DcnVsyncDeferred;      // completion observation deferred (may report the old buffer)\n    volatile LONG DcnVsyncOldBufferReports; // vblanks preserved with a distinct observed scanout');p.write_text(s)
p=root/'wddm.c';s=p.read_text().replace('''        data.CrtcVsync.PhysicalAddress.QuadPart = (LONGLONG)scanned;
''','''        data.CrtcVsync.PhysicalAddress.QuadPart = (LONGLONG)scanned;
        InterlockedIncrement(&Device->DcnVsyncOldBufferReports);
''');s=s.replace('''"acked %ld refused %ld deferred (flip still pending)",''','''"acked %ld refused %ld completion-deferred %ld old-buffer-reports",''');s=s.replace('''Wddm->Device->DcnVsyncTicks, Wddm->Device->DcnVsyncRefused, Wddm->Device->DcnVsyncDeferred);''','''Wddm->Device->DcnVsyncTicks, Wddm->Device->DcnVsyncRefused, Wddm->Device->DcnVsyncDeferred,
             Wddm->Device->DcnVsyncOldBufferReports);''');p.write_text(s)
p=root/'test/vidpn_flip_test.c';s=p.read_text().replace('volatile LONG DcnVsyncAcked, DcnVsyncDeferred;', 'volatile LONG DcnVsyncAcked, DcnVsyncDeferred, DcnVsyncOldBufferReports;');s=s.replace('''        // Fallback is also generation-protected, even for an ABA writer.''','''        CHECK(d.DcnVsyncOldBufferReports==2);
        // Fallback is also generation-protected, even for an ABA writer.''');p.write_text(s)
