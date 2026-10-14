"""Integration controls for cache invalidation and deployment artifact identity."""
import argparse
import contextlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch
from artifact_gate import sha, verify
from msvc_analysis import run

# Where the temporary trees of these tests go. BC250_TEST_OUT keeps them off drive C: of the
# development PC; without it tempfile picks the system temporary directory.
OUT = os.environ.get("BC250_TEST_OUT") or None


def find_cl():
    """cl.exe, or None when this computer has no MSVC.

    The gate under test takes the compiler out of a compile database, where the build has already
    written its full path (msvc_analysis.run: shutil.which(argv[0]) or argv[0]). A test that writes
    its own database has to find the compiler itself, and PATH alone does not hold it: cl.exe is on
    PATH only inside a Visual Studio developer shell, and the gate runner is an ordinary shell. So
    look the way this repository's build scripts look (driver/kmd/build.ps1 and the others): PATH
    first, then the newest toolset of the installation vswhere reports.
    """
    found = shutil.which('cl')
    if found:
        return found
    program_files = os.environ.get('ProgramFiles(x86)') or r'C:\Program Files (x86)'
    vswhere = Path(program_files) / 'Microsoft Visual Studio' / 'Installer' / 'vswhere.exe'
    if not vswhere.is_file():
        return None
    try:
        probe = subprocess.run([str(vswhere), '-latest', '-products', '*', '-property', 'installationPath'],
                               capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.SubprocessError):
        return None
    root = probe.stdout.strip().splitlines()
    tools = Path(root[0]) / 'VC' / 'Tools' / 'MSVC' if root else None
    if tools is None or not tools.is_dir():
        return None
    for version in sorted((p for p in tools.iterdir() if p.is_dir()), reverse=True):
        candidate = version / 'bin' / 'Hostx64' / 'x64' / 'cl.exe'
        if candidate.is_file():
            return str(candidate)
    return None


class QualityControls(unittest.TestCase):
    def test_artifact_identity(self):
        with tempfile.TemporaryDirectory(dir=OUT) as d:
            path=Path(d)/'driver.dll';path.write_bytes(b'tested')
            receipt={'status':'PASS','exit_code':0,'loaded_artifacts':{'driver.dll':sha(path)}}
            verify(receipt,{'driver.dll':path})
            path.write_bytes(b'rebuilt')
            with self.assertRaises(ValueError):verify(receipt,{'driver.dll':path})
            receipt['status']='FAIL'
            with self.assertRaises(ValueError):verify(receipt,{'driver.dll':path})

    @patch.dict(os.environ, {'BC250_GATE_NOCACHE': '0'})
    def test_analysis_cache_and_warning(self):
        cl = find_cl()
        if not cl:
            # Never an error: this control checks the analysis gate by running the compiler, and a
            # computer without MSVC cannot run it. Say which tool is missing and where it was sought.
            self.skipTest('no cl.exe on PATH and none under the Visual Studio installation vswhere reports: '
                          'this control runs the compiler it checks')
        with tempfile.TemporaryDirectory(dir=OUT) as d:
            root=Path(d);header=root/'value.h';source=root/'control.c'
            header.write_text('#define VALUE 1\n')
            source.write_text('#include "value.h"\nint f(int flag) { int value; if (flag) value = 1; return VALUE; }\n')
            db=root/'compile_commands.json';db.write_text(json.dumps([{'directory':str(root),'file':str(source),'arguments':[cl,'/nologo','/c','/W4',str(source)]}]))
            args=argparse.Namespace(database=db,match='control.c$',out=root/'results',timeout=30,force=False)
            def check():
                with contextlib.redirect_stdout(io.StringIO()):run(args)
                return json.loads((args.out/'summary.json').read_text())[0]['status']
            self.assertEqual(check(),'PASS')
            self.assertEqual(check(),'CACHED')
            # This is a fixture cache, even inside a forced release run. Check
            # the forced path too; no production gate inherits this override.
            with patch.dict(os.environ, {'BC250_GATE_NOCACHE': '1'}):
                self.assertEqual(check(),'PASS')
            header.write_text('#define VALUE 2\n')
            self.assertEqual(check(),'PASS')
            header.write_text('#define VALUE value\n')
            with self.assertRaises(RuntimeError):check()
            logs = b'\n'.join(p.read_bytes() for p in args.out.glob('*/analysis.log'))
            self.assertIn(b'C6001', logs)
            self.assertFalse(list(args.out.glob('*/pass.json')))


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',required=True,type=Path);a=p.parse_args()
    OUT=a.out.resolve();OUT.mkdir(parents=True,exist_ok=True)
    unittest.main(argv=['quality-controls'])
