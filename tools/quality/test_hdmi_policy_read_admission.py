"""The private HDMI policy must be observable without widening public writes."""
import importlib.util
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('hdmi_gen_regs', ROOT / 'driver/kmd/gen_regs.py')
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)
GENERATED = (ROOT / 'driver/kmd/regs.generated.h').read_text()


def table(name):
    body = re.search(r'\b' + name + r'\[[^\]]+\]\s*=\s*\{(.*?)\};', GENERATED, re.S)
    if body is None:
        raise AssertionError('Missing generated table ' + name)
    return {int(value, 16) for value in re.findall(r'0x[0-9A-Fa-f]+', body[1])}


class HdmiReadAdmission(unittest.TestCase):
    def test_policy_snapshot_is_readable_by_name_and_address(self):
        maps = {}
        allowed = table('g_MmioReadAllow')
        names = re.search(r'#define BC250_REG_READ_NAMES\(X\)(.*?) /\* end \*/', GENERATED, re.S)
        self.assertIsNotNone(names)
        self.assertEqual(len(generator.HDMI_POLICY_READS), 26)
        for name in generator.HDMI_POLICY_READS:
            with self.subTest(register=name):
                offset = generator.offset(maps, 'DMU', name)
                self.assertIn(offset, allowed)
                self.assertIn(f'X("DMU", "{name[2:]}", 0x{offset:05X}ul)', names[1])

    def test_policy_does_not_grant_public_register_writes(self):
        maps = {}
        writes = table('g_MmioWriteAllow')
        self.assertEqual(writes, {generator.offset(maps, 'GC', 'mmSCRATCH_REG0'),
                                 generator.offset(maps, 'GC', 'mmSCRATCH_REG1')})
        for name in generator.HDMI_POLICY_READS:
            self.assertNotIn(generator.offset(maps, 'DMU', name), writes)


if __name__ == '__main__':
    unittest.main()
