from pathlib import Path
import argparse
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
parts=[]
for file,names in [('dcn.c',['static BOOLEAN AddressAllowed(', 'static NTSTATUS CaptureFirmwareSurface(', 'static NTSTATUS FillSurface(', 'void DcnUnmapScanout(', 'BOOLEAN DcnScanoutMapping(']),('wddm.c',['static BOOLEAN WddmCopyScanoutRows(', 'static NTSTATUS Bc250WddmGetStandardAllocationDriverData('])]:
 s=(a.source/file).read_text()
 for name in names:
  start=s.index(name);end=s.index('\n}',start)+2;parts.append(s[start:end])
fixture=Path(__file__).with_name('scanout_geometry_test.c').read_text();a.out.write_text(fixture.replace('/* ACTUAL_SOURCE */','\n'.join(parts)))
print('Extracted scanout geometry, firmware capture, mapping, fill, row copy and allocation DDI')
