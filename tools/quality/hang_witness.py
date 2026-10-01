"""Compile/run the KMD193 hang-witness decode helpers (driver/kmd/ih_fault.h, paging_identity.h)."""
import argparse, pathlib, subprocess
p=argparse.ArgumentParser();p.add_argument('--out',required=True);a=p.parse_args()
out=pathlib.Path(a.out).resolve();out.mkdir(parents=True,exist_ok=True)
repo=pathlib.Path(__file__).resolve().parents[2]
source=repo/'driver/kmd/test/hang_witness_test.c'
exe=out/'hang_witness_test.exe'
# /wd4127: most of the checks here compare two compile-time constants on purpose - that the kind numbers and
# the field masks still are what the readers outside this build expect (same disable as run_ih_consume.ps1).
cmd=['cl','/nologo','/TC','/W4','/WX','/wd4127','/Od','/I'+str(repo/'driver/kmd'),
     '/I'+str(repo/'third_party/linux-amdgpu'),
     str(source),'/Fe'+str(exe),'/Fo'+str(out/'test.obj')]
r=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT);(out/'build.log').write_bytes(r.stdout)
print(r.stdout.decode(errors='replace'));r.check_returncode()
r=subprocess.run([str(exe)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT);(out/'run.log').write_bytes(r.stdout)
print(r.stdout.decode(errors='replace'));r.check_returncode()
