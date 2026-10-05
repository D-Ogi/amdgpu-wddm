import importlib.util
import io
import json
import os
import tempfile
import unittest
from contextlib import redirect_stdout

_spec = importlib.util.spec_from_file_location('diff_caps', os.path.join(os.path.dirname(__file__), 'diff-caps.py'))
diff_caps = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(diff_caps)


class DiffCapsTest(unittest.TestCase):
    def test_leaves_sorted_with_absent_and_flags(self):
        left = {'b': {'x': 1, 'gone': True}, 'a': {'Support1Names': 'A|B'}, 'same': {'v': 'k'}}
        right = {'b': {'x': 2, 'new': None}, 'a': {'Support1Names': 'B|C'}, 'same': {'v': 'k'}}
        self.assertEqual(diff_caps.diff(left, right), [
            'a.Support1Names: "A|B" -> "B|C"  (+C -A)',
            'b.gone: true -> <absent>',
            'b.new: <absent> -> null',
            'b.x: 1 -> 2',
        ])

    def test_bool_is_not_int(self):
        self.assertEqual(diff_caps.diff({'v': True}, {'v': 1}), ['v: true -> 1'])

    def test_ignore_and_empty_object(self):
        import re
        self.assertEqual(diff_caps.diff({'m': {'Budget': 1}, 'e': {}}, {'m': {'Budget': 2}, 'e': {}},
                                        [re.compile(r'Budget$')]), [])

    def test_main_counts_and_never_fails(self):
        with tempfile.TemporaryDirectory() as d:
            p1, p2 = os.path.join(d, 'l.json'), os.path.join(d, 'r.json')
            for p, v in ((p1, {'x': 1}), (p2, {'x': 2})):
                with open(p, 'w', encoding='utf-8') as f:
                    json.dump(v, f)
            out = io.StringIO()
            with redirect_stdout(out):
                self.assertEqual(diff_caps.main([p1, p2]), 0)
                self.assertEqual(diff_caps.main([p1, os.path.join(d, 'missing.json')]), 0)
            lines = out.getvalue().splitlines()
            self.assertEqual(lines[:2], ['x: 1 -> 2', 'differences: 1'])
            self.assertTrue(lines[2].startswith('differences: not computed'))


if __name__ == '__main__':
    unittest.main()
