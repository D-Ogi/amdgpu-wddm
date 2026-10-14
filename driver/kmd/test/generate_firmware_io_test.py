"""Compile the production file reader with fault-injected OS calls, no firmware bytes."""
from pathlib import Path
import sys

root, out = map(Path, sys.argv[1:3])
source = (root / 'driver/kmd/psp.c').read_text()

def function(marker):
    start = source.index(marker)
    end = source.index('\n}', start) + 2
    return source[start:end] + '\n'

code = 'static HANDLE g_FirmwareDirectory;\n'
code += function('NTSTATUS PspDriverInit(')
code += function('void PspDriverCleanup(')
code += function('static NTSTATUS ReadOneFile(')
if '--accept-short' in sys.argv[3:]:
    before = 'io.Information != (ULONG_PTR)info.EndOfFile.QuadPart'
    assert before in code
    code = code.replace(before, '0 /* negative control */')
out.write_text(code)
