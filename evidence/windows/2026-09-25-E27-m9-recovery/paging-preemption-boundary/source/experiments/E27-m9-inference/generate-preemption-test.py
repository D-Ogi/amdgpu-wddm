"""Extract the actual report DPC into a deterministic scheduler-callback harness.
Usage: python generate-preemption-test.py WDDM_SOURCE OUTPUT_C
Companion prefix/suffix contain host stubs and externally observable assertions.
"""
import pathlib
import sys
here = pathlib.Path(__file__).resolve().parent
source = pathlib.Path(sys.argv[1]).read_text()
def extract(marker):
 a=source.index(marker);b=source.index("{",a);depth=1;i=b+1
 while depth:
  if source[i]=="{":depth+=1
  elif source[i]=="}":depth-=1
  i+=1
 return source[a:i]
pathlib.Path(sys.argv[2]).write_text((here / "preemption-test-prefix.c").read_text() +
 extract("static void WddmReleasePreemptedPagingLocked(") + "\n" +
 extract("static void WddmReportDpcRoutine(") + "\n" +
 (here / "preemption-test-suffix.c").read_text())
