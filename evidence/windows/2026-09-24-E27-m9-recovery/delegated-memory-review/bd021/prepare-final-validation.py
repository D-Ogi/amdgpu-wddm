from pathlib import Path
r=Path('bc250-win/experiments/E27-m9-inference')
s=(r/'generate-paging-route-test.py').read_text()
setup=Path('scratch/m9/bd021/isolate-observation.py').read_text()
exec(setup[setup.index("b=Path('scratch"):])
b=Path('scratch/m9/bd021/mutation-harness');b.mkdir(exist_ok=True)
for n in ['paging-route-test-prefix.c','paging-route-test-suffix.c']:(b/n).write_text((r/n).read_text())
s=(r/'generate-paging-route-test.py').read_text();s=s.replace('out.write_text(template)', '''old='(vm->Pte.table_size && segment==vm->Pte.table_segment?2u:1u);'
assert template.count(old)==1
template=template.replace(old,'(vm->Pte.table_size && segment==vm->Pte.table_segment?0u:0u);')
out.write_text(template)''');(b/'generate-paging-route-test.py').write_text(s)
s=Path('bc250-win/driver/shim/test/run_paging.ps1').read_text().replace("$repo = Resolve-Path (Join-Path $here '..\\..\\..')", "$repo = 'P:\\bc-250\\bc250-win'")
s=s.replace("(Join-Path $repo 'experiments\\E27-m9-inference\\generate-paging-route-test.py')","'P:\\bc-250\\scratch\\m9\\bd021\\mutation-harness\\generate-paging-route-test.py'")
(b/'run-mutation.ps1').write_text(s)
s=Path('bc250-win/driver/kmd/build.ps1').read_text();e=s.index("Invoke-Tool (Join-Path $bin 'cl.exe') ($clFlags + $shimInc + @(")
s=s[:e].replace("$here = Split-Path -Parent $MyInvocation.MyCommand.Path","$here = 'P:\\bc-250\\bc250-win\\driver\\kmd'")
s=s.replace('($clFlags + $shimInc + $sources + $shimSources)',"($clFlags + $shimInc + @((Join-Path $here 'vidmm.c')))")
s+='Write-Host "vidmm.c kernel compile PASS"\n';Path('scratch/m9/bd021/compile-vidmm.ps1').write_text(s)
