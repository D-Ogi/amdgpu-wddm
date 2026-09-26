"""Validate the saved run016 without rerunning GPU work. No hardware access."""
import hashlib
import json
from pathlib import Path
import re
import sys

run = Path(sys.argv[1])
repo = Path(__file__).resolve().parents[2]
reference = repo / "evidence/linux/2026-09-21-E14-vulkan-compute-reference/llama"


def read(path):
    raw = path.read_bytes()
    return raw.decode("utf-16" if raw.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig")


def checked_process(name):
    assert int(read(run / (name + ".exit"))) == 0, name
    trace = read(run / (name + ".err"))
    assert "bc250: progress before submit" in trace, name
    assert "quiet-submit" in trace, name
    return trace


result = {"text": [], "benchmarks": {}}
checked_process("m8")
assert "8 test(s) run, 0 mismatch(es)" in read(run / "m8.out")
for model in ("stories15M", "tinyllama"):
    expected = read(reference / (model + "-ngl99.out")).replace("\r\n", "\n")
    for i in range(1, 5):
        name = f"{model}-{i}"
        trace = checked_process(name)
        offload = re.search(r"offloaded (\d+)/(\d+) layers to GPU", trace)
        assert offload and int(offload[1]) > 0 and offload[1] == offload[2], name
        actual = read(run / (name + ".out")).replace("\r\n", "\n")
        assert actual == expected, name
        result["text"].append({"run": name, "matches_linux_gpu": True,
                               "sha256": hashlib.sha256(actual.encode()).hexdigest()})
    name = "bench-" + model
    checked_process(name)
    rows = json.loads(read(run / (name + ".out")))
    assert len(rows) == 2
    assert {(r["n_prompt"], r["n_gen"]) for r in rows} == {(512, 0), (0, 128)}
    for row in rows:
        assert row["n_gpu_layers"] == 99 and row["backends"] == "Vulkan"
        assert row["avg_ts"] > 0 and len(row["samples_ts"]) == 3
    result["benchmarks"][model] = rows

kmd = read(run / "kmd.txt")
matches = re.findall(r"node 0 hardware: (\d+) submitted, (\d+) completed, (\d+) timeouts, (\d+) refused", kmd)
assert matches
submitted, completed, timeouts, refused = map(int, matches[-1])
assert submitted > 0 and submitted == completed and timeouts == refused == 0
assert re.search(r"summary: umd: .*?, 0 not run", kmd)
assert "summary: no TDR" in kmd and "summary: *** TDR:" not in kmd
result["hardware"] = dict(submitted=submitted, completed=completed, timeouts=timeouts, refused=refused)
result["residency"] = {heap: int(read(run / f"residency-{heap}-64K.exit")) for heap in ("vram", "gtt")}
result["paging_validated"] = False
print(json.dumps(result, indent=2))
