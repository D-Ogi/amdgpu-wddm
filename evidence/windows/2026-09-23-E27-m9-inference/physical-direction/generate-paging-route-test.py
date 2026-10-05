from pathlib import Path
import sys
root=Path(sys.argv[1]);out=Path(sys.argv[2]);here=Path(__file__).resolve().parent
s=(root/'driver/kmd/gfx.c').read_text()
a=s.index('BOOLEAN GfxPagingMdlAddress(')
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
a=vm.index('NTSTATUS VidMmStartLayout(');b=vm.index('// PASSIVE_LEVEL. Immediate CPU_VIRTUAL',a)
actual+=vm[a:b]
a=vm.index('static BOOLEAN VidMmTranslateViewLocked(');b=vm.index('// Same walk as VidMmTranslate',a)
actual+=vm[a:b].replace('VidMmTranslate','VidMmTranslateRetained')
a=vm.index('static BOOLEAN VidMmProbeIbLocked(');b=vm.index('void VidMmSummary(',a)
actual+=vm[a:b]
gs=(root/'driver/kmd/gart.c').read_text()
a=gs.index('NTSTATUS GartCaptureAperture(');b=gs.index('// ---- start and stop',a)
actual+=gs[a:b]
ws=(root/'driver/kmd/wddm.c').read_text()
a=ws.index('static BOOLEAN WddmMemoryLayout(',ws.index('// The one part of VRAM'))
b=ws.index('// Two passes, as documented:',a)
actual+=ws[a:b]
a=ws.index('static NTSTATUS WddmPublishPagingRecordCore(');b=ws.index('static DXGKDDI_BUILDPAGINGBUFFER',a)
actual+=ws[a:b]

for marker in ['static NTSTATUS WddmQuerySegment4(', 'static NTSTATUS WddmPageTableLevelDesc(']:
 a=ws.index(marker);b=ws.index('{',a);depth=1;i=b+1
 while depth:
  if ws[i]=='{':depth+=1
  if ws[i]=='}':depth-=1
  i+=1
 actual+=ws[a:i]+'\n'
template=(root/'driver/shim/test/paging_packets.c').read_text()
prefix=(here/'paging-route-test-prefix.c').read_text()
suffix=(here/'paging-route-test-suffix.c').read_text()
a=template.index('int main(int argc, char **argv)')
template=template[:a]+prefix+actual+suffix+template[a:]
template=template.replace('\tcase_mapped_copy();','\tcase_mapped_copy();\n\tcase_kmd_routes();\n\tcase_kmd_flush();\n\tcase_kmd_updates();\n\tcase_cpu_updates();\n\tcase_retained_mapping();\n\tcase_queued_pte_ordering();\n\tcase_copy_range_builder();\n\tcase_copy_publication();\n\tcase_copy_real_walk();\n\tcase_separate_table_extent();\n\tcase_wddm_memory_layout();\n\tcase_wddm_segment_queries();\n\tcase_probe_physical_copy();\n\tcase_mdl_address();\n\tcase_physical_stream();\n\tcase_physical_fill_ddi();\n\tcase_virtual_fill_publication();\n\tcase_virtual_fill_real_walk();\n\tcase_prepare_transfer();\n\tcase_copy_slice_identity();\n\tcase_transfer_publication();\n\tcase_virtual_transfer_real_walk();\n\tcase_aperture_partition();\n\tcase_aperture_ddi();\n\tcase_aperture_state_lifetime();\n\tcase_aperture_transfer();\n\tcase_indirect_multipass_bytes();case_permutation_packets(0);case_permutation_packets(1);case_permutation_packets(2);case_permutation_multipass(0);case_permutation_multipass(1);case_virtual_alias_ordering(0);case_virtual_alias_ordering(1);case_virtual_alias_ordering(2);case_graph_shadow_publication();case_retained_graph_capture(0);case_retained_graph_capture(1);case_retained_graph_capture(2);case_monotone_alias_direction();case_unequal_virtual_alias(0,1);case_unequal_virtual_alias(1,1);case_unequal_virtual_alias(0,2);case_unequal_virtual_alias(1,2);case_unequal_virtual_alias(2,2);case_unequal_virtual_alias(3,2);')
template=template.replace('\tcase_transfer_three_pages();', '\tfor (i=1;i<argc;i++) if (strcmp(argv[i], "--queued-pte-ordering")==0) { case_queued_pte_ordering(); printf("queued PTE ordering: %u checks, %u failures\\n",g_checks,g_failures); return g_failures ? 1 : 0; }\n\tcase_transfer_three_pages();')
if '--omit-logical-commit' in sys.argv:
 template=template.replace('status=VidMmCommitPagingUpdate(&Build->UpdatePageTable,Start,Next-Start);','status=STATUS_SUCCESS;')
