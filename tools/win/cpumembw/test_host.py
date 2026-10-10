"""CPU-only instrument checks. Does not load Vulkan, D3D12 or contact a lab."""
import argparse
import json
import subprocess
import time


def run(exe, args, code=0):
    start = time.monotonic()
    result = subprocess.run([exe, *args], capture_output=True, text=True, timeout=10)
    assert result.returncode == code, (args, result.returncode, result.stdout, result.stderr)
    return result, time.monotonic() - start


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--output", required=True)
    args = ap.parse_args()
    results = []
    r, elapsed = run(args.exe, ["--self-test"])
    results.append({"case": "patterns-corruption-adaptation-luid", "stdout": r.stdout, "seconds": elapsed})
    for heap in ("default", "unified"):
        r, elapsed = run(args.exe, ["--api", "cpu", "--heap", heap, "--bytes", "65544", "--duration", ".02", "--timeout-ms", "3000"])
        rows = [json.loads(line) for line in r.stdout.splitlines()]
        measurements = [row for row in rows if row["event"] == "measurement"]
        assert len(measurements) == 4
        assert all(row["bytes"] > 0 and row["seconds"] > 0 for row in measurements)
        assert rows[-1] == {"event": "complete", "status": "pass"}
        assert rows[0]["heap_policy"] == heap
        results.append({"case": "cpu-supervised-" + heap, "rows": rows, "seconds": elapsed})
    for bad in (["--bytes", "-1"], ["--bytes", "4097"], ["--duration", "nan"],
                ["--timeout-ms", "170000"], ["--luid", "foo"], ["--api", "invalid"],
                ["--type", "32"], ["--adapter", "4294967296"], ["--bytes"]):
        r, elapsed = run(args.exe, bad, 2)
        assert not r.stdout, "bad option started a measurement"
        results.append({"case": "reject " + " ".join(bad), "seconds": elapsed})
    r, elapsed = run(args.exe, ["--api", "cpu", "--test-corrupt", "--bytes", "65544", "--duration", ".01"], 2)
    assert "content mismatch" in r.stderr and '"event":"complete"' not in r.stdout
    results.append({"case": "actual-phase-corruption-tail", "seconds": elapsed})
    r, elapsed = run(args.exe, ["--api", "cpu", "--test-stall", "--timeout-ms", "500"], 124)
    rows = [json.loads(line) for line in r.stdout.splitlines()]
    assert rows[-1]["event"] == "watchdog" and rows[-1]["child_exited"] is True
    assert elapsed < 3
    results.append({"case": "blocked-child-watchdog", "seconds": elapsed, "rows": rows})
    with open(args.output, "w", encoding="utf-8") as f:
        json.dump({"status": "pass", "tests": results}, f, indent=2)
    print("PASS: 14 CPU-only, malformed-input and child-watchdog cases")


if __name__ == "__main__":
    main()
