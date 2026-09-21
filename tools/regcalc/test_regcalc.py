"""Run: python -m unittest discover -s tools/regcalc"""

import unittest

from regcalc import RegMap


class GcAddressing(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rm = RegMap()

    def test_segment_bases_are_dword_indices(self):
        self.assertEqual(self.rm.segs[0], 0x1260)
        self.assertEqual(self.rm.segs[1], 0xA000)

    def test_historic_radeon_addresses(self):
        # GRBM_STATUS at 0x8010 is the same byte address since R600: sanity check of the formula.
        self.assertEqual(self.rm.byte_offset("mmGRBM_STATUS"), 0x8010)
        self.assertEqual(self.rm.byte_offset("mmGRBM_SOFT_RESET"), 0x8020)
        self.assertEqual(self.rm.byte_offset("mmCP_ME_CNTL"), 0x86D8)

    def test_seg0_registers(self):
        self.assertEqual(self.rm.byte_offset("mmSPI_PG_ENABLE_STATIC_WGP_MASK"), 0x935C)
        self.assertEqual(self.rm.byte_offset("mmCC_GC_SHADER_ARRAY_CONFIG"), 0x89BC)
        self.assertEqual(self.rm.byte_offset("mmCP_RB0_BASE"), 0xC100)
        self.assertEqual(self.rm.byte_offset("mmCP_RB0_CNTL"), 0xC104)
        self.assertEqual(self.rm.byte_offset("mmCP_HQD_PQ_BASE"), 0xC844)

    def test_seg1_registers(self):
        self.assertEqual(self.rm.byte_offset("mmSCRATCH_REG0"), 0x30100)
        self.assertEqual(self.rm.byte_offset("mmGRBM_GFX_INDEX"), 0x30800)
        self.assertEqual(self.rm.byte_offset("mmRLC_PG_ALWAYS_ON_WGP_MASK"), 0x3B14C)

    def test_reverse_roundtrip(self):
        self.assertIn("mmGRBM_STATUS", self.rm.reverse(0x8010))

    def test_predecessor_addresses_are_not_what_they_claimed(self):
        # Addresses used by Keshas-dev/AMD-BC-250-Windows-Driver @63f8956.
        self.assertNotIn("mmSPI_PG_ENABLE_STATIC_WGP_MASK", self.rm.reverse(0x5C3C))
        self.assertNotIn("mmCC_GC_SHADER_ARRAY_CONFIG", self.rm.reverse(0x9C1C))
        self.assertIn("mmGCEA_PERFCOUNTER1_CFG", self.rm.reverse(0x9C1C))

    def test_fabricated_names_do_not_exist(self):
        for name in ("mmKIQ_BASE_LO", "mmKIQ_CNTL", "mmCP_RING0_BASE_LO", "mmCOMPUTE_RING0_BASE_LO"):
            self.assertNotIn(name, self.rm.regs)


if __name__ == "__main__":
    unittest.main()
