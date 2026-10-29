"""Extract the actual admission handler; stubs count every core invocation."""
from pathlib import Path
import argparse
p = argparse.ArgumentParser()
p.add_argument('--out', type=Path, required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
source = (root / 'display.c').read_text()
start = source.index('static void LatchProbeEscape(')
end = source.index('\n}\n', start) + 3
fixture = Path(__file__).with_name('dcn_latch_escape_test.c').read_text()
assert fixture.count('/* ACTUAL_HANDLER */') == 1
a.out.write_text(fixture.replace('/* ACTUAL_HANDLER */', source[start:end]), newline='\n')
