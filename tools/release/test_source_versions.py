"""Resource preflight controls, using synthetic PE files without executing them."""
import importlib.util
import tempfile
import unittest
from pathlib import Path

import check_source_versions as gate

REPO = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('stamp_fixtures', REPO/'tools/build/test_versionstamp.py')
fixtures = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixtures)
vs = fixtures.vs
VERSION = '0.7.216.105-tester.33'


class SourceVersions(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.path = self.root/'d3d11bench.exe'
        self.path.write_bytes(fixtures.make_pe())
        self.sources = {'files':[{'path':'payload/tools/d3d11bench.exe','source':'d3d11bench.exe',
                                  'built_from':{'commit':'fixture'}}]}

    def stamp(self, version=VERSION, flavour='release'):
        vs.stamp_file(self.path, version, vs.inf_provider(REPO/'driver/kmd/bc250kmd.inf'),
                      vs.load_table(REPO/'tools/build/versioninfo.json'), flavour=flavour)

    def check(self):
        return gate.check(self.sources,self.root,REPO,VERSION,'release')

    def test_current_resource(self):
        self.stamp()
        self.assertEqual(self.check(),(1,[]))

    def test_prior_release(self):
        self.stamp('0.7.216.104-tester.32')
        self.assertTrue(self.check()[1])

    def test_wrong_flavour(self):
        self.stamp(flavour='lab')
        self.assertTrue(self.check()[1])

    def test_missing_resource(self):
        self.assertTrue(self.check()[1])

    def test_missing_file(self):
        self.path.unlink()
        self.assertIn('source missing',self.check()[1][0])

    def test_empty_first_party_set(self):
        self.sources['files'][0]['third_party'] = {'name':'fixture'}
        self.assertEqual(self.check(),(0,['No first-party PE resources were checked']))


if __name__ == '__main__':
    unittest.main()
