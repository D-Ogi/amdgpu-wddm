from pathlib import Path
import sys
here=Path(__file__).resolve().parent
s=Path(sys.argv[1]).read_text()
chunks=[]
for name in ["WddmCompleteSoftware", "WddmGpuFence", "WddmSubmitDpcRoutine", "WddmGpuFencePaging", "WddmPagingSubmitDpcRoutine"]:
    marker=("static void " if name in ["WddmCompleteSoftware", "WddmSubmitDpcRoutine", "WddmPagingSubmitDpcRoutine"] else "void ")+name+"("
    start=s.index(marker); p=s.index("{",start); depth=1; end=p+1
    while depth:
        depth+=(s[end]=="{")-(s[end]=="}"); end+=1
    chunks.append(s[start:end])
Path(sys.argv[2]).write_text((here/"watchdog-test-prefix.c").read_text()+"\n".join(chunks)+(here/"watchdog-test-suffix.c").read_text())
