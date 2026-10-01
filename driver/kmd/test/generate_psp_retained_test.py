from pathlib import Path
import sys
source=Path(sys.argv[1]).read_text();out=Path(sys.argv[2])
def fn(name):
    a=source.index(name);return source[a:source.index('\n}\n',a)+3]
a=source.index('typedef struct _BC250_PSP {');b=source.index('// ---- firmware files',a)
parts=[source[a:b]]
for sig in ['static void FreeFiles(', 'void PspReleaseFirmware(', 'static void FillAddresses(', 'static int Setup(', 'static NTSTATUS LayOut(', 'static NTSTATUS ReadFirmwareMetadata(', 'NTSTATUS PspReadFirmware(', 'NTSTATUS PspPrepareFirmware(', 'static ULONG Microseconds(', 'static NTSTATUS UnloadHardware(', 'static NTSTATUS Unload(', 'static void RetainPrepared(', 'static void PspExecutePrepared(', 'NTSTATUS PspInitializePrepared(', 'NTSTATUS PspSetPowerRetained(', 'BOOLEAN PspPowerIsSuspended(', 'void PspStop(']:
    parts.append(fn(sig))
text='\n'.join(parts)
if '--drop-retain' in sys.argv: text=text.replace('if (Psp->Retained==Prepared) return;','if (Psp->Retained!=Prepared) return; /* mutation: initial reference omitted */')
# Keep fixtures independent of private typedef ordering.
fixture=Path(__file__).with_name('psp_retained_test.c').read_text()
a=fixture.index('/* ACTUAL_TYPES */');b=fixture.index('/* ACTUAL_FUNCTIONS */')
fixture=fixture.replace('/* ACTUAL_TYPES */',parts[0]).replace('/* ACTUAL_FUNCTIONS */',text[len(parts[0])+1:])
out.write_text(fixture)
