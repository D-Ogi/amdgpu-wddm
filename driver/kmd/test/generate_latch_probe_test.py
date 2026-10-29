"""Insert the actual lab probe; only replace its WDK include with the fixture."""
from pathlib import Path
import argparse
p = argparse.ArgumentParser()
p.add_argument('--out', type=Path, required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
source = (root / 'dcn_latch_probe.c').read_text()
needle = '#include "bc250kmd.h"'
assert source.count(needle) == 1
fixture = Path(__file__).with_name('dcn_latch_probe_test.c').read_text()
assert fixture.count('/* ACTUAL_PROBE */') == 1
a.out.write_text(fixture.replace('/* ACTUAL_PROBE */', source.replace(needle, '')), newline='\n')
