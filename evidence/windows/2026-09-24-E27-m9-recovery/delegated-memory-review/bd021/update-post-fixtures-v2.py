from pathlib import Path
p=Path('bc250-win/experiments/E27-m9-inference/paging-route-test-suffix.c');s=p.read_text()
for name in ['case_aperture_transfer','case_virtual_alias_ordering','case_unequal_virtual_alias_mode']:
 a=s.index('static void '+name+'(');e=s.index('\nstatic ',a+1) if '\nstatic ' in s[a+1:] else len(s)
 part=s[a:e]
 marker=' d.VramPhysical.QuadPart=0x200000000ll;d.VramLength=8ull<<30;'
 assert marker in part,name
 part=part.replace(marker,marker+'prepare_layout_post(&d);',1)
 s=s[:a]+part+s[e:]
a=s.index('static void case_transfer_publication(');e=s.index('\nstatic ',a+1);part=s[a:e]
part=part.replace('"Transfer publication advertised layout");','"Transfer publication advertised layout");\n gfx.PagingCopyStaging.mc=d.VramMcBase+app+24576;')
part=part.replace('dma[3]==(pass==0?8192u:pass==1?4096u:1u)','dma[3]==app+(pass==0?8192u:pass==1?4096u:1u)')
part=part.replace('dma[5]==24576 && dma[20]==24576','dma[5]==app+24576 && dma[20]==app+24576')
part=part.replace('if(dma[3]<16384 && dma[22]<16384','if(dma[3]>=app && dma[3]-app<16384 && dma[22]>=app && dma[22]-app<16384')
part=part.replace('memory+dma[5],memory+dma[3]','memory+(dma[5]-app),memory+(dma[3]-app)').replace('memory+dma[22],memory+dma[20]','memory+(dma[22]-app),memory+(dma[20]-app)')
s=s[:a]+part+s[e:];p.write_text(s)
