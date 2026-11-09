"""Compile/run the actual included identity helpers against callback/lock stubs."""
import argparse, pathlib, subprocess
p=argparse.ArgumentParser();p.add_argument('--out',required=True);a=p.parse_args()
out=pathlib.Path(a.out).resolve();out.mkdir(parents=True,exist_ok=True)
for name in ('allocation_identity_test','mpo_storage_test'):
 source=pathlib.Path(__file__).resolve().parents[2]/f'driver/kmd/test/{name}.c'
 exe=out/f'{name}.exe'
 cmd=['cl','/nologo','/TC','/W4','/WX','/Od',str(source),'/Fe'+str(exe),'/Fo'+str(out/f'{name}.obj')]
 r=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT);(out/f'{name}-build.log').write_bytes(r.stdout)
 print(r.stdout.decode(errors='replace'));r.check_returncode()
 r=subprocess.run([str(exe)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT);(out/f'{name}-run.log').write_bytes(r.stdout)
 print(r.stdout.decode(errors='replace'));r.check_returncode()
