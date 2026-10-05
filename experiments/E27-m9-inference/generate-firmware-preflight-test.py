from pathlib import Path
import sys
root=Path(sys.argv[1]);out=Path(sys.argv[2]);here=Path(__file__).resolve().parent
s=(root/'driver/kmd/psp.c').read_text()
def get(marker):
 a=s.index(marker);b=s.index('{',a);depth=1;i=b+1
 while depth:
  if s[i]=='{':depth+=1
  elif s[i]=='}':depth-=1
  i+=1
 return s[a:i]+'\n'
h=(root/'driver/shim/include/bc250_psp.h').read_text();a=h.index('enum bc250_fw_file {');b=h.index('};',a)+2
types=s[s.index('typedef struct _BC250_PSP_FILES'):s.index('// ---- firmware files')]
prefix=(here/'firmware-preflight-test-prefix.c').read_text().replace('/* TYPES */',h[a:b]+'\n'+types)
code=prefix+get('static void FreeFiles(')+get('static NTSTATUS ReadFiles(')
a=s.index('    if (Data->Op > BC250_PSP_OP_UNLOAD)',s.index('static void PspExecutePrepared('))
b=s.index('    ExAcquireFastMutex(',a)
code+='static void PspExecutePrepared(BC250_DEVICE* Device,BC250_ESCAPE_PSP* Data,const BC250_PSP_FIRMWARE* Prepared) {\n BC250_PSP_FILES files={0}; NTSTATUS status=0; ULONG failedFile=0;\n'+s[a:b]
code+=' hardwareCalls++; usedByte=files.Data[0][0]; Data->State=7;Data->CommandCount=2;Data->CommandsDone=loadFail?1:2;Data->Result=loadFail?-62:0;Data->NtStatus=0;Data->Status=loadFail?BC250_ESCAPE_STATUS_REFUSED:BC250_ESCAPE_STATUS_DONE;\n'
code+=' if (!Prepared) FreeFiles(&files);\n}\n'
for marker in ['void PspReleaseFirmware(', 'NTSTATUS PspPrepareFirmware(', 'NTSTATUS PspInitializePrepared(']:code+=get(marker)
code+=(here/'firmware-preflight-test-suffix.c').read_text()
out.write_text(code)
