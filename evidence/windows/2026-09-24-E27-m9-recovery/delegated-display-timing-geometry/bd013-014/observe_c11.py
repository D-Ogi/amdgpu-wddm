from pathlib import Path
p=Path('bc250-win/driver/kmd/test/dcn_observe_test.c');s=p.read_text().replace('#define C_ASSERT(x) static_assert(x,#x)','#define C_ASSERT(x) _Static_assert(x,#x)');p.write_text(s)
p=Path('bc250-win/driver/kmd/test/run_dcn_observe.ps1');s=p.read_text().replace('/TC /W4','/TC /std:c11 /W4');p.write_text(s)
