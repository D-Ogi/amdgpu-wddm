"""BD-218 negative controls, including aliases, conditionals and C line splicing."""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import kmd_nx as gate


class KmdNx(unittest.TestCase):
    def test_each_forbidden_token(self):
        for token in gate.FORBIDDEN:
            with self.subTest(token=token):
                self.assertEqual(token, gate.findings(f'void f() {{ {token}; }}')[0]['token'])

    def test_nx_apis_and_flags_pass(self):
        self.assertEqual([], gate.findings('''
ExAllocatePool2(POOL_FLAG_NON_PAGED, bytes, tag);
ExAllocatePool2(POOL_FLAG_PAGED, bytes, tag);
MmAllocateContiguousNodeMemory(n,l,h,b,PAGE_READWRITE,MM_ANY_NODE_OK);
MmMapIoSpaceEx(p,n,PAGE_READONLY | PAGE_NOCACHE);
MmMapIoSpaceEx(p,n,PAGE_READWRITE | PAGE_WRITECOMBINE);
NonPagedPoolNx; NonPagedPoolNxCacheAligned;
'''))

    def test_comments_and_literals_are_not_requests(self):
        self.assertEqual([], gate.findings(r'''
// MmAllocateContiguousMemorySpecifyCache(...)
/* PAGE_EXECUTE_READWRITE */
const char *s = "PAGE_EXECUTE_READWRITE \\";
const char *t = "quoted \" NonPagedPool";
char c = '\'';
'''))

    def test_macro_alias_and_inactive_branch_are_checked(self):
        source = '#define allocate MmAllocateContiguousMemory\n#if LAB\nPAGE_EXECUTE_READWRITE\n#endif'
        self.assertEqual([1, 3], [h['line'] for h in gate.findings(source)])

    def test_line_splicing_and_physical_line(self):
        source = '\nMmAllocateContiguous\\\r\nMemory(4096);\nPAGE_EXECUTE;'
        self.assertEqual([2, 4], [h['line'] for h in gate.findings(source)])

    def test_spliced_line_comment_stays_comment(self):
        self.assertEqual([], gate.findings('// old API \\\nMmAllocateContiguousMemory();\n'))

    def test_longer_identifier_is_not_forbidden(self):
        self.assertEqual([], gate.findings('MyNonPagedPool; PAGE_EXECUTE_READWRITE_DESCRIPTION;'))

    def test_missing_tree_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            result = gate.check(Path(directory))
            self.assertEqual('FAIL', result['status'])
            self.assertTrue(any('missing required' in e for e in result['errors']))

    def test_incomplete_scan_cannot_pass(self):
        with patch.object(gate.os, 'walk', side_effect=PermissionError('unreadable source')):
            result = gate.check(Path(__file__).resolve().parents[2])
            self.assertEqual('FAIL', result['status'])
            self.assertTrue(any('incomplete source scan' in e for e in result['errors']))

    def test_test_directory_excluded_but_production_header_checked(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            base = root / 'driver/kmd'
            (base / 'test').mkdir(parents=True)
            for name in ('gart.c', 'gpumem.c'):
                (base / name).write_text('/* no allocation */', encoding='utf-8')
            (base / 'test/negative.c').write_text('PAGE_EXECUTE;', encoding='utf-8')
            self.assertEqual('PASS', gate.check(root)['status'])
            (base / 'helper.h').write_text('#define protection PAGE_EXECUTE_READWRITE', encoding='utf-8')
            result = gate.check(root)
            self.assertEqual('FAIL', result['status'])
            self.assertEqual(3, len(result['files']))
            self.assertIn('helper.h:1:', result['errors'][0])
            self.assertFalse(result['hvci_acceptance'])


if __name__ == '__main__':
    unittest.main()
