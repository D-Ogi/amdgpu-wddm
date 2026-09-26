from pathlib import Path
import argparse
p=argparse.ArgumentParser();p.add_argument('--gfx',type=Path,required=True);p.add_argument('--sdma',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
g=a.gfx.read_text();s=a.sdma.read_text()
def fn(text,sig):
 start=text.index(sig);return text[start:text.index('\n}\n',start)+3]
body='\n'.join(fn(g,f) for f in ('static void GfxAccessClose(', 'static void GfxAccessOpen(', 'NTSTATUS GfxRecoverSdma0Unpublished('))
body='\n'.join(line for line in g.splitlines() if line.startswith('#define BC250_RECOVERY_'))+'\n'+body
shim='\n'.join(fn(s,f) for f in ('unsigned int bc250_sdma_fence_size(', 'int bc250_sdma_emit_fence(', 'int bc250_sdma_recovery_probe_submit(', 'int bc250_sdma_recovery_transport(', 'u64 bc250_sdma_fence_addr(', 'u64 bc250_sdma_fence_read('))
a.out.write_text(Path(__file__).with_name('sdma0_recovery_test.c').read_text().replace('/* ACTUAL_SOURCE */',shim+'\n'+body))
