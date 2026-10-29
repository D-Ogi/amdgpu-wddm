"""BD-223 stage S2: compile dcn.c's own plane state code (DcnPlaneCheck to DcnPlaneShapeCurrent) against models."""
from pathlib import Path
import argparse
import re

p = argparse.ArgumentParser()
p.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1] / 'dcn.c')
p.add_argument('--out', type=Path, required=True)
a = p.parse_args()
s = a.source.read_text()
start = s.index('// ---- BD-223 stage S2: the display plan of a plane')
end = s.index('\nULONG DcnPlanePitch(', start)
body = s[start:end]
kmd = Path(__file__).resolve().parents[1]
header = kmd.joinpath('bc250kmd.h').read_text()
connector = kmd.joinpath('connector.h').read_text()
# The types the code uses, from the production headers, so that the fixture cannot drift from them.
types = [re.search(r'^#define BC250_DCN_PLANE_STATE_BYTES [^\r\n]+', header, re.M),
         re.search(r'typedef struct _BC250_DCN_PLANE_CHECK \{.*?\} BC250_DCN_PLANE_CHECK;', header, re.S),
         re.search(r'typedef struct _BC250_LINK_PLANE \{.*?\} BC250_LINK_PLANE;', connector, re.S),
         re.search(r'typedef struct _BC250_LINK_PLANE_DML \{.*?\} BC250_LINK_PLANE_DML;', connector, re.S)]
assert all(types)
fixture = Path(__file__).with_name('dcn_plane_state_test.c').read_text()
assert fixture.count('/* ACTUAL_SOURCE */') == 1 and fixture.count('/* EXTRACTED_TYPES */') == 1
for name in ['ULONG DcnPlaneCheck(', 'BOOLEAN DcnPlaneApproved(', 'static BOOLEAN DcnPlaneShapeAdmit(',
             'static NTSTATUS DcnPlaneShapeLocked(', 'void DcnPlaneShapeModeset(', 'BOOLEAN DcnPlaneShapeIs(']:
    assert body.count(name) == 1, name
fixture = fixture.replace('/* EXTRACTED_TYPES */', '\n'.join(t.group() for t in types))
a.out.write_text(fixture.replace('/* ACTUAL_SOURCE */', body))
print('Extracted the plane state code from', a.source)
