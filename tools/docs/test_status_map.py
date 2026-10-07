"""Tests of tools/docs/status_map.py: the verdict rule, the preprocessor and the treemap. Standard library only.

python -m unittest discover -s tools/docs
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import status_map as sm  # noqa: E402

SAMPLE = r'''
#define TRACED(Name) static NTSTATUS Traced##Name(HANDLE h) { return Logged(#Name, Name(h)); }
static NTSTATUS Logged(const char* n, NTSTATUS s) { GuardLog("%s", n); return s; }
NTSTATUS RealWork(HANDLE h) { Device* d = (Device*)h; d->State = 3; return STATUS_SUCCESS; }
NTSTATUS LogOnly(HANDLE h) { UNREFERENCED_PARAMETER(h); GuardLog("called"); return STATUS_SUCCESS; }
NTSTATUS Refuses(HANDLE h) { if (!h) return STATUS_INVALID_PARAMETER; GuardLog("no"); return STATUS_NOT_SUPPORTED; }
NTSTATUS ZeroOnly(HANDLE h, ARGS* a) { RtlZeroMemory(a, sizeof(*a)); a->Count = 0; return STATUS_SUCCESS; }
NTSTATUS Forward(HANDLE h) { return RealWork(h); }
NTSTATUS ForwardStub(HANDLE h) { return LogOnly(h); }
TRACED(RealWork)
TRACED(LogOnly)
HRESULT Either(Dev* d) { return d->lost.load() ? D3DDDIERR_DEVICEREMOVED : E_NOTIMPL; }
HRESULT CallsEither(Dev* d, void* a) { if (!a) return E_INVALIDARG; return Either(d); }
void Intake(Dev* d, Stage stage) { if (stage == Stage::Mesh) { report(d, E_NOTIMPL); return; } d->shader = Build(d); }
void CreateVs(Dev* d) { Intake(d, Stage::Standard); }
void CreateMs(Dev* d) { Intake(d, Stage::Mesh); }
void Nothing(Dev* d) {}
void Partly(Dev* d, int k) { if (k == 2) { report(d, E_NOTIMPL); return; } Engine(d, k); }
'''


class VerdictTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        src = sm.Source(Path("sample.c"), SAMPLE)
        cls.src = src
        cls.judge = sm.Judge(sm.find_functions(src))

    def v(self, name):
        return self.judge.verdict(name, self.src)

    def test_real_work_is_done(self):
        self.assertTrue(self.v("RealWork")["done"])

    def test_log_only_is_not_done(self):
        self.assertFalse(self.v("LogOnly")["done"])

    def test_refusal_is_not_done(self):
        self.assertFalse(self.v("Refuses")["done"])

    def test_zeroing_is_not_done(self):
        self.assertFalse(self.v("ZeroOnly")["done"])

    def test_forwarder_takes_the_verdict_of_its_target(self):
        self.assertTrue(self.v("Forward")["done"])
        self.assertFalse(self.v("ForwardStub")["done"])

    def test_macro_generated_wrappers(self):
        self.assertTrue(self.v("TracedRealWork")["done"])
        self.assertFalse(self.v("TracedLogOnly")["done"])

    def test_ternary_and_called_failures(self):
        self.assertFalse(self.v("Either")["done"])
        self.assertFalse(self.v("CallsEither")["done"])

    def test_constant_argument_refused_by_the_callee(self):
        self.assertTrue(self.v("CreateVs")["done"])
        self.assertFalse(self.v("CreateMs")["done"])

    def test_empty_body_is_not_done(self):
        self.assertFalse(self.v("Nothing")["done"])

    def test_partial(self):
        v = self.v("Partly")
        self.assertTrue(v["done"])
        self.assertTrue(v.get("partial"))

    def test_missing_definition(self):
        self.assertFalse(self.v("NoSuchFunction")["done"])


class TextTests(unittest.TestCase):
    def test_clean_keeps_lines(self):
        text = 'a /* x\ny */ b // c\n"s;t" d'
        out = sm.clean(text)
        self.assertEqual(out.count("\n"), text.count("\n"))
        self.assertNotIn(";", out)

    def test_is_failure(self):
        self.assertTrue(sm.is_failure("E_NOTIMPL"))
        self.assertTrue(sm.is_failure("(x ? STATUS_NOT_SUPPORTED : E_FAIL)".split("?", 1)[1].split(":")[0]))
        self.assertFalse(sm.is_failure("S_OK"))
        self.assertFalse(sm.is_failure("hr"))


class TreemapTests(unittest.TestCase):
    def test_squarify_areas_and_bounds(self):
        values = [48, 29, 25, 19, 18, 17, 15, 6, 4]
        rects = sm.squarify(values, 0, 0, 840, 330)
        self.assertEqual(len(rects), len(values))
        total = sum(values)
        for v, (x, y, w, h) in zip(values, rects):
            self.assertAlmostEqual(w * h, v / total * 840 * 330, delta=1e-6)
            self.assertGreaterEqual(x, -1e-9)
            self.assertGreaterEqual(y, -1e-9)
            self.assertLessEqual(x + w, 840 + 1e-6)
            self.assertLessEqual(y + h, 330 + 1e-6)

    def test_svg_is_deterministic(self):
        metric = {"title": "T", "subtitle": "S", "groups": [
            {"name": "A", "slots": [{"name": "a", "done": True, "reason": "r"}] * 5},
            {"name": "B", "slots": [{"name": "b", "done": False, "reason": "r"}] * 3}]}
        self.assertEqual(sm.treemap_svg(metric), sm.treemap_svg(metric))
        self.assertIn("5 of 8 slots done", sm.treemap_svg(metric))


if __name__ == "__main__":
    unittest.main()
