from pathlib import Path
p=Path('bc250-win/experiments/E27-m9-inference/generate-paging-route-test.py');s=p.read_text();s=s.replace("a=vm.index('NTSTATUS VidMmStartLayout(');b=vm.index('// PASSIVE_LEVEL. Immediate CPU_VIRTUAL',a)","a=vm.index('NTSTATUS VidMmStartLayout(');b=vm.index('// Count only completely validated encodings.',a)");p.write_text(s)
