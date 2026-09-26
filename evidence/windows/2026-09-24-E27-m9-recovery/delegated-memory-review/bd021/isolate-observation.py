from pathlib import Path
r=Path('bc250-win/experiments/E27-m9-inference')
p=r/'generate-paging-route-test.py';s=p.read_text();marker="if '--omit-native-write-permission' in sys.argv:"
new='''template=template.replace('\\tcase_transfer_three_pages();', '\\tfor (i=1;i<argc;i++) if (strcmp(argv[i], "--pte-observations")==0) { case_pte_observations(); printf("PTE observations: %u checks, %u failures\\\\n",g_checks,g_failures); return g_failures ? 1 : 0; }\\n\\tcase_transfer_three_pages();')
'''
assert s.count(marker)==1;s=s.replace(marker,new+marker);p.write_text(s)
b=Path('scratch/m9/bd021/baseline-harness');b.mkdir(parents=True,exist_ok=True)
s=s.replace("a=vm.index('static void VidMmCountEncoding(');b=vm.index('// PASSIVE_LEVEL. Immediate CPU_VIRTUAL',a)\nencoder=vm[a:b]+encoder\n",'')
s=s.replace("b=vm.index('// Count only completely validated encodings.',a)","b=vm.index('// PASSIVE_LEVEL. Immediate CPU_VIRTUAL',a)")
s=s.replace(r'\tcase_pte_observations();\n','')
s='\n'.join(line for line in s.splitlines() if '--pte-observations' not in line)+'\n'
(b/'generate-paging-route-test.py').write_text(s)
s=(r/'paging-route-test-prefix.c').read_text().replace('Written,EncodedCoherent[4][3],EncodedNoncoherent[4][3],EncodedSnoopMismatch[4][3];','Written,EncodedCoherentSystem,EncodedUncachedSystem,EncodedCoherencyMismatch;');(b/'paging-route-test-prefix.c').write_text(s)
s=(r/'paging-route-test-suffix.c').read_text();a=s.index('// BD-021 observations must preserve');e=s.index('static void case_retained_mapping(',a);s=s[:a]+s[e:];s=s.replace('g_VidMm.EncodedCoherent[0][0]','g_VidMm.EncodedCoherentSystem').replace('g_VidMm.EncodedNoncoherent[0][0]','g_VidMm.EncodedUncachedSystem').replace('g_VidMm.EncodedSnoopMismatch[0][0]','g_VidMm.EncodedCoherencyMismatch');(b/'paging-route-test-suffix.c').write_text(s)
s=Path('bc250-win/driver/shim/test/run_paging.ps1').read_text();s=s.replace("$repo = Resolve-Path (Join-Path $here '..\\..\\..')", "$repo = 'P:\\bc-250\\scratch\\m9\\display137-build-source'")
s=s.replace("(Join-Path $repo 'experiments\\E27-m9-inference\\generate-paging-route-test.py')","'P:\\bc-250\\scratch\\m9\\bd021\\baseline-harness\\generate-paging-route-test.py'")
(b/'run-baseline.ps1').write_text(s)
