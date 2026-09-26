import bz2
import io
import json
import os
from pathlib import Path
import tarfile
import tempfile
import unittest
from compare_piglit import read_results, inventory, compare

REPO = Path(__file__).resolve().parents[2]


def row(status, subtests=None):
    return {'result': status, 'subtests': subtests or {}}


class CompareTests(unittest.TestCase):
    def test_complete(self):
        r={'a':row('pass'),'b':row('skip')}
        self.assertTrue(compare(set(r),r,r)['complete_matching_pass_or_skip'])

    def test_same_failure_not_accepted(self):
        r={'a':row('fail')}
        self.assertFalse(compare(set(r),r,r)['complete_matching_pass_or_skip'])

    def test_missing_unfinished_and_unexpected(self):
        w={'a':row('notrun'),'extra':row('pass')};l={'a':row('pass'),'b':row('skip')}
        r=compare({'a','b'},w,l)
        self.assertFalse(r['complete']['windows'])
        self.assertTrue(r['complete']['linux'])
        self.assertEqual(len(r['differences']),3)

    def test_subtests(self):
        w={'a':row('pass',{'part':'fail'})}
        self.assertFalse(compare({'a'},w,w)['complete_matching_pass_or_skip'])
        l={'a':row('pass',{'part':'skip'})}
        self.assertIn('different_result',compare({'a'},w,l)['differences'][0]['reasons'])

    def test_readers_and_duplicate_rejection(self):
        with tempfile.TemporaryDirectory(dir=os.environ.get('BC250_TEST_TMP')) as folder:
            p=Path(folder);r={'a':row('pass')}
            (p/'result.json.bz2').write_bytes(bz2.compress(json.dumps({'tests':r}).encode()))
            with tarfile.open(p/'partial.tar','w') as t:
                data=json.dumps(r).encode();m=tarfile.TarInfo('./tests/0.json');m.size=len(data);t.addfile(m,io.BytesIO(data))
            self.assertEqual(read_results(p/'result.json.bz2'),read_results(p/'partial.tar'))
            with tarfile.open(p/'partial.tar','a') as t:
                data=json.dumps(r).encode();m=tarfile.TarInfo('./tests/1.json');m.size=len(data);t.addfile(m,io.BytesIO(data))
            with self.assertRaisesRegex(ValueError,'Duplicate case'):read_results(p/'partial.tar')
            (p/'bad.json').write_text('{"tests":{"a":{},"a":{}}}')
            with self.assertRaisesRegex(ValueError,'Duplicate JSON key'):read_results(p/'bad.json')
            (p/'cases').write_text('a\na\n')
            with self.assertRaises(ValueError):inventory(p/'cases')

    def test_recorded_interrupted_run(self):
        r=read_results(REPO / 'evidence/windows/2026-09-26-E33-piglit-first-failure/run002/partial-results.tar')
        self.assertEqual(len(r),437)
        self.assertEqual(r['fast_color_clear@fcc-front-buffer-distraction']['result'],'fail')
        expected=inventory(REPO / 'evidence/windows/2026-09-26-E33-piglit-full-stage/cases.txt')
        report=compare(expected,r,r)
        self.assertFalse(report['complete']['windows'])
        self.assertEqual(sum('windows_missing' in d['reasons'] for d in report['differences']),44600)


if __name__=='__main__':unittest.main()
