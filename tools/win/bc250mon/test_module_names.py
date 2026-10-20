"""The user-mode driver file names exist twice: in the driver package contract (driver/contract/amdgpu_driver_package.h,
the names the shells and the router load from the DriverStore, x64 and x86) and in the graphics panel
(tools/win/bc250mon/src/GraphicsApiProvider.cs), which reads them out of a process's module list to tell our GPU
path from WARP. On tester.31 the shells' ICD copies got new names and the panel kept the old one, so every D3D11
and D3D12 application read as "CPU (WARP)" or "no ICD yet". This test fails when the header names a file that the
panel does not know.

    python -m unittest discover -s tools/win/bc250mon
"""

import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
HEADER = os.path.join(HERE, "..", "..", "..", "driver", "contract", "amdgpu_driver_package.h")
PANEL = os.path.join(HERE, "src", "GraphicsApiProvider.cs")

# Each define of the header, by the panel list that has to hold its file name. The config record is no image.
ROLES = {
    "AMDGPU_CPU_UMD_FILE": "CpuUmds",
    "AMDGPU_HOSTED_UMD_FILE": "HostedUmds",
    "AMDGPU_APP_UMD_RELATIVE": "D3d11Shells",
    "AMDGPU_DXVK_FILE": "D3d11Engines",
    "AMDGPU_D3D11_ICD_FILE": "D3d11Icds",
    "AMDGPU_VKD3D_FILE": "D3d12Engines",
    "AMDGPU_D3D12_ICD_FILE": "D3d12Icds",
}
SKIP = {"AMDGPU_D3D11_CONFIG_FILE"}


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def header_names():
    """[(define, file name)] for both architecture branches of the header."""
    found = re.findall(r'#define\s+(AMDGPU_\w+)\s+L"([^"]+)"', read(HEADER))
    assert found, f"no AMDGPU_* file defines in {HEADER}"
    return [(d, v.replace("\\\\", "\\").split("\\")[-1].lower()) for d, v in found]


def panel_lists():
    """{list name: set of file names} from the static string arrays of GraphicsApiProvider."""
    lists = {}
    for name, body in re.findall(r"static readonly string\[\]\s+(\w+)\s*=\s*\{([^}]*)\}", read(PANEL)):
        lists[name] = {v.lower() for v in re.findall(r'"([^"]+)"', body)}
    return lists


class ModuleNames(unittest.TestCase):
    def test_every_header_name_is_known_to_the_panel(self):
        lists = panel_lists()
        missing = []
        for define, name in header_names():
            if define in SKIP:
                continue
            self.assertIn(define, ROLES, f"{define}: new define, give it a panel list in ROLES")
            if name not in lists.get(ROLES[define], set()):
                missing.append(f"{define} {name} not in {ROLES[define]}")
        self.assertEqual(missing, [])

    def test_both_architectures_are_read(self):
        self.assertEqual(len([d for d, _ in header_names() if d == "AMDGPU_D3D11_ICD_FILE"]), 2)


if __name__ == "__main__":
    unittest.main()
