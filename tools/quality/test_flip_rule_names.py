"""Controls for the DirectFlip refusal-table gate (audit finding K4).

Each rule of flip_rule_names.py gets a fixture that breaks it alone, and one fixture that is the
shipping shape and must pass. The real tree is checked too, so the gate fails here as well as in
quick.ps1 when the document and the rule drift apart again.
"""
import os
from pathlib import Path
import tempfile
import unittest

from flip_rule_names import check

OUT = os.environ.get("BC250_TEST_OUT") or None
REPO = Path(__file__).resolve().parents[2]

# A rule with three clauses. `third` is appended to the enumeration after `second`, as immediate_swizzle
# was, and applied between `first` and `second`, so the two orders differ on purpose.
ROUTER = """
namespace bc250front {
enum class FlipRefusal {
    none,
    first,          // a comment with the word second in it, which must not count
    second,
    third,
};
inline const char *FlipRefusalText(FlipRefusal reason)
{
    switch (reason) {
    case FlipRefusal::none: return "supported";
    case FlipRefusal::first: return "first";
    case FlipRefusal::second: return "second";
    case FlipRefusal::third: return "third-clause";
    }
    return "unknown";
}
inline FlipRefusal FlipReason(const bc250_scanout_caps &caps, const Resource *client,
                              const Resource *compositor, unsigned checkFlags)
{
    if (!client) return FlipRefusal::first;
    if (checkFlags) return FlipRefusal::third;
    if (!compositor) return FlipRefusal::second;
    return FlipRefusal::none;
}
}
"""
LOG = "static const unsigned kFlipRules = 4;\n"
DOCUMENT = """# A document

The rule applies these clauses in order, and each clause has a name that a trace prints:

| Refusal | What it means |
| --- | --- |
| `first` | the first clause |
| `third-clause` | the clause that was appended to the enumeration |
| `second` | the second clause |

More prose, and a stray table that is not the refusal list:

| Thing | Meaning |
| --- | --- |
| `other` | not a refusal |
"""


class FlipRuleNames(unittest.TestCase):
    def run_check(self, router=ROUTER, log=LOG, document=DOCUMENT):
        with tempfile.TemporaryDirectory(dir=OUT) as directory:
            path = Path(directory)
            (path / "front-direct-flip.h").write_text(router, encoding="utf-8")
            (path / "front-flip-log.h").write_text(log, encoding="utf-8")
            (path / "direct-flip-handshake.md").write_text(document, encoding="utf-8")
            return check(path / "front-direct-flip.h", path / "front-flip-log.h",
                         path / "direct-flip-handshake.md")[0]

    def test_shipping_shape_passes(self):
        self.assertEqual(self.run_check(), [])

    def test_a_clause_without_a_printed_name_fails(self):
        router = ROUTER.replace('    case FlipRefusal::third: return "third-clause";\n', "")
        failures = self.run_check(router=router)
        self.assertTrue(any("prints no name for: third" in f for f in failures), failures)

    def test_two_clauses_with_one_printed_name_fail(self):
        router = ROUTER.replace('case FlipRefusal::third: return "third-clause";',
                                'case FlipRefusal::third: return "second";')
        failures = self.run_check(router=router)
        self.assertTrue(any("one name for two enumerators" in f for f in failures), failures)

    def test_a_counter_array_of_the_wrong_size_fails(self):
        failures = self.run_check(log="static const unsigned kFlipRules = 3;\n")
        self.assertTrue(any("kFlipRules is 3 and FlipRefusal has 4 enumerators" in f for f in failures), failures)

    def test_a_clause_absent_from_the_table_fails(self):
        """The K4 defect itself: the rule gained a clause and the published table did not."""
        document = DOCUMENT.replace("| `third-clause` | the clause that was appended to the enumeration |\n", "")
        failures = self.run_check(document=document)
        self.assertTrue(any("does not list: third-clause" in f for f in failures), failures)

    def test_a_table_row_the_rule_cannot_answer_fails(self):
        document = DOCUMENT.replace("| `second` | the second clause |",
                                    "| `second` | the second clause |\n| `fourth` | a clause that does not exist |")
        failures = self.run_check(document=document)
        self.assertTrue(any("lists what the rule cannot answer: fourth" in f for f in failures), failures)

    def test_the_enumeration_order_is_not_accepted_as_the_clause_order(self):
        """A table written from the enumeration instead of the rule: every name present, wrong row."""
        document = DOCUMENT.replace(
            "| `first` | the first clause |\n"
            "| `third-clause` | the clause that was appended to the enumeration |\n"
            "| `second` | the second clause |",
            "| `first` | the first clause |\n"
            "| `second` | the second clause |\n"
            "| `third-clause` | the clause that was appended to the enumeration |")
        failures = self.run_check(document=document)
        self.assertTrue(any("is out of order" in f for f in failures), failures)

    def test_a_clause_the_rule_never_answers_fails(self):
        router = ROUTER.replace("    if (checkFlags) return FlipRefusal::third;\n", "")
        document = DOCUMENT.replace("| `third-clause` | the clause that was appended to the enumeration |\n", "")
        failures = self.run_check(router=router, document=document)
        self.assertTrue(any("FlipReason never answers: third" in f for f in failures), failures)

    def test_the_shipping_tree(self):
        failures = check(REPO / "driver" / "umd" / "router" / "front-direct-flip.h",
                         REPO / "driver" / "umd" / "router" / "front-flip-log.h",
                         REPO / "docs" / "design" / "direct-flip-handshake.md")[0]
        self.assertEqual(failures, [])


if __name__ == "__main__":
    unittest.main()
