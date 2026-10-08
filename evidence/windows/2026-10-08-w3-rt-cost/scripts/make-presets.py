"""Per-effect RT presets for the W3 RT effect-cost series (2026-10-08): the HIGH preset (game-recon/presets/high.txt,
w3-preset.py) with only the [Rendering/RT] lines changed. graphics.xml: EnableRT is the master switch and RT global
illumination is on whenever it is (RTGIPreset false = the menu's performance mode, true = quality); EnableRtRadiance
= RT reflections; Shadows 0 off, 1 performance, 2 quality; RTAOEnabled = RT ambient occlusion; PTEnable = path tracer.
The game's own RT button (preset btn_rt) = GI performance + reflections + shadows performance, no AO = high-rt.txt.
Writes game-recon/presets/w3rt-<arm>.txt."""
from pathlib import Path

PRESETS = Path(__file__).resolve().parents[1] / 'm15' / 'game-recon' / 'presets'
RT_KEYS = ('EnableRT', 'RTGIPreset', 'EnableRtRadiance', 'Shadows', 'RTAOEnabled')
ARMS = {
    # name: (EnableRT, RTGIPreset, EnableRtRadiance, Shadows, RTAOEnabled)
    'w3rt-all': ('true', 'false', 'true', '1', 'true'),
    'w3rt-gi': ('true', 'false', 'false', '0', 'false'),
    'w3rt-refl': ('true', 'false', 'true', '0', 'false'),
    'w3rt-shadow': ('true', 'false', 'false', '1', 'false'),
    'w3rt-ao': ('true', 'false', 'false', '0', 'true'),
    'w3rt-off': ('false', 'false', 'false', '0', 'false'),
}


def main():
    base = (PRESETS / 'high.txt').read_text(encoding='ascii').splitlines()
    keep = [l for l in base if not (l.startswith('Rendering/RT|') and l.split('|', 1)[1].split('=', 1)[0] in RT_KEYS)]
    for name, values in ARMS.items():
        rt = [f'Rendering/RT|{k}={v}' for k, v in zip(RT_KEYS, values)] + ['Rendering/RT/PathTracer|PTEnable=false']
        (PRESETS / f'{name}.txt').write_text('\n'.join(keep + rt) + '\n', encoding='ascii')
        print(name, len(keep + rt), 'lines')


if __name__ == '__main__':
    main()
