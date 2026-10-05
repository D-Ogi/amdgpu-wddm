from pathlib import Path
import sys
root=Path(sys.argv[1]);out=Path(sys.argv[2]);here=Path(__file__).resolve().parent
s=(root/'driver/kmd/gfx.c').read_text()
a=s.index('typedef struct _BC250_PAGING_STREAM')
b=s.index('// PASSIVE_LEVEL. Resolve each page',a)
actual=s[a:b]
a=s.index('NTSTATUS GfxPagingBuildFlush(')
b=s.index('// DISPATCH_LEVEL. Validate all private records',a)
actual+=s[a:b]
template=(root/'driver/shim/test/paging_packets.c').read_text()
prefix=(here/'paging-route-test-prefix.c').read_text()
suffix=(here/'paging-route-test-suffix.c').read_text()
a=template.index('int main(int argc, char **argv)')
template=template[:a]+prefix+actual+suffix+template[a:]
template=template.replace('\tcase_mapped_copy();','\tcase_mapped_copy();\n\tcase_kmd_routes();\n\tcase_kmd_flush();')
out.write_text(template)
