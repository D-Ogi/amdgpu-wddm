"""Host tests for amd_isa_xml.py over a small file in the schema of AMD's machine-readable ISA.

python -m unittest discover -s tools/isa
"""
import io
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import amd_isa_xml  # noqa: E402

FIXTURE = """<?xml version="1.0"?>
<Spec><ISA><Instructions>
 <Instruction><InstructionName>V_MOV_B32</InstructionName><Description>Move data.</Description>
  <InstructionEncodings><InstructionEncoding><EncodingName>ENC_VOP1</EncodingName>
   <Opcode Radix="10">1</Opcode><Operands/></InstructionEncoding></InstructionEncodings></Instruction>
 <Instruction><InstructionName>IMAGE_LOAD_PCK2</InstructionName>
  <Description>Load 2 horizontal   elements.</Description>
  <InstructionEncodings><InstructionEncoding><EncodingName>ENC_MIMG</EncodingName>
   <Opcode Radix="10">112</Opcode><Operands>
    <Operand Input="false" Output="true" IsImplicit="false" Order="1"><FieldName>VDATA</FieldName>
     <OperandSize>128</OperandSize></Operand>
    <Operand Input="true" Output="false" IsImplicit="false" Order="2"><FieldName>SRSRC</FieldName>
     <OperandSize>256</OperandSize></Operand>
   </Operands></InstructionEncoding></InstructionEncodings></Instruction>
</Instructions></ISA></Spec>
"""


class AmdIsaXmlTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.path = os.path.join(self.dir, "amdgpu_isa_test.xml")
        with open(self.path, "w", encoding="utf-8") as f:
            f.write(FIXTURE)

    def run_main(self, *args):
        out = io.StringIO()
        with redirect_stdout(out):
            rc = amd_isa_xml.main(["amd_isa_xml.py"] + list(args))
        return rc, out.getvalue()

    def test_slots_names_hex_opcode(self):
        rc, out = self.run_main("slots", self.dir, "ENC_MIMG", "0x70", "0x71")
        self.assertEqual(rc, 0)
        self.assertIn("'0x70': ['IMAGE_LOAD_PCK2']", out)
        self.assertNotIn("0x71", out)

    def test_describe_collapses_whitespace_and_lists_operands(self):
        rc, out = self.run_main("describe", self.path, "IMAGE_LOAD_PCK2")
        self.assertEqual(rc, 0)
        self.assertIn("Load 2 horizontal elements.", out)
        self.assertIn("ENC_MIMG, opcode 112 (0x70): VDATA out 128 bits, SRSRC in 256 bits", out)

    def test_describe_unknown_name_fails(self):
        rc, out = self.run_main("describe", self.path, "IMAGE_LOAD_PCK9")
        self.assertEqual(rc, 1)
        self.assertIn("not in this file", out)

    def test_control_failure_is_loud(self):
        with open(self.path, "w", encoding="utf-8") as f:
            f.write(FIXTURE.replace('<Opcode Radix="10">1</Opcode>', '<Opcode Radix="10">2</Opcode>'))
        rc, _ = self.run_main("slots", self.dir, "ENC_MIMG", "0x70")
        self.assertEqual(rc, 1)


if __name__ == "__main__":
    unittest.main()