if '--omit-logical-commit' in sys.argv:
 template=template.replace('status=VidMmCommitPagingCopy(CopySource,CopyDestination,CopyCount);','(void)CopySource;(void)CopyDestination;status=STATUS_SUCCESS;')
if '--omit-logical-commit' in sys.argv:
 template=template.replace('status=VidMmCommitPagingFill(FillPhysical,FillBytes,FillPattern);','(void)FillPhysical;(void)FillPattern;status=STATUS_SUCCESS;')
if '--omit-logical-commit' in sys.argv:
 template=template.replace('status=VidMmCommitPagingTransfer(Transfer);','status=STATUS_SUCCESS;')
if '--omit-aperture-commit' in sys.argv:
 template=template.replace('status=VidMmCommitPagingAperture(Build,Start,Next);','status=STATUS_SUCCESS;')
if '--repeat-mdl-source-page' in sys.argv:
 old='return PagingResolvePhysical(stream,stream->Source,Address,Bytes,Mc);'
 assert template.count(old)==1
 template=template.replace(old,'return PagingResolvePhysical(stream,stream->Source,stream->Source->Mdl ? (Address & 4095) : Address,Bytes,Mc);')
if '--break-cycle-restore' in sys.argv:
 old='PAGING_U64 from=moves[i].source==PAGING_PERMUTATION_SCRATCH ? gfx->PagingCopyStaging.mc :'
 assert template.count(old)==1
 template=template.replace(old,'PAGING_U64 from=moves[i].source==PAGING_PERMUTATION_SCRATCH ? sources[0]|PAGING_SYSTEM_ADDRESS :')
if '--break-cycle-restore' in sys.argv:
 old='if(ids[side]==PAGING_PERMUTATION_SCRATCH)mc[side]=gfx->PagingCopyStaging.mc;'
 assert template.count(old)==1
 template=template.replace(old,'if(ids[side]==PAGING_PERMUTATION_SCRATCH)mc[side]=side?gfx->PagingCopyStaging.mc:(Capture->SystemPages[0]?(Capture->Pages[0]|PAGING_SYSTEM_ADDRESS):Capture->Pages[0]-Capture->VramPhysical+Capture->VramMcBase);')
template=template.replace('\tcase_transfer_three_pages();', '\tfor (i=1;i<argc;i++) if (strcmp(argv[i], "--virtual-alias-ordering")==0) { case_virtual_alias_ordering(0);case_virtual_alias_ordering(1);case_virtual_alias_ordering(2); printf("virtual alias ordering: %u checks, %u failures\\n",g_checks,g_failures); return g_failures ? 1 : 0; }\n\tcase_transfer_three_pages();')
if '--omit-logical-commit' in sys.argv:
 template=template.replace('status=VidMmCommitPagingGraph(Graph);','status=STATUS_SUCCESS;')
template=template.replace('\tcase_transfer_three_pages();', '\tfor (i=1;i<argc;i++) if (strcmp(argv[i], "--graph-shadow-publication")==0) { case_graph_shadow_publication(); printf("graph shadow publication: %u checks, %u failures\\n",g_checks,g_failures); return g_failures ? 1 : 0; }\n\tcase_transfer_three_pages();')
template=template.replace('\tcase_transfer_three_pages();', '\tfor (i=1;i<argc;i++) if (strcmp(argv[i], "--retained-graph-capture")==0) { case_retained_graph_capture(0);case_retained_graph_capture(1);case_retained_graph_capture(2);case_monotone_alias_direction();case_unequal_virtual_alias(0,1);case_unequal_virtual_alias(1,1);case_unequal_virtual_alias(0,2);case_unequal_virtual_alias(1,2);case_unequal_virtual_alias(2,2);case_unequal_virtual_alias(3,2); printf("retained graph packets: %u checks, %u failures\\n",g_checks,g_failures); return g_failures ? 1 : 0; }\n\tcase_transfer_three_pages();')
if '--force-graph-replan' in sys.argv:
 old='if(Capture->PlannedBand!=Band) {'
 assert template.count(old)==1
 template=template.replace(old,'if(1) {')
template=template.replace('\tcase_transfer_three_pages();', '\tfor (i=1;i<argc;i++) if (strcmp(argv[i], "--unequal-virtual-alias")==0) { case_unequal_virtual_alias(0,2);case_unequal_virtual_alias(1,2);case_unequal_virtual_alias(2,2);case_unequal_virtual_alias(3,2); printf("unequal virtual alias: %u checks, %u failures\\n",g_checks,g_failures); return g_failures ? 1 : 0; }\n\tcase_transfer_three_pages();')
if '--force-forward-alias' in sys.argv:
 old='c->Backward=(unsigned)backward;'
 assert template.count(old)==1
 template=template.replace(old,'c->Backward=0;')
out.write_text(template)
