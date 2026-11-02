from pathlib import Path
import argparse
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
parts=[]
# Each name is the start of a definition; it ends at its closing brace in column 0 (a typedef also takes its name).
for file,names in [('dcn.c',['static BOOLEAN AddressAllowed(', 'static NTSTATUS CaptureFirmwareSurface(', 'static NTSTATUS FillSurface(',
                             'typedef struct DCN_SCANOUT_OPS_CONTEXT {', 'static void* DcnOpsMap(', 'static void DcnOpsUnmap(',
                             'static int DcnOpsAllowed(', 'void DcnUnmapScanout(', 'BOOLEAN DcnScanoutMapping(']),
                   ('wddm.c',['static BOOLEAN WddmCopyScanoutRows(', 'static NTSTATUS Bc250WddmGetStandardAllocationDriverData('])]:
 s=(a.source/file).read_text()
 for name in names:
  start=s.index(name);end=s.index('\n}',start)+2
  if name.startswith('typedef'):end=s.index(';',end)+1
  parts.append(s[start:end])
fixture=Path(__file__).with_name('scanout_geometry_test.c').read_text();a.out.write_text(fixture.replace('/* ACTUAL_SOURCE */','\n'.join(parts)))
print('Extracted scanout geometry, firmware capture, mapping rule operations, mapping, fill, row copy and allocation DDI')
