"""Compile/run the table budget host test (driver/kmd/test/table_budget_test.c, BD-028)."""
import argparse, pathlib, subprocess
p=argparse.ArgumentParser();p.add_argument('--out',required=True);a=p.parse_args()
out=pathlib.Path(a.out).resolve();out.mkdir(parents=True,exist_ok=True)
repo=pathlib.Path(__file__).resolve().parents[2]
source=repo/'driver/kmd/test/table_budget_test.c'
exe=out/'table_budget_test.exe'
cmd=['cl','/nologo','/TC','/W4','/WX','/Od','/I'+str(repo/'driver/kmd'),
     str(source),'/Fe'+str(exe),'/Fo'+str(out/'test.obj')]
r=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT);(out/'build.log').write_bytes(r.stdout)
print(r.stdout.decode(errors='replace'));r.check_returncode()
r=subprocess.run([str(exe)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT);(out/'run.log').write_bytes(r.stdout)
print(r.stdout.decode(errors='replace'));r.check_returncode()
