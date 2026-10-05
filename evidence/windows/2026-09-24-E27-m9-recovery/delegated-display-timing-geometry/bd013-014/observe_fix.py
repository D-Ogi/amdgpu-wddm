from pathlib import Path
p=Path('bc250-win/driver/kmd/test/dcn_observe_test.c');s=p.read_text().replace('#define _In_\n#define _Inout_\n','').replace('#define C_ASSERT(x) _Static_assert(x,#x)','#define C_ASSERT(x) static_assert(x,#x)');p.write_text(s)
p=Path('bc250-win/driver/kmd/gen_regs.py');s=p.read_text().replace('["mmOTG0_OTG_STATUS_POSITION"]]','["mmOTG0_OTG_STATUS_POSITION", "mmOTG0_OTG_GLOBAL_CONTROL0"]]');p.write_text(s)
