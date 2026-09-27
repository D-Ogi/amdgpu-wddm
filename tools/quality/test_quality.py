"""Integration controls for cache invalidation and deployment artifact identity."""
import argparse
import contextlib
import io
import json
from pathlib import Path
import shutil
import tempfile
import unittest
from artifact_gate import sha, verify
from msvc_analysis import run


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

    def test_analysis_cache_and_warning(self):
        with tempfile.TemporaryDirectory(dir=OUT) as d:
            root=Path(d);header=root/'value.h';source=root/'control.c'
            header.write_text('#define VALUE 1\n')
            source.write_text('#include "value.h"\nint f(int flag) { int value; if (flag) value = 1; return VALUE; }\n')
            db=root/'compile_commands.json';db.write_text(json.dumps([{'directory':str(root),'file':str(source),'arguments':[shutil.which('cl'),'/nologo','/c','/W4',str(source)]}]))
            args=argparse.Namespace(database=db,match='control.c$',out=root/'results',timeout=30,force=False)
            def check():
                with contextlib.redirect_stdout(io.StringIO()):run(args)
                return json.loads((args.out/'summary.json').read_text())[0]['status']
            self.assertEqual(check(),'PASS')
            self.assertEqual(check(),'CACHED')
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
