"""Tests for analyze.py: the C selftest file (exact shares), a random GPU timeline with known truth sampled the way the
lab tool samples it (estimates within tolerance, per frame too), the lite set, failed reads and format checks.

    python -m unittest discover -s tools/win/gpu-timeline -p "test_*.py" -v

The C selftest case needs gpu-timeline.exe (build.ps1 writes it outside this repository; BC250_GTL_BUILD or
BC250_GTL_EXE names it) and skips itself when the exe is absent.
"""
import bisect
import os
import random
import re
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

import analyze

HERE = Path(__file__).resolve().parent
ROOT = Path(os.environ.get("BC250_ROOT") or HERE.parents[3].parent)
BUILD = Path(os.environ.get("BC250_GTL_BUILD") or ROOT / "scratch/build/gpu-timeline")
EXE = Path(os.environ.get("BC250_GTL_EXE") or BUILD / "gpu-timeline.exe")
MASKS = analyze.load_masks(analyze.DEFAULT_MASK_HEADER)


def mask(reg, field):
    for f, m, _ in MASKS[reg]:
        if f == field:
            return m
    raise KeyError(reg + "." + field)


def gtl_sets():
    """FULL and LITE register lists from the generated gtl_regs.h, the same the exe was built with."""
    text = (HERE / "gtl_regs.h").read_text(encoding="utf-8")
    sets = {}
    for name in ("Full", "Lite"):
        body = text[text.index(f"g_Gtl{name}["):]
        body = body[:body.index("};")]
        sets[name.lower()] = [(int(o, 16), n) for o, n in re.findall(r"\{ 0x([0-9A-Fa-f]+)ul, \"(\w+)\" \}", body)]
    return sets


def encode(state, regs):
    """Register values for one state, with the bits the C selftest uses."""
    v = {n: 0 for _, n in regs}
    g = mask("GRBM_STATUS", "DB_CLEAN") | mask("GRBM_STATUS", "CB_CLEAN")
    act = mask("GRBM_STATUS", "GUI_ACTIVE") | mask("GRBM_STATUS", "CP_BUSY")
    if state == "idle":
        pass
    elif state == "fed":
        g |= act | sum(mask("GRBM_STATUS", f) for f in ("SPI_BUSY", "PA_BUSY", "SC_BUSY", "CB_BUSY", "DB_BUSY"))
        v["CP_STAT"] = mask("CP_STAT", "CP_BUSY") | mask("CP_STAT", "ME_BUSY")
        v["CP_BUSY_STAT"] = mask("CP_BUSY_STAT", "ME_PARSING_PACKETS")
    elif state == "compute":
        g |= act | mask("GRBM_STATUS", "SPI_BUSY") | mask("GRBM_STATUS", "TA_BUSY")
        v["CP_STAT"] = mask("CP_STAT", "CP_BUSY")
    elif state == "drain":
        g |= act | mask("GRBM_STATUS", "SPI_BUSY")
        v["CP_STALLED_STAT2"] = mask("CP_STALLED_STAT2", "ME_WAITING_ON_PARTIAL_FLUSH")
    elif state == "sync":
        g |= act | mask("GRBM_STATUS", "CP_COHERENCY_BUSY")
        v["CP_STAT"] = mask("CP_STAT", "SURFACE_SYNC_BUSY")
    elif state == "poll":
        g |= act
        v["CP_BUSY_STAT"] = mask("CP_BUSY_STAT", "SEM_POLLING_FOR_PASS")
    elif state == "fetch":
        g |= act
        v["CP_STALLED_STAT2"] = mask("CP_STALLED_STAT2", "PFP_WAITING_ON_BUFFER_DATA")
    else:
        raise ValueError(state)
    v["GRBM_STATUS"] = g
    if "SDMA0_STATUS_REG" in v:
        v["SDMA0_STATUS_REG"] = mask("SDMA0_STATUS_REG", "IDLE")
        v["SDMA1_STATUS_REG"] = mask("SDMA1_STATUS_REG", "IDLE")
    return v


EXPECT = {"idle": ("idle", ""), "fed": ("pipeline", "fed"), "compute": ("pipeline", "fed"),
          "drain": ("pipeline", "drain"), "sync": ("cp_only", "sync"), "poll": ("cp_only", "poll"),
          "fetch": ("cp_only", "fetch")}


def write_gtl(path, regs, samples, freq=10_000_000, hz=1009, setname=b"full", qpc_start=5_000_000_000):
    """samples: (qpc, ticks, status, {name: value}). The closing GRBM_STATUS repeats the first."""
    header = analyze.HEADER.pack(b"GTL1", 1, analyze.HEADER.size, analyze.REGDESC.size, 16 + 4 * len(regs), len(regs),
                                 hz, 60, len(samples), 0, sum(1 for s in samples if s[2]), 0, freq, qpc_start,
                                 samples[-1][0] + 1, 0, 0, 0xE0000000, 0xE0000000, 0, 1, setname, 0, 0)
    out = bytearray(header)
    for off, name in regs:
        out += analyze.REGDESC.pack(off, name.encode())
    for qpc, ticks, status, v in samples:
        out += struct.pack("<QII", qpc, ticks, status)
        out += struct.pack(f"<{len(regs)}I", *[v.get(n, 0) if not status else 0 for _, n in regs])
    Path(path).write_bytes(bytes(out))


