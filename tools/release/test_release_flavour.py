"""Release and lab packages must never borrow each other's payload pins."""
import copy
import unittest
from release_flavour import select_sources


class FlavourTests(unittest.TestCase):
    def source(self):
        return {'schema': 4, 'files': [
            {'path': path, 'flavour': flavour, 'sha256': flavour + path, 'source': flavour + '/' + path}
            for path in ('payload/kmd/driver.sys', 'payload/d3d11/driver.config', 'payload/control/app.exe')
            for flavour in ('release', 'lab')
        ] + [{'path': 'licenses/notice.txt', 'flavour': 'shared', 'source': 'notice.txt'}]}

    def test_each_flavour_keeps_exact_pins_and_shared_data(self):
        sources = self.source()
        original = copy.deepcopy(sources)
        for flavour in ('release', 'lab'):
            result = select_sources(sources, flavour)
            self.assertEqual(result['flavour'], flavour)
            self.assertEqual(len(result['files']), 4)
            for row in result['files'][:3]:
                self.assertEqual(row['sha256'], flavour + row['path'])
                self.assertEqual(row['source'], flavour + '/' + row['path'])
            self.assertEqual(select_sources(result, flavour), result)
            with self.assertRaises(ValueError):
                select_sources(result, 'lab' if flavour == 'release' else 'release')
        self.assertEqual(sources, original)

    def test_partial_duplicate_unlabelled_and_shared_executable_sets_refuse(self):
        for change in ('missing', 'duplicate', 'unlabelled', 'shared', 'unknown', 'overlap'):
            sources = self.source()
            if change == 'missing':
                del sources['files'][1]
            elif change == 'duplicate':
                sources['files'].append(copy.deepcopy(sources['files'][1]))
            elif change == 'unlabelled':
                del sources['files'][1]['flavour']
            elif change == 'shared':
                sources['files'][1]['flavour'] = 'shared'
            elif change == 'unknown':
                sources['files'][1]['flavour'] = 'debug'
            else:
                sources['files'].append({'path': 'LICENSES/NOTICE.TXT', 'flavour': 'lab'})
            with self.subTest(change=change), self.assertRaises(ValueError):
                select_sources(sources, 'lab')

    def test_legacy_pins_are_release_only(self):
        sources = {'schema': 3, 'files': [{'path': 'payload/driver.dll', 'sha256': 'old-pin'}]}
        self.assertEqual(select_sources(sources)['files'], sources['files'])
        with self.assertRaises(ValueError):
            select_sources(sources, 'lab')


if __name__ == '__main__':
    unittest.main()
