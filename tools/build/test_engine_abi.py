"""The release and the lab engine DLL (BD-167) must never pass each other's build or run."""
import copy
import json
import unittest
from pathlib import Path

from engine_abi import PENDING, check_engine_dll, select_engine_abi

PIN = Path(__file__).resolve().parents[2] / 'driver' / 'umd' / 'd3d12' / 'engine-ddi' / 'engine-abi.json'
DIGEST = {'release': 'a' * 64, 'lab': 'b' * 64}


def pin(**flavour_fields):
    """A schema 2 pin with a different DLL per flavour, as engine-abi.json has."""
    return {'schema': 2, 'abi_version': '1.3', 'engine_dll': 'amdgpu_wddm_vkd3d.dll',
            'header_sha256': 'c' * 64, 'flavours': {
                name: dict({'engine_dll_dir': 'build/' + name, 'engine_dll_sha256': DIGEST[name],
                            'engine_dll_recipe': name + ' recipe'}, **flavour_fields.get(name, {}))
                for name in ('release', 'lab')}}


class EngineFlavourTests(unittest.TestCase):
    def test_each_flavour_keeps_its_own_dll_and_the_shared_abi(self):
        source = pin()
        original = copy.deepcopy(source)
        for flavour in ('release', 'lab'):
            selected = select_engine_abi(source, flavour)
            self.assertEqual(selected['flavour'], flavour)
            self.assertEqual(selected['engine_dll_sha256'], DIGEST[flavour])
            self.assertEqual(selected['engine_dll_dir'], 'build/' + flavour)
            self.assertEqual(selected['engine_dll_recipe'], flavour + ' recipe')
            self.assertEqual(selected['header_sha256'], 'c' * 64)
            self.assertNotIn('flavours', selected)
            # Selecting again changes nothing; selecting the other flavour from it is refused.
            self.assertEqual(select_engine_abi(selected, flavour), selected)
            with self.assertRaises(ValueError):
                select_engine_abi(selected, 'lab' if flavour == 'release' else 'release')
        self.assertEqual(source, original)

    def test_a_release_stage_refuses_the_lab_dll_and_the_lab_stage_the_release_dll(self):
        source = pin()
        for flavour, other in (('release', 'lab'), ('lab', 'release')):
            selected = select_engine_abi(source, flavour)
            self.assertEqual(check_engine_dll(selected, DIGEST[flavour].upper()), DIGEST[flavour])
            with self.assertRaises(ValueError):
                check_engine_dll(selected, DIGEST[other])
            with self.assertRaises(ValueError):
                check_engine_dll(selected, 'd' * 64)
            with self.assertRaises(ValueError):
                check_engine_dll(dict(selected, flavour=None), DIGEST[flavour])

    def test_a_flavour_with_no_dll_pinned_yet_selects_but_runs_nothing(self):
        source = pin(lab={'engine_dll_sha256': PENDING})
        self.assertEqual(select_engine_abi(source, 'lab')['engine_dll_sha256'], PENDING)
        with self.assertRaises(ValueError):
            check_engine_dll(select_engine_abi(source, 'lab'), DIGEST['lab'])
        # The other flavour stays usable while one is pending, and two pending pins are not a collision.
        self.assertEqual(check_engine_dll(select_engine_abi(source, 'release'), DIGEST['release']),
                         DIGEST['release'])
        both = pin(lab={'engine_dll_sha256': PENDING}, release={'engine_dll_sha256': PENDING})
        self.assertEqual(select_engine_abi(both, 'release')['engine_dll_sha256'], PENDING)

    def test_incomplete_shadowing_and_equal_pins_refuse(self):
        cases = {}
        cases['missing lab'] = {'schema': 2, 'flavours': {'release': pin()['flavours']['release']}}
        cases['third flavour'] = pin()
        cases['third flavour']['flavours']['debug'] = pin()['flavours']['lab']
        cases['other fields'] = pin(lab={'engine_dll_map': 'build/lab/map'})
        cases['equal digests'] = pin(lab={'engine_dll_sha256': DIGEST['release'].upper()})
        cases['not a digest'] = pin(lab={'engine_dll_sha256': 'e' * 63})
        cases['no digest'] = pin(lab={'engine_dll_sha256': None})
        cases['shadows a shared field'] = pin(release={'abi_version': '1.3'}, lab={'abi_version': '1.3'})
        cases['empty flavour'] = {'schema': 2, 'flavours': {'release': {}, 'lab': {}}}
        cases['no flavours'] = {'schema': 2, 'abi_version': '1.3'}
        for name, source in cases.items():
            for flavour in ('release', 'lab'):
                with self.subTest(case=name, flavour=flavour), self.assertRaises(ValueError):
                    select_engine_abi(source, flavour)
        with self.assertRaises(ValueError):
            select_engine_abi(pin(), 'debug')

    def test_a_pin_from_before_the_two_flavours_is_release_only(self):
        legacy = {'schema': 1, 'engine_dll_sha256': DIGEST['release']}
        self.assertEqual(select_engine_abi(legacy)['engine_dll_sha256'], DIGEST['release'])
        with self.assertRaises(ValueError):
            select_engine_abi(legacy, 'lab')

    def test_the_pinned_engine_abi_selects_both_flavours(self):
        source = json.loads(PIN.read_text(encoding='utf-8-sig'))
        dirs = set()
        for flavour in ('release', 'lab'):
            selected = select_engine_abi(source, flavour)
            self.assertEqual(selected['flavour'], flavour)
            self.assertTrue(selected['engine_dll_recipe'])
            self.assertEqual(selected['engine_dll'], 'amdgpu_wddm_vkd3d.dll')
            dirs.add(selected['engine_dll_dir'].casefold())
            digest = selected['engine_dll_sha256'].casefold()
            # A digest the build pins, or PENDING until that flavour's DLL is built. Equal digests are
            # refused by the selection itself, so the file cannot let one flavour's DLL pass the other's run.
            self.assertTrue(digest == PENDING or (len(digest) == 64 and not digest.strip('0123456789abcdef')))
        self.assertEqual(len(dirs), 2)


if __name__ == '__main__':
    unittest.main()
