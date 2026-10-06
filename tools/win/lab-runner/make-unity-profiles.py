"""Writes runner profiles for the small Unity games installed on the lab on 2026-10-04 (owner: smaller offline
D3D11/D3D12 games from the owner's library). Unity selects the graphics API with the player command line
-force-d3d12 / -force-d3d11 (Unity manual, Command line arguments). Executables from game-exes.ps1."""
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent / 'profiles'
D3D11 = 'release'  # the installed release's 64-bit shell, resolved by run.py from lab-baseline.json (since 415; was b12 AD750E51)
D3D11_X86 = '69AFE349D430E63029959DBE0D7C805F1D9AC5C7AB0C0A179DB91BF3F89430C6'  # b12 x86 shell (BD-064); not the gate witness: the runner gate reads the 64-bit GpuUmdPath (357)
X86 = {'poi', 'south-of-the-circle'}
GAMES = [
    # id, title, app id, install dir, exe stem, default api, note
    ('godstrike', 'Godstrike', 1476170, 'Godstrike', 'Godstrike', 'd3d11', 'Unity; boot.config hdr-display-enabled=1; default start takes D3D11 (351)'),
    ('poi', 'Poi', 401810, 'Poi', 'Poi', 'd3d11', 'Unity 5 era single executable (no UnityPlayer.dll); store lists DX10/DX12'),
    ('stygian', 'Stygian: Reign of the Old Ones', 779290, 'Stygian Reign of the Old Ones', 'STYGIAN', 'd3d11', 'Unity 2017.4; default start takes D3D11 (353)'),
    ('south-of-the-circle', 'South of the Circle', 1811040, 'South of the Circle', 'SouthOfTheCircle', 'd3d11', 'Unity; store lists DX11'),
]
MODULES = ['d3d11.dll', 'dxgi.dll', 'd3d12.dll', 'd3d12core.dll', 'bc250d3d_router.dll', 'amdgpu_wddm_d3d11.dll',
           'amdgpu_wddm_d3d12.dll', 'amdgpu_wddm_dxvk.dll', 'amdgpu_wddm_vkd3d.dll', 'amdgpu_wddm_radv.dll',
           'bc250d3d.dll', 'bc250umd.dll', 'vulkan-1.dll', 'unityplayer.dll']

for gid, title, app, folder, stem, api, note in GAMES:
    p = {
        'schema': 1, 'id': gid, 'title': title, 'app_id': app, 'api': api,
        'apis': {
            'd3d12': {'selection': 'game default, no switch (owner 2026-10-04); -force-d3d12 would force it', 'arguments': '',
                      'router': 'unchecked', 'witness': {'module': 'amdgpu_wddm_d3d12.dll', 'sha256': 'staged'}},
            'd3d11': {'selection': 'game default, no switch (owner 2026-10-04); -force-d3d11 would force it', 'arguments': '',
                      'router': 'allow', 'witness': {'module': 'amdgpu_wddm_d3d11.dll', 'sha256': D3D11}},
        },
        'steam': {'client': 'C:\\Program Files (x86)\\Steam\\steam.exe',
                  'library': 'C:\\Program Files (x86)\\Steam\\steamapps', 'install_dir': folder,
                  'launch_entry': stem + '.exe (installed 2026-10-04)', 'arguments': '',
                  'require_state_flags': 4, 'wait_seconds': 90, 'eula': ''},
        'executable': {'directory': '', 'image': stem + '.exe', 'direct_arguments': ''},
        'processes': {'main': stem, 'launch_chain': [stem], 'renderers': [stem],
                      'end_at_cleanup': [stem, 'UnityCrashHandler64', 'cdb'], 'survivors': [stem, 'UnityCrashHandler64', 'cdb'],
                      'writers': [stem, 'cdb'], 'counters': [stem], 'etw': [stem]},
        'router_images': [stem + '.exe'],
        'readiness': {'mode': 'generic', 'control_after_window_seconds': 10, 'min_jobs': 50},
        'modules': MODULES,
        'settings': {'policy': 'untouched'},
        'saves': '',
        'events_filter': stem + '|amdgpu_wddm|bc250d3d|d3d11|d3d12|dxgi|Unity',
        'app_profile': {'image': '', 'experiment': '', 'proposal': 'none: first sessions without D3D12 experiments'},
        'session': {'default_seconds': 600, 'entry': 'run-game.sh N %s [SECONDS]' % gid},
        'notes': [note, 'Unity writes Player.log under %USERPROFILE%\\AppData\\LocalLow\\<company>\\<product>.'],
    }
    (HERE / (gid + '.json')).write_text(json.dumps(p, indent=2) + '\n', encoding='ascii', newline='\n')
    print('wrote', gid)
