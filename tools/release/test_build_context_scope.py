"""Selected recipe context fixtures; never visit the installed toolchain."""
import json
import os
from pathlib import Path
import tempfile
import stat
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import build_context as c

class ScopeTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(dir=os.environ.get('BC250_TEST_OUT'))
        self.root=Path(self.tmp.name);self.repo=self.root/'repo'
        for name in c.RUNTIME_HELPERS:self.write(self.repo/name,b'helper')
        self.write(self.root/'toolchain/selected/cc.exe',b'compiler')
        self.write(self.root/'toolchain/sdk/header.h',b'header')
        self.write(self.root/'ref/selected/contract.h',b'contract')
        self.row={'id':'a','repo':'recipes','commit':'a'*40,'recipe':'build.ps1','args':[]}
        self.manifest={'recipe_repository':'recipes','recipe_commit':'a'*40,'rows':[self.row]}
        self.decl={'schema':1,'recipes':[{'recipe':'build.ps1','commit':'a'*40,'inputs':[
            {'path':'{root}/toolchain/selected','parameter':'CompilerPath'},
            {'path':'{root}/toolchain/sdk','parameter':'Kits'},
            {'path':'{root}/ref/selected'}]}]}
    def tearDown(self):self.tmp.cleanup()
    def write(self,path,data):path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data)
    def capture(self,**kw):
        options=dict(environ={},executables={},discover_vs=False,manifest=self.manifest,declarations=self.decl)
        options.update(kw)
        return c.capture_context(self.root,self.repo,**options)
    def test_actual_selected_compiler_sdk_reference_each_invalidates(self):
        for path in ('toolchain/selected/cc.exe','toolchain/sdk/header.h','ref/selected/contract.h'):
            before=self.capture();self.write(self.root/path,b'changed-'+path.encode())
            self.assertNotEqual(before['fingerprint'],self.capture()['fingerprint'])
    def closure_fixture(self):
        self.decl['recipes'][0]['reference_closures']=[dict(path='{root}/ref/closure',
            include_dirs=['include'],sources=['main.c'],external_or_generated=['stdio.h'],
            generators=['gen.py'],external_python=['sys'],data=['table.json'])]
        for name,data in {'main.c':b'#include "one.h"\n#include <stdio.h>\n',
                          'include/one.h':b'int one;', 'gen.py':b'import helper\nimport sys\n',
                          'helper.py':b'value=1\n','table.json':b'{}'}.items():
            self.write(self.root/'ref/closure'/name,data)
        return self.root/'ref/closure'
    def test_closure_tracks_headers_generator_imports_and_data(self):
        base=self.closure_fixture()
        for name in ['include/one.h','helper.py','table.json']:
            before=self.capture();self.assertTrue(before['complete'],before['incomplete_inputs'])
            path=base/name;path.write_bytes(path.read_bytes()+b'\n')
            self.assertNotEqual(before['fingerprint'],self.capture()['fingerprint'])
        (base/'include/one.h').write_text('#include "two.h"\n')
        self.write(base/'include/two.h',b'int two;')
        before=self.capture();self.assertTrue(before['complete'])
        self.write(base/'include/two.h',b'int changed;')
        self.assertNotEqual(before['fingerprint'],self.capture()['fingerprint'])
    def test_closure_ignores_unconsumed_files(self):
        base=self.closure_fixture();before=self.capture()
        self.write(base/'unrelated/README.rst',b'unrelated')
        self.assertEqual(before,self.capture())
    def test_closure_unknown_include_macro_import_and_missing_refuse(self):
        base=self.closure_fixture()
        for text in ['#include "missing.h"\n','#include MACRO\n']:
            (base/'main.c').write_text(text)
            result=self.capture();self.assertFalse(result['complete'])
            self.assertEqual(result['incomplete_inputs'][0]['state'],'unresolved-reference-closure')
        (base/'main.c').write_text('#include "one.h"\n')
        (base/'gen.py').write_text('import unreviewed\n')
        self.assertFalse(self.capture()['complete'])
        (base/'gen.py').write_text('import sys\n');(base/'table.json').unlink()
        self.assertFalse(self.capture()['complete'])
    def test_closure_required_unreadable_refuses(self):
        base=self.closure_fixture();original=Path.read_bytes
        def read(path):
            if path==base/'include/one.h':raise PermissionError('required denied')
            return original(path)
        with patch.object(Path,'read_bytes',read):self.assertFalse(self.capture()['complete'])
    def test_closure_required_link_and_linked_ancestor_refuse(self):
        base=self.closure_fixture();original=Path.lstat
        declaration=self.decl['recipes'][0]['reference_closures'][0]
        for linked in [base/'include/one.h',base/'include']:
            def lstat(path,*a,**kw):
                if path==linked:return SimpleNamespace(st_mode=stat.S_IFLNK,st_file_attributes=0x400)
                return original(path,*a,**kw)
            with patch.object(Path,'lstat',lstat),self.assertRaisesRegex(ValueError,'linked'):
                c.discover_reference_closure(base,declaration,{})
    def test_closure_discovery_hash_cannot_be_raced(self):
        base=self.closure_fixture();original=c.discover_reference_closure
        def discover(*a,**kw):
            result=original(*a,**kw)
            (base/'include/one.h').write_text('int changed_after_discovery;')
            return result
        with patch.object(c,'discover_reference_closure',discover):result=self.capture()
        self.assertFalse(result['complete'])
        self.assertTrue(any(p['state']=='reference-changed-during-capture' for p in result['incomplete_inputs']))
    def test_unrelated_old_llvm_and_smartplug_do_not_change_or_get_read(self):
        before=self.capture()
        for path in ('toolchain/old-llvm/cc.exe','toolchain/smartplug-python/secret.py'):
            self.write(self.root/path,b'unrelated')
        original=Path.open
        def opened(path,*a,**k):
            if 'old-llvm' in str(path) or 'smartplug' in str(path):raise AssertionError('unrelated input touched')
            return original(path,*a,**k)
        with patch.object(Path,'open',opened):self.assertEqual(before,self.capture())
    def test_selected_unreadable_refuses(self):
        original=Path.open
        def opened(path,*a,**k):
            if path==self.root/'toolchain/sdk/header.h':raise PermissionError('required denied')
            return original(path,*a,**k)
        with patch.object(Path,'open',opened):result=self.capture()
        self.assertFalse(result['complete']);self.assertTrue(any(x['state']=='unreadable' for x in result['incomplete_inputs']))
    def test_selected_override_is_hashed_default_not_selected(self):
        self.write(self.root/'alternate/cc.exe',b'alternate')
        self.row['args']=['-CompilerPath',str(self.root/'alternate')]
        first=self.capture();self.write(self.root/'toolchain/selected/cc.exe',b'unused change')
        self.assertEqual(first,self.capture())
        self.write(self.root/'alternate/cc.exe',b'new alternate')
        self.assertNotEqual(first['fingerprint'],self.capture()['fingerprint'])
    def test_unknown_revision_refuses_before_any_fingerprinting(self):
        self.row['commit']='b'*40
        with patch.object(c.Fingerprinter,'capture',side_effect=AssertionError('must not read inputs')):
            result=self.capture()
        self.assertFalse(result['complete']);self.assertEqual(result['incomplete_inputs'][0]['state'],'recipe-inputs-not-audited')
    def test_tool_placeholder_explicit_mapping_is_consumed(self):
        self.row['args']=['-Sdk','{tool:kits}']
        self.write(self.root/'custom/header.h',b'custom')
        first=self.capture(tools={'kits':str(self.root/'custom')})
        self.write(self.root/'custom/header.h',b'changed')
        self.assertNotEqual(first['fingerprint'],self.capture(tools={'kits':str(self.root/'custom')})['fingerprint'])
    def test_root_output_argument_not_input(self):
        self.row['args']=['-Out','{root}/scratch/output']
        first=self.capture();self.write(self.root/'scratch/output/result.bin',b'output')
        self.assertEqual(first,self.capture())
    def test_sdk_bundle_does_not_hash_unselected_package(self):
        self.decl['recipes'][0]['inputs'][1]['children']=['selected']
        self.write(self.root/'toolchain/sdk/selected/header.h',b'header')
        self.row['args']=['-Kits','{root}/toolchain/sdk']
        before=self.capture();self.write(self.root/'toolchain/sdk/old-package/blob',b'unrelated')
        self.assertEqual(before,self.capture())
        self.write(self.root/'toolchain/sdk/selected/header.h',b'changed')
        self.assertNotEqual(before['fingerprint'],self.capture()['fingerprint'])
    def test_sdk_environment_override_uses_selected_bundle(self):
        self.decl['recipes'][0]['inputs'][1]['children']=['selected']
        self.row['args']=['-Kits','{tool:kits}']
        self.write(self.root/'custom-kits/selected/header.h',b'header')
        env={'BC250_TOOL_KITS':str(self.root/'custom-kits')}
        before=self.capture(environ=env)
        bundle=before['inputs']['environment_inputs']['BC250_TOOL_KITS'][0]
        self.assertEqual(bundle['state'],'selected-bundle')
        self.write(self.root/'custom-kits/unrelated-package/file',b'unrelated')
        self.assertEqual(before,self.capture(environ=env))
        self.write(self.root/'custom-kits/selected/header.h',b'changed')
        self.assertNotEqual(before['fingerprint'],self.capture(environ=env)['fingerprint'])
    def test_firmware_directory_override_selects_same_named_members(self):
        self.decl['recipes'][0]['inputs']=[{'path':'{root}/firmware/one.bin','parameter':'FirmwareDir','override_append':'one.bin'}]
        self.write(self.root/'alternate-fw/one.bin',b'firmware')
        self.row['args']=['-FirmwareDir',str(self.root/'alternate-fw')]
        before=self.capture();self.assertTrue(before['complete'])
        self.write(self.root/'alternate-fw/one.bin',b'changed')
        self.assertNotEqual(before['fingerprint'],self.capture()['fingerprint'])
    def test_new_revision_requires_equal_audited_helper_bindings(self):
        self.row['commit']='b'*40
        self.decl['recipes'][0]['bindings']={'build.ps1':'c'*40,'helpers':'d'*40}
        from types import SimpleNamespace
        with patch.object(c.subprocess,'run',return_value=SimpleNamespace(returncode=0,stdout='c'*40+'\n'+'d'*40+'\n')):
            self.assertTrue(self.capture()['complete'])
        with patch.object(c.subprocess,'run',return_value=SimpleNamespace(returncode=0,stdout='c'*40+'\n'+'e'*40+'\n')):
            self.assertFalse(self.capture()['complete'])

if __name__=='__main__':unittest.main(verbosity=2)
