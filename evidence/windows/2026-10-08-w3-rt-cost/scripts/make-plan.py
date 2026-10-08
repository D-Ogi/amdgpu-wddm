"""Writes plans/plan-N.md for one arm of the W3 RT effect-cost series (method in plan-469.md).
Usage: python make-plan.py N ARM PRESET [PERFTEST]"""
import sys
from pathlib import Path

PLANS = Path(__file__).resolve().parents[1] / 'm15' / 'native-caps001' / 'plans'
n, arm, preset = sys.argv[1], sys.argv[2], sys.argv[3]
perftest = sys.argv[4] if len(sys.argv) > 4 else ''
knob = f' with RADV_PERFTEST={perftest} (marker C:\\BC250\\tools\\radv-perftest.txt, cleared after the session)' if perftest else ''
text = f"""# Trial {n}: Witcher 3 HIGH, W3 RT effect-cost series arm {arm}: preset {preset}{knob}

`ARM_LABEL="{arm}" bash scratch/w3-rt-cost/run-arm.sh {n} {preset}{(' ' + perftest) if perftest else ''}` - method, lab state, conjecture C74, kill and
safety rules as plan-469.md (same location, drive-still input, window B 90 s GPU only, 1920x1080 exclusive fullscreen,
FXAA, no upscaler/FG/DRS, VSync off, LimitFPS 240, direct start, no experiment, overlay summary poll paused).
Expected closure: recovery-unverified (Verify "Confirmed CPU baseline required": the game's 1920x1080 mode commit moves
the KMD epoch, as 466/469); the runner re-confirms the start and checks the stack identities by hand.
"""
(PLANS / f'plan-{n}.md').write_text(text, encoding='utf-8')
print('wrote', PLANS / f'plan-{n}.md')
