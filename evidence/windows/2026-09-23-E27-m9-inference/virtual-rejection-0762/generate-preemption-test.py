"""Extract the actual report DPC into a deterministic scheduler-callback harness.
Usage: python generate-preemption-test.py WDDM_SOURCE OUTPUT_C
Companion prefix/suffix contain host stubs and externally observable assertions.
"""
import pathlib
import sys
here = pathlib.Path(__file__).resolve().parent
source = pathlib.Path(sys.argv[1]).read_text()
a = source.index("static void WddmReportDpcRoutine(")
b = source.index("// ---- the software VSync", a)
pathlib.Path(sys.argv[2]).write_text((here / "preemption-test-prefix.c").read_text() + source[a:b] + (here / "preemption-test-suffix.c").read_text())