def make_timeline(rng, seconds):
    """Frames of 16-22 ms: an idle gap, then busy segments. Returns segment starts, states, frame starts, truth."""
    starts, states, frames = [], [], []
    t = 0.0
    while t < seconds:
        frames.append(t)
        frame = rng.uniform(0.016, 0.022)
        end = t + frame
        gap = rng.uniform(0.002, 0.007)
        starts.append(t); states.append("idle")
        t += gap
        while t < end:
            s = rng.choices(["fed", "compute", "drain", "sync", "poll", "fetch"], [50, 10, 12, 10, 8, 4])[0]
            d = min(end - t, rng.uniform(0.0002, 0.003))
            starts.append(t); states.append(s)
            t += d
        t = end
    starts.append(t); states.append("idle")
    return starts, states, frames, t


def truth(starts, states, t0, t1):
    tot = {}
    for k in range(len(starts) - 1):
        a, b = max(starts[k], t0), min(starts[k + 1], t1)
        if b > a:
            tot[states[k]] = tot.get(states[k], 0.0) + (b - a)
    return tot


class SelfTestFile(unittest.TestCase):
    def test_exact_shares(self):
        if not EXE.exists():
            self.skipTest("build/gpu-timeline.exe not built")
        with tempfile.TemporaryDirectory(dir=EXE.parent) as d:
            path = Path(d) / "s.gtl"
            subprocess.run([str(EXE), "selftest", "--out", str(path)], check=True, capture_output=True)
            res = self.analyze(path)
        top, sub = res["top_share"], res["sub_share"]
        self.assertAlmostEqual(top["idle"], 0.30, places=6)
        self.assertAlmostEqual(top["pipeline"], 0.50, places=6)
        self.assertAlmostEqual(top["cp_only"], 0.20, places=6)
        self.assertAlmostEqual(sub["pipeline/drain"], 0.10, places=6)
        self.assertAlmostEqual(sub["pipeline/fed"], 0.40, places=6)
        self.assertAlmostEqual(sub["cp_only/sync"], 0.10, places=6)
        self.assertAlmostEqual(sub["cp_only/poll"], 0.05, places=6)
        self.assertAlmostEqual(sub["cp_only/parse"], 0.05, places=6)
        self.assertAlmostEqual(res["bracket_change_share"], 0.02, places=6)
        self.assertAlmostEqual(res["SDMA0_STATUS_REG_busy_share"], 0.10, places=6)
        self.assertAlmostEqual(res["rb0_rptr_moves_per_s"], 200, delta=1)
        self.assertEqual(res["anomalies"], {})
        self.assertEqual(res["set"], "synth")

    def analyze(self, path, **kw):
        args = analyze.argparse.Namespace(run=path, fps=kw.get("fps"), frames=kw.get("frames"), frames_csv=None,
                                          window=kw.get("window"), batch_s=1.0, top=8, json=None)
        return analyze.analyze(analyze.Run(path), MASKS, args)


