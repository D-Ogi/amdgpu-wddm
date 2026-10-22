"""Controls for the driver-state-settings gate (check_settings.py): every call form the kernel source uses is
read, a declaration is recognised positively, and an unresolved call fails instead of being skipped."""
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
GATE = HERE / 'check_settings.py'

CATALOG = {'values': [
    {'path': '', 'name': 'Known', 'kind': 'setting', 'type': 'dword', 'code': 1},
    {'path': '', 'name': 'Other', 'kind': 'setting', 'type': 'dword', 'code': 0},
    {'path': '', 'name': 'Qword', 'kind': 'setting', 'type': 'qword', 'code': 0},
]}


def run_gate(source, header=''):
    """A throwaway tree with one catalog, one kernel source file and one header; returns (exit, output)."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / 'driver/state').mkdir(parents=True)
        (root / 'driver/kmd/sub').mkdir(parents=True)
        (root / 'driver/kmd/test').mkdir(parents=True)
        (root / 'driver/state/catalog.json').write_text(json.dumps(CATALOG), encoding='utf-8')
        (root / 'driver/kmd/bc250kmd.h').write_text(
            'ULONG GuardReadSetting(_In_z_ PCWSTR Name, ULONG Default);\n'
            'BOOLEAN GuardConsumeSetting (_In_z_ PCWSTR Name, ULONG Default);\n' + header, encoding='utf-8')
        (root / 'driver/kmd/sub/probe.c').write_text(source, encoding='utf-8')
        # a host fixture mock must not count: test/ is excluded
        (root / 'driver/kmd/test/mock.c').write_text('static ULONG GuardReadSetting(const wchar_t* n, ULONG d){return 1;}\n'
                                                      'void t(void){ GuardReadSetting(L"NotInCatalog", 0); }\n', encoding='utf-8')
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')
        r = subprocess.run([sys.executable, '-I', str(GATE), '--root', str(root)], capture_output=True, text=True, env=env)
        return r.returncode, r.stdout + r.stderr


class CheckSettingsTests(unittest.TestCase):
    def test_every_call_form_is_read(self):
        code, out = run_gate(
            '#define NAME_MACRO L"Known"\n'
            '#define DEFAULT_MACRO (1u)\n'
            'ULONG GuardReadSetting(_In_z_ PCWSTR Name, ULONG Default)\n{\n    return Default;\n}\n'
            'static void f(void) {\n'
            '    ULONG a = GuardReadSetting(L"Known", 1);\n'
            '    ULONG b = GuardReadSetting (L"Known", (1));\n'
            '    ULONG c = GuardReadSetting(NAME_MACRO, 0x1);\n'
            '    ULONG d = GuardReadSetting(L"Known", DEFAULT_MACRO);\n'
            '    ULONG e = GuardReadSetting(L"Known", 01);\n'
            '    ULONG g = GuardReadSetting(L"Known", a + 1);\n'
            '    if (GuardConsumeSetting(L"Other", FALSE)) return;\n'
            '    // GuardReadSetting(L"Commented", 0) and /* GuardReadSetting(L"AlsoCommented", 0) */\n'
            '    GuardReadSetting(NAME_MACRO, 1);\n'
            '}\n')
        self.assertEqual(code, 0, out)
        self.assertIn('8 reads match the catalog (1 with a computed default), 0 failures', out)

    def test_assignment_through_a_name_macro_is_checked(self):
        # the BD-190 class through a macro: the review found this form skipped as a declaration
        code, out = run_gate('#define NAME_MACRO L"NotInCatalog"\nstatic ULONG base;\n'
                             'static void f(void) {\n    base = GuardReadSetting(NAME_MACRO, 0);\n}\n')
        self.assertEqual(code, 1, out)
        self.assertIn('NotInCatalog is not a root DWORD entry', out)

    def test_unknown_name_and_default_mismatch_fail(self):
        code, out = run_gate('static void f(void) {\n    GuardReadSetting(L"NotInCatalog", 0);\n'
                             '    GuardReadSetting(L"Known", 0);\n    GuardReadSetting(L"Known", 0x2);\n'
                             '    GuardReadSetting(L"Qword", 0);\n}\n')
        self.assertEqual(code, 1, out)
        self.assertIn('NotInCatalog is not a root DWORD entry', out)
        self.assertIn('Known default 0 in the call, 1 in the catalog', out)
        self.assertIn('Known default 2 in the call, 1 in the catalog', out)
        self.assertIn('Qword is not a root DWORD entry', out)
        self.assertIn('4 failures', out)

    def test_unresolved_call_is_a_failure_not_a_skip(self):
        for source in ('static void f(PCWSTR name) {\n    ULONG v = GuardReadSetting(name, 0);\n}\n',
                       'static void f(void) {\n    ULONG v = GuardReadSetting(L"Known");\n}\n',
                       'static void f(void) {\n    ULONG (*fn)(PCWSTR, ULONG) = GuardReadSetting;\n}\n'):
            code, out = run_gate(source)
            self.assertEqual(code, 1, out)
            self.assertRegex(out, r'call not understood|used without a call')


if __name__ == '__main__':
    unittest.main()
