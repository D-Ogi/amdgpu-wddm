from pathlib import Path
p=Path('P:/bc-250/bc250-win/driver/kmd/test/vidpn_flip_test.c');s=p.read_text()
s=s.replace('static int irq,pending,inject_update,nested_writer;', 'static int irq,pending,inject_update,nested_writer,inject_fallback_update;')
s=s.replace('{ (void)d; *out=(ULONGLONG)displayed; return STATUS_SUCCESS; }','''{
    *out=(ULONGLONG)displayed;
    if(inject_fallback_update){inject_fallback_update=0;d->Wddm->PrimarySequence+=2;}
    return STATUS_SUCCESS;
}''')
needle='''        // Matching address alone cannot override an asserted pending bit.
''';replacement='''        // Fallback is also generation-protected, even for an ABA writer.
        before=reports;pending=1;inject_fallback_update=1;d.DcnVsyncAcked=1;WddmDcnVsync(&d);
        CHECK(reports==before);
        // Matching address alone cannot override an asserted pending bit.
''';assert needle in s;s=s.replace(needle,replacement);p.write_text(s)
