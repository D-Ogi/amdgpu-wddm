"""Run null/equality controls against the actual Mesa surface helper."""
import argparse
from pathlib import Path
import subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--mesa',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True)
s=(a.mesa/'src/gallium/auxiliary/util/u_inlines.h').read_text(encoding='utf-8')
b=s.index('static inline bool\npipe_surface_equal(');e=s.index('\n}',b)+2
source='#include <stdbool.h>\n#include <stdio.h>\nstruct pipe_surface { void *texture; unsigned format,nr_samples,level,first_layer,last_layer; };\n'+s[b:e]+'\nint main(void) {\n struct pipe_surface a={0},b={0}; int failures=0;\n failures+=!pipe_surface_equal(0,0);\n failures+=pipe_surface_equal(&a,0);\n failures+=pipe_surface_equal(0,&a);\n failures+=!pipe_surface_equal(&a,&b);\n a.format=1; failures+=pipe_surface_equal(&a,&b); b.format=1;\n a.texture=&failures;b.texture=&failures; failures+=!pipe_surface_equal(&a,&b);\n a.level=1;failures+=pipe_surface_equal(&a,&b);\n printf("surface controls: 7 checks, %d failures\\n",failures);\n return failures?1:0;\n}\n'
f=a.out/'surface-control.c';f.write_text(source)
subprocess.run(['cl','/nologo','/TC','/W4','/WX','/O2',str(f),'/Fo'+str(a.out/'surface-control.obj'),'/Fe'+str(a.out/'surface-control.exe')],check=True)
subprocess.run([str(a.out/'surface-control.exe')],check=True)
