"""Extract the actual builder and stop entry/exit for a Windows host lifetime test.
Kernel push locks are modeled by SRW locks; this is not kernel scheduler validation.
"""
from pathlib import Path
import sys
here=Path(__file__).resolve().parent
s=Path(sys.argv[1]).read_text()
a=s.index("NTSTATUS GfxPagingBuild(")
b=s.index("// DISPATCH_LEVEL. Validate all private records",a)
builder=s[a:b]
a=s.index("void GfxStop(")
stop=s[a:]
a=stop.index("    KeEnterCriticalRegion();")
b=stop.index("    if (gfx != NULL &&",a)
entry=stop[a:b]
a=stop.index("    ExReleaseFastMutex(&Device->GartLock);")
b=stop.index("\n}",a)
leave=stop[a:b]
stopfn="static DWORD WINAPI StopThread(void* arg) { BC250_DEVICE* Device=arg; BC250_GFX* gfx;\n"+entry+"SetEvent(stopEntered);\n"+leave+"return 0; }\n"
Path(sys.argv[2]).write_text((here/'paging-lifetime-test-prefix.c').read_text()+builder+stopfn+(here/'paging-lifetime-test-suffix.c').read_text())

if len(sys.argv)>3 and sys.argv[3]=="--disable-builder-lock":
    p=Path(sys.argv[2])
    p.write_text(p.read_text().replace("ExAcquirePushLockShared(&Device->GfxPagingLock);", "(void)Device;").replace("ExReleasePushLockShared(&Device->GfxPagingLock);", "(void)Device;"))
