from pathlib import Path
import sys
root=Path(sys.argv[1]);out=Path(sys.argv[2]);here=Path(__file__).resolve().parent
s=(root/'driver/kmd/gfx.c').read_text()
a=s.index('typedef struct _BC250_PAGING_STREAM')
b=s.index('// PASSIVE_LEVEL. Resolve each page',a)
actual=s[a:b]
a=s.index('NTSTATUS GfxPagingBuildUpdate(')
b=s.index('// DISPATCH_LEVEL. Validate all private records',a)
actual+=s[a:b]
vm=(root/'driver/kmd/vidmm.c').read_text()
a=vm.index('BOOLEAN VidMmRootPhysical(');b=vm.index('// PASSIVE_LEVEL page-table walk',a)
root_function=vm[a:b]
a=vm.index('BOOLEAN VidMmEncodePageTable(');b=vm.index('// A root page table address',a)
encoder=vm[a:b]
a=vm.index('static NTSTATUS VidMmUpdatePageTableLocked(');b=vm.index('void VidMmSetRootPageTable(',a)
actual=root_function+encoder+vm[a:b]+actual
a=vm.index('NTSTATUS VidMmStart(');b=vm.index('// PASSIVE_LEVEL. Immediate CPU_VIRTUAL',a)
actual+=vm[a:b]
a=vm.index('static BOOLEAN VidMmTranslateViewLocked(');b=vm.index('// Same walk as VidMmTranslate',a)
actual+=vm[a:b].replace('VidMmTranslate','VidMmTranslateRetained')
ws=(root/'driver/kmd/wddm.c').read_text()
a=ws.index('static NTSTATUS WddmPublishPagingRecord(');b=ws.index('static DXGKDDI_BUILDPAGINGBUFFER',a)
actual+=ws[a:b]
template=(root/'driver/shim/test/paging_packets.c').read_text()
prefix=(here/'paging-route-test-prefix.c').read_text()
suffix=(here/'paging-route-test-suffix.c').read_text()
a=template.index('int main(int argc, char **argv)')
template=template[:a]+prefix+actual+suffix+template[a:]
template=template.replace('\tcase_mapped_copy();','\tcase_mapped_copy();\n\tcase_kmd_routes();\n\tcase_kmd_flush();\n\tcase_kmd_updates();\n\tcase_cpu_updates();\n\tcase_retained_mapping();\n\tcase_queued_pte_ordering();')
template=template.replace('\tcase_transfer_three_pages();', '\tfor (i=1;i<argc;i++) if (strcmp(argv[i], "--queued-pte-ordering")==0) { case_queued_pte_ordering(); printf("queued PTE ordering: %u checks, %u failures\\n",g_checks,g_failures); return g_failures ? 1 : 0; }\n\tcase_transfer_three_pages();')
if '--omit-logical-commit' in sys.argv:
 template=template.replace('status=VidMmCommitPagingUpdate(&Build->UpdatePageTable,Start,Next-Start);','status=STATUS_SUCCESS;')
out.write_text(template)
