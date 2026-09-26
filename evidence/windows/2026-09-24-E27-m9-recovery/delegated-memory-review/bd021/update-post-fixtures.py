from pathlib import Path
p=Path('bc250-win/experiments/E27-m9-inference/paging-route-test-suffix.c');s=p.read_text()
marker='static void case_wddm_memory_layout(void)'
helper='''// Positive paging fixtures must describe an identified, nonempty POST surface
// since M435. Its first64KiB stays outside application/table storage.
static void prepare_layout_post(BC250_DEVICE* Device)
{
 Device->Post.Pitch=4;Device->Post.Height=1;layout_fb_known=1;layout_fb_offset=0;
}

'''
s=s.replace(marker,helper+marker)
old='d.VramLength=BC250_VRAM_TOP_RESERVED+16*1024*1024;layout_fb_known=0;'
new='''d.VramLength=BC250_VRAM_TOP_RESERVED+16*1024*1024;layout_fb_known=0;
 check(!WddmMemoryLayout(&d,&offset,&length,&tableOffset,&tableLength),"unknown POST refuses allocation over live scanout");
 prepare_layout_post(&d);d.VramLength+=65536;'''
assert s.count(old)==1;s=s.replace(old,new)
s=s.replace('d.VramLength=BC250_VRAM_TOP_RESERVED+4*1024*1024;', 'd.VramLength=BC250_VRAM_TOP_RESERVED+4*1024*1024+65536;')
s=s.replace(' layout_fb_known=0;\n check(WddmMemoryLayout(&dev', ' prepare_layout_post(&dev);\n check(WddmMemoryLayout(&dev')
s=s.replace('ring.max_dw=1024;layout_fb_known=0;', 'ring.max_dw=1024;prepare_layout_post(&d);')
s=s.replace('d.VramMcBase=0x100000000ull;layout_fb_known=0;', 'd.VramMcBase=0x100000000ull;prepare_layout_post(&d);')
s=s.replace('  check(WddmMemoryLayout(&d,&app,&appLength,&table,&tableLength)', '  prepare_layout_post(&d);\n  check(WddmMemoryLayout(&d,&app,&appLength,&table,&tableLength)')
p.write_text(s)
