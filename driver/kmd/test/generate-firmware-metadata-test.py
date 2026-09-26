from pathlib import Path
import sys
root=Path(sys.argv[1]);out=Path(sys.argv[2])
def function(source,marker):
    a=source.index(marker);b=source.index('{',a);depth=1;i=b+1
    while depth:
        if source[i]=='{':depth+=1
        elif source[i]=='}':depth-=1
        i+=1
    return source[a:i]+'\n'
psp=(root/'driver/kmd/psp.c').read_text()
code=function(psp,'static NTSTATUS ReadFirmwareMetadata(')+function(psp,'NTSTATUS PspReadFirmware(')
a=psp.index('            psp->Loaded = (result == 0 && Data->CommandsDone')
b=psp.index('            break;',a)
code+='static void CompleteLoad(BC250_PSP* psp, int result, unsigned done, unsigned total, struct bc250_umd_firmware firmware) {\n struct counters {unsigned CommandsDone,CommandCount;} counters={done,total};\n struct counters* Data=&counters;\n'+psp[a:b]+'}\n'
wddm=(root/'driver/kmd/wddm.c').read_text()
a=wddm.index('    case DXGKQAITYPE_UMDRIVERPRIVATE:');b=wddm.index('    default:',a)
code+='static NTSTATUS QueryCaps(BC250_DEVICE* device, QUERY* QueryAdapterInfo) {\n NTSTATUS status=STATUS_INVALID_PARAMETER;\n switch(DXGKQAITYPE_UMDRIVERPRIVATE) {\n'+wddm[a:b]+' }\n return status;\n}\n'
if '--omit-runtime-firmware' in sys.argv[3:]:
    line='        RtlCopyMemory((PUCHAR)QueryAdapterInfo->pOutputData+UMD_CAPS_FIRMWARE_OFFSET,&firmware,sizeof(firmware));'
    assert line in code
    code=code.replace(line,'        (void)firmware; /* negative control: stale historical replies */')
out.write_text(code)
