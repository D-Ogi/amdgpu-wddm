"""Exercise the cross-header escape namespace, including the b30 collision."""
from pathlib import Path
import unittest

import escape_commands as gate


class EscapeCommands(unittest.TestCase):
    def fixture(self, wave="34u"):
        return {"driver/kmd/board.h": "#define BC250_ESCAPE_RUN_BOARD_CAPS 33u\n",
                "driver/kmd/wave.h": f"#define BC250_ESCAPE_GET_WAVES {wave}\n"}

    def test_separate_headers_pass(self):
        commands, errors = gate.check_sources(self.fixture())
        self.assertEqual([], errors)
        self.assertEqual([33, 34], [c["number"] for c in commands])

    def test_duplicate_in_separate_header_fails(self):
        _, errors = gate.check_sources(self.fixture("33u"))
        self.assertEqual(1, len(errors))
        self.assertIn("duplicate command number 33", errors[0])
        self.assertIn("board.h", errors[0])
        self.assertIn("wave.h", errors[0])

    def test_hex_parentheses_and_suffix_still_collide(self):
        _, errors = gate.check_sources(self.fixture("(0x21UL)"))
        self.assertIn("duplicate command number 33", errors[0])

    def test_unknown_verb_is_in_namespace(self):
        sources = self.fixture()
        sources["driver/kmd/future.h"] = "#define BC250_ESCAPE_QUERY_FUTURE 34u\n"
        self.assertIn("duplicate command number 34", gate.check_sources(sources)[1][0])

    def test_shared_hip_command_collides_with_kmd_replug(self):
        sources = self.fixture()
        sources["driver/shim/include/bc250_hip_journal.h"] = "#define BC250_HIP_JOURNAL_COMMAND 35u\n"
        sources["driver/kmd/replug.h"] = "#define BC250_ESCAPE_CONNECTOR_REPLUG 35u\n"
        _, errors = gate.check_sources(sources)
        self.assertEqual(1, len(errors))
        self.assertIn("duplicate command number 35", errors[0])
        self.assertIn("BC250_HIP_JOURNAL_COMMAND", errors[0])
        self.assertIn("BC250_ESCAPE_CONNECTOR_REPLUG", errors[0])
        sources["driver/kmd/replug.h"] = "#define BC250_ESCAPE_CONNECTOR_REPLUG 36u\n"
        self.assertEqual([], gate.check_sources(sources)[1])

    def test_noncommands_and_comments_are_excluded(self):
        sources = self.fixture()
        sources["driver/kmd/fields.h"] = """
/* #define BC250_ESCAPE_OLD 33u */
// #define BC250_ESCAPE_OLD2 34u
#define BC250_ESCAPE_MAGIC 33u
#define BC250_ESCAPE_FLAG_ACTIVE 33u
#define BC250_ESCAPE_STATUS_DONE 34u
"""
        self.assertEqual([], gate.check_sources(sources)[1])

    def test_alias_or_expression_cannot_escape_validation(self):
        for expression in ("BC250_ESCAPE_RUN_BOARD_CAPS", "(30 + 3)", "-1", "0", "0x100000000", "041"):
            with self.subTest(expression=expression):
                self.assertTrue(gate.check_sources(self.fixture(expression))[1])

    def test_duplicate_name_fails_even_with_new_value(self):
        sources = self.fixture()
        sources["duplicate.h"] = "#define BC250_ESCAPE_GET_WAVES 35u\n"
        self.assertIn("duplicate command name", gate.check_sources(sources)[1][0])

    def test_empty_scan_fails(self):
        self.assertTrue(gate.check_sources({})[1])

    def test_c_line_splicing_cannot_hide_a_duplicate(self):
        for split in ("#de\\\nfine", "#define BC250_ESCA\\\nPE_FUTURE"):
            sources = self.fixture()
            definition = split + (" BC250_ESCAPE_FUTURE" if split.startswith("#de\\") else "")
            sources["split.h"] = definition + " 33u\n"
            with self.subTest(definition=definition):
                self.assertIn("duplicate command number 33", gate.check_sources(sources)[1][0])

    def test_repository(self):
        commands, errors = gate.check(Path(__file__).resolve().parents[2])
        self.assertEqual([], errors)
        self.assertGreaterEqual(len(commands), 33)
        self.assertIn({"name": "BC250_HIP_JOURNAL_COMMAND", "number": 35},
                      [{"name": c["name"], "number": c["number"]} for c in commands])


if __name__ == "__main__":
    unittest.main()
