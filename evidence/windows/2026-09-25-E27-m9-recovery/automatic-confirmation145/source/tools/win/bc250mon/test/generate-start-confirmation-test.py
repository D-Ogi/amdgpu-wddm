"""Compile actual monitor policy and extract provider methods with fake boundaries."""
from pathlib import Path
import argparse
p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);a=p.parse_args()
s=(Path(__file__).parents[1]/'src/KmdProvider.cs').read_text();parts=[]
for signature in ['        void ResetAutoConfirmation()', '        string Confirm(State state, KmdSnapshot s, int starts)', '        string ConfirmDisplayOnly(State state, KmdSnapshot s, int starts)']:
    start=s.index(signature);end=s.index('\n        }\n',start)+len('\n        }\n');parts.append(s[start:end])
actual='\n'.join(parts).replace('Driver.','FakeDriver.').replace('Environment.TickCount','FakeEnvironment.TickCount')
f=Path(__file__).with_name('StartConfirmationTest.cs').read_text();a.out.write_text(f.replace('// ACTUAL_PROVIDER_METHODS',actual))