class RandomTimeline(unittest.TestCase):
    def sample(self, seed, seconds, regs, hz=1009, freq=10_000_000, idle_skip=0.0):
        """idle_skip: chance that a sample taken in an idle stretch is followed by two missed periods (a sampler
        starved exactly when the GPU idles), the case the time weighting exists for."""
        rng = random.Random(seed)
        starts, states, frames, end = make_timeline(rng, seconds)
        cache = {s: encode(s, regs) for s in EXPECT}
        samples, t = [], 0.0
        while t < end - 0.001:
            if rng.random() < 0.002:           # a late wake: a gap of a few periods
                t += rng.uniform(2, 5) / hz
                continue
            ticks = int(rng.uniform(15e-6, 40e-6) * freq)
            mid = t + ticks / freq / 2
            state = states[bisect.bisect_right(starts, mid) - 1]
            samples.append((5_000_000_000 + int(t * freq), ticks, 0, cache[state]))
            t += 1.0 / hz + rng.uniform(0, 0.2e-3)
            if state == "idle" and rng.random() < idle_skip:
                t += 2.0 / hz
        return starts, states, frames, samples

    def run_case(self, regs, seed=7, seconds=30.0, idle_skip=0.0):
        starts, states, frames, samples = self.sample(seed, seconds, regs, idle_skip=idle_skip)
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "r.gtl"
            write_gtl(path, regs, samples)
            fpath = Path(d) / "frames.txt"
            fpath.write_text("\n".join(str(5_000_000_000 + int(f * 10_000_000)) for f in frames) + "\n")
            res = SelfTestFile.analyze(self, path, frames=str(fpath), fps=None)
        t0 = (samples[0][0] - 5_000_000_000) / 1e7
        t1 = (samples[-1][0] - 5_000_000_000) / 1e7
        tot = truth(starts, states, t0, t1)
        span = sum(tot.values())
        want_top, want_sub = {}, {}
        for s, x in tot.items():
            top, sub = EXPECT[s]
            want_top[top] = want_top.get(top, 0.0) + x / span
            if sub:
                want_sub[f"{top}/{sub}"] = want_sub.get(f"{top}/{sub}", 0.0) + x / span
        return res, want_top, want_sub, frames, starts, states

    def test_full_set_estimates(self):
        res, want_top, want_sub, frames, starts, states = self.run_case(gtl_sets()["full"])
        for c in analyze.TOP:
            err = res["batches"]["per_class"].get(c, {"stderr": 0.0})["stderr"]
            self.assertLess(abs(res["top_share"][c] - want_top.get(c, 0.0)), max(0.01, 4 * err), c)
        for k, x in want_sub.items():
            self.assertLess(abs(res["sub_share"].get(k, 0.0) - x), 0.01, k)
        self.assertEqual(res["inputs_missing_for_rules"], [])
        self.assertLess(res["bracket_change_share"], 1e-9)   # the writer repeats GRBM_STATUS exactly

        # Per frame: mean ms per class against the truth over the same frames.
        f = res["frames"]
        self.assertGreater(f["frames"], 1300)
        n = len(frames) - 1
        per = {c: 0.0 for c in analyze.TOP}
        for a, b in zip(frames, frames[1:]):
            for s, x in truth(starts, states, a, b).items():
                per[EXPECT[s][0]] += x * 1e3
        for c in ("idle", "pipeline", "cp_only"):
            self.assertLess(abs(f[c]["mean"] - per[c] / n), 0.35, c)
        self.assertLess(abs(f["ms"]["mean"] - 19.0), 0.2)

    def test_weighting_corrects_state_correlated_gaps(self):
        res, want_top, *_ = self.run_case(gtl_sets()["full"], seed=3, seconds=20.0, idle_skip=0.5)
        weighted = abs(res["top_share"]["idle"] - want_top["idle"])
        unweighted = abs(res["top_share_unweighted"]["idle"] - want_top["idle"])
        self.assertLess(weighted, 0.02)
        self.assertGreater(unweighted, 2 * weighted + 0.02)

    def test_lite_set(self):
        res, want_top, want_sub, *_ = self.run_case(gtl_sets()["lite"], seed=11, seconds=15.0)
        for c in ("idle", "pipeline", "cp_only"):
            self.assertLess(abs(res["top_share"][c] - want_top.get(c, 0.0)), 0.015, c)
        # The lite set has no CPF registers: the fetch rule loses CP_CPF_STALLED_STAT1 but keeps CP_STALLED_STAT2.
        self.assertIn("CP_CPF_STALLED_STAT1", res["inputs_missing_for_rules"])
        self.assertLess(abs(res["sub_share"].get("cp_only/fetch", 0.0) - want_sub.get("cp_only/fetch", 0.0)), 0.01)

    def test_failed_reads_are_excluded(self):
        regs = gtl_sets()["full"]
        v = encode("fed", regs)
        samples = [(5_000_000_000 + i * 9910, 300, 0 if i % 10 else 31, v) for i in range(2000)]
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "f.gtl"
            write_gtl(path, regs, samples)
            res = SelfTestFile.analyze(self, path)
        self.assertEqual(res["failed_reads"], 200)
        self.assertEqual(res["samples"], 1800)
        self.assertAlmostEqual(res["top_share"]["pipeline"], 1.0)


class Format(unittest.TestCase):
    def test_bad_magic(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "x.gtl"
            path.write_bytes(b"NOPE" + bytes(200))
            with self.assertRaises(ValueError):
                analyze.Run(path)

    def test_masks_match_the_c_build(self):
        # The C side uses GRBM_STATUS__GUI_ACTIVE_MASK etc. from the same header; this pins the parser to it.
        self.assertEqual(mask("GRBM_STATUS", "GUI_ACTIVE"), 0x80000000)
        self.assertEqual(mask("SDMA0_STATUS_REG", "IDLE"), 0x1)
        self.assertEqual(analyze.HEADER.size, 120)
        self.assertEqual(analyze.REGDESC.size, 32)

    def test_offsets_from_regcalc(self):
        import sys
        sys.path.insert(0, str(HERE.parents[1] / "regcalc"))
        import regcalc
        rm = regcalc.RegMap()
        for regs in gtl_sets().values():
            for off, name in regs:
                self.assertEqual(off, rm.byte_offset("mm" + name), name)


if __name__ == "__main__":
    unittest.main()
