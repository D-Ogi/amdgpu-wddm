"""Keep the named wave decoder complete and tied to the imported GFX10 header."""
import re
from pathlib import Path
import unittest
import gen_wave_decode


class WaveDecodeTests(unittest.TestCase):
    def test_generated_table_current(self):
        self.assertEqual(gen_wave_decode.generate(), gen_wave_decode.OUTPUT.read_text(encoding="utf-8"))

    def test_all_abi_registers_have_generated_fields(self):
        abi = (gen_wave_decode.ROOT / "driver/kmd/wave_snapshot.h").read_text(encoding="utf-8")
        enum = abi.split("enum BC250_WAVE_REGISTER {", 1)[1].split("};", 1)[0]
        expected = re.findall(r"\bBc250Wave(\w+)", enum)
        self.assertEqual([name for name, _ in gen_wave_decode.REGISTERS], expected[:-1])
        self.assertEqual(expected[-1], "RegisterCount")
        for name, _ in gen_wave_decode.REGISTERS:
            self.assertIn("{ Bc250Wave" + name + ",", gen_wave_decode.generate())

    def test_decode_uses_source_macros(self):
        fields = gen_wave_decode.generate().split("static const BC250_WAVE_FIELD WaveFields[]", 1)[1]
        for line in fields.splitlines():
            if "Bc250Wave" in line:
                self.assertRegex(line, r"SQ_WAVE_\w+__\w+_MASK, SQ_WAVE_\w+__\w+__SHIFT")


if __name__ == "__main__":
    unittest.main()
