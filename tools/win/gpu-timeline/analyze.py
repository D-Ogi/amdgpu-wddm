#!/usr/bin/env python3
"""analyze.py - GPU time split from a gpu-timeline.exe run (GTL1 file): idle, shader/fixed-function pipeline busy,
command processor alone (and what it waits on), per second and per frame.

    python analyze.py RUN.gtl [--fps F] [--frames FILE] [--window S0 S1] [--top N] [--json OUT]
                              [--frames-csv OUT] [--mask-header gc_10_1_0_sh_mask.h]

Field masks come from the vendored amdgpu header (third_party/linux-amdgpu/gc_10_1_0_sh_mask.h), looked up by the
register names the file carries; no bit position is typed here.

Classes, from the first GRBM_STATUS of each sample (GRBM_STATUS is read again last; a sample whose class differs
between the two reads is counted as a "bracket change", the error bar of the point-sample assumption):
  idle         GUI_ACTIVE clear
  pipeline     GUI_ACTIVE and any of SPI TA GDS GE SX BCI SC PA DB CB busy. Split into
               pipeline/fed      the CP is not blocked
               pipeline/drain    the ME is blocked behind the pipeline: partial flush, surface sync or a memory poll
                                 (barrier drains: the pipeline still works, nothing new is fed to it)
  cp_only      GUI_ACTIVE, no pipeline block busy, CP busy. First matching reason wins:
               sync        surface sync / cache flush (CP_STAT SURFACE_SYNC_BUSY, GCRIU_BUSY, CP_COHERENCY_BUSY,
                           COHER_CNT_NEQ_ZERO, SURF_SYNC_NEEDS_*)
               drain       partial flush or end-of-pipe still pending (ME_WAITING_ON_PARTIAL_FLUSH, EOPD_FIFO_NEEDS_*,
                           EOP_DONE_BUSY)
               poll        memory/semaphore wait: WAIT_REG_MEM and friends (SEM_POLLING_FOR_PASS,
                           SEM_FAILED_AND_HOLDING, SEMAPHORE_BUSY, ME_WAITING_ON_TC_READ_DATA / REG_READ_DATA,
                           ME_STALLED_ON_ATOMIC_RTN_DATA)
               write       write confirms (ME/PFP_STALLED_ON_TC_WR_CONFIRM, PFP_STALLED_ON_ATOMIC_RTN_DATA)
               fetch       command fetch starved (PFP_WAITING_ON_BUFFER_DATA, ME_WAITING_DATA_FROM_PFP, CPF
                           *_FETCHING_DATA, UTCL1 translation waits)
               parse       the CP is processing packets with nothing behind it (ME/PFP_PARSING_PACKETS, ME/PFP/MEQ busy)
               other       none of these bits
  other_active GUI_ACTIVE without a pipeline block or CP busy bit
The bit names are AMD's; what each means under load is not documented beyond its name, so the top raw bit
combinations of each CP class are printed too, and the interpretation can change without a new run.
"""
import argparse
import collections
import json
import math
import re
import struct
import sys
from pathlib import Path

# bc250-win/tools/win/gpu-timeline/analyze.py: the repository root is three levels up.
DEFAULT_MASK_HEADER = Path(__file__).resolve().parents[3] / "third_party" / "linux-amdgpu" / \
    "gc_10_1_0_sh_mask.h"

HEADER = struct.Struct("<4s11I3Q2Q2II I8s2I")
REGDESC = struct.Struct("<I28s")
FLAGS = {1: "stopped-by-file", 2: "stopped-by-failures", 4: "synthetic", 8: "sample-cap"}

PIPE_FIELDS = ["SPI_BUSY", "TA_BUSY", "GDS_BUSY", "GE_BUSY", "SX_BUSY", "BCI_BUSY", "SC_BUSY", "PA_BUSY",
               "DB_BUSY", "CB_BUSY"]
DRAW_FIELDS = ["PA_BUSY", "SC_BUSY", "DB_BUSY", "CB_BUSY"]
SHADER_FIELDS = ["SPI_BUSY", "TA_BUSY"]

# (register, field) lists, evaluated in order; a register the run did not read counts as 0.
DRAIN_WHILE_PIPE = [
    ("CP_STALLED_STAT2", "ME_WAITING_ON_PARTIAL_FLUSH"), ("CP_STALLED_STAT2", "SURF_SYNC_NEEDS_IDLE_CNTXS"),
    ("CP_STALLED_STAT2", "SURF_SYNC_NEEDS_ALL_CLEAN"), ("CP_STAT", "SURFACE_SYNC_BUSY"),
    ("CP_BUSY_STAT", "SEM_POLLING_FOR_PASS"), ("CP_BUSY_STAT", "SEM_FAILED_AND_HOLDING"),
    ("CP_STALLED_STAT1", "ME_WAITING_ON_TC_READ_DATA"),
]
CP_REASONS = [
    ("sync", [("CP_STAT", "SURFACE_SYNC_BUSY"), ("CP_STAT", "GCRIU_BUSY"), ("GRBM_STATUS", "CP_COHERENCY_BUSY"),
              ("CP_BUSY_STAT", "COHER_CNT_NEQ_ZERO"), ("CP_STALLED_STAT2", "SURF_SYNC_NEEDS_IDLE_CNTXS"),
              ("CP_STALLED_STAT2", "SURF_SYNC_NEEDS_ALL_CLEAN")]),
    ("drain", [("CP_STALLED_STAT2", "ME_WAITING_ON_PARTIAL_FLUSH"), ("CP_STALLED_STAT2", "EOPD_FIFO_NEEDS_SC_EOP_DONE"),
               ("CP_STALLED_STAT2", "EOPD_FIFO_NEEDS_WR_CONFIRM"), ("CP_BUSY_STAT", "EOP_DONE_BUSY")]),
    ("poll", [("CP_BUSY_STAT", "SEM_POLLING_FOR_PASS"), ("CP_BUSY_STAT", "SEM_FAILED_AND_HOLDING"),
              ("CP_STAT", "SEMAPHORE_BUSY"), ("CP_STALLED_STAT1", "ME_WAITING_ON_TC_READ_DATA"),
              ("CP_STALLED_STAT1", "ME_WAITING_ON_REG_READ_DATA"), ("CP_STALLED_STAT1", "ME_STALLED_ON_ATOMIC_RTN_DATA")]),
    ("write", [("CP_STALLED_STAT1", "ME_STALLED_ON_TC_WR_CONFIRM"), ("CP_STALLED_STAT2", "PFP_STALLED_ON_TC_WR_CONFIRM"),
               ("CP_STALLED_STAT2", "PFP_STALLED_ON_ATOMIC_RTN_DATA")]),
    ("fetch", [("CP_STALLED_STAT2", "PFP_WAITING_ON_BUFFER_DATA"), ("CP_STALLED_STAT2", "ME_WAITING_DATA_FROM_PFP"),
               ("CP_CPF_STALLED_STAT1", "RING_FETCHING_DATA"), ("CP_CPF_STALLED_STAT1", "INDR1_FETCHING_DATA"),
               ("CP_CPF_STALLED_STAT1", "INDR2_FETCHING_DATA"), ("CP_CPF_STALLED_STAT1", "STATE_FETCHING_DATA"),
               ("CP_CPF_STALLED_STAT1", "DATA_FETCHING_DATA"), ("CP_CPF_STALLED_STAT1", "GFX_UTCL1_WAITING_ON_TRANS"),
               ("CP_STALLED_STAT3", "UTCL1_WAITING_ON_TRANS")]),
    ("parse", [("CP_BUSY_STAT", "ME_PARSING_PACKETS"), ("CP_BUSY_STAT", "PFP_PARSING_PACKETS"),
               ("CP_STAT", "ME_BUSY"), ("CP_STAT", "PFP_BUSY"), ("CP_STAT", "MEQ_BUSY")]),
]
COMBO_REGS = ["CP_STAT", "CP_BUSY_STAT", "CP_STALLED_STAT1", "CP_STALLED_STAT2", "CP_CPF_STALLED_STAT1"]
MEMORY_FIELDS = [("GRBM_STATUS2", "EA_BUSY"), ("GRBM_STATUS2", "UTCL2_BUSY"), ("GRBM_STATUS2", "TCP_BUSY"),
                 ("GRBM_STATUS3", "GL2CC_BUSY"), ("GRBM_STATUS3", "GL1CC_BUSY"), ("GRBM_STATUS3", "CH_BUSY")]
TOP = ["idle", "pipeline", "cp_only", "other_active"]


def load_masks(path):
    """{register: [(field, mask, shift)]} from an amdgpu *_sh_mask.h."""
    rx = re.compile(r"^#define\s+(\w+?)__(\w+)_MASK\s+(0x[0-9A-Fa-f]+)L?\s*$")
    out = collections.defaultdict(list)
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        m = rx.match(line)
        if m:
            mask = int(m.group(3), 16)
            out[m.group(1)].append((m.group(2), mask, (mask & -mask).bit_length() - 1))
    return out


class Run:
    def __init__(self, path):
        data = Path(path).read_bytes()
        h = HEADER.unpack_from(data, 0)
        (magic, self.version, header_bytes, regdesc_bytes, self.record_bytes, self.nregs, self.hz, self.seconds,
         self.nsamples, self.late, self.failures, self.slow_reads, self.freq, self.qpc_start, self.qpc_end,
         self.ft_start, self.ft_end, self.gfx_index_start, self.gfx_index_end, self.flags, self.pid, setname,
         self.max_read_ticks, _) = h
        if magic != b"GTL1" or header_bytes != HEADER.size or regdesc_bytes != REGDESC.size:
            raise ValueError(f"{path}: not a GTL1 file this analyzer knows (magic {magic!r}, header {header_bytes})")
        if self.record_bytes != 16 + 4 * self.nregs:
            raise ValueError(f"{path}: record size {self.record_bytes} does not match {self.nregs} registers")
        self.set = setname.split(b"\0")[0].decode()
        pos = HEADER.size
        self.regs = []
        for _ in range(self.nregs):
            off, name = REGDESC.unpack_from(data, pos)
            self.regs.append((off, name.split(b"\0")[0].decode()))
            pos += REGDESC.size
        need = pos + self.nsamples * self.record_bytes
        if len(data) < need:
            raise ValueError(f"{path}: truncated ({len(data)} bytes, {need} expected)")
        rec = struct.Struct(f"<QII{self.nregs}I")
        self.qpc, self.ticks, self.status, self.values = [], [], [], []
        for i in range(self.nsamples):
            r = rec.unpack_from(data, pos + i * self.record_bytes)
            self.qpc.append(r[0])
            self.ticks.append(r[1])
            self.status.append(r[2])
            self.values.append(r[3:])
        names = [n for _, n in self.regs]
        self.index = {}
        for i, n in enumerate(names):
            self.index.setdefault(n, i)
        self.bracket = len(names) - 1 if names.count("GRBM_STATUS") == 2 and names[-1] == "GRBM_STATUS" else None

    def t(self, i):
        """Seconds since the run's start, at the middle of sample i's reads."""
        return (self.qpc[i] + self.ticks[i] / 2 - self.qpc_start) / self.freq


class Classifier:
    def __init__(self, run, masks):
        self.run, self.masks = run, masks
        rules = DRAIN_WHILE_PIPE + MEMORY_FIELDS + [p for _, pairs in CP_REASONS for p in pairs]
        self.missing = {reg for reg, _ in rules if reg not in run.index}

    def mask(self, reg, field):
        for f, m, _ in self.masks.get(reg, []):
            if f == field:
                return m
        raise KeyError(f"{reg}.{field} not in the mask header")

    def test(self, v, pairs):
        for reg, field in pairs:
            i = self.run.index.get(reg)
            if i is None:
                continue
            if v[i] & self.mask(reg, field):
                return True
        return False

    def grbm_class(self, g):
        if not g & self.mask("GRBM_STATUS", "GUI_ACTIVE"):
            return "idle"
        if any(g & self.mask("GRBM_STATUS", f) for f in PIPE_FIELDS):
            return "pipeline"
        if g & (self.mask("GRBM_STATUS", "CP_BUSY") | self.mask("GRBM_STATUS", "CP_COHERENCY_BUSY")):
            return "cp_only"
        return "other_active"

    def classify(self, v):
        top = self.grbm_class(v[0])
        if top == "pipeline":
            return top, "drain" if self.test(v, DRAIN_WHILE_PIPE) else "fed"
        if top == "cp_only":
            for name, pairs in CP_REASONS:
                if self.test(v, pairs):
                    return top, name
            return top, "other"
        return top, ""

    def pipe_kind(self, g):
        draw = any(g & self.mask("GRBM_STATUS", f) for f in DRAW_FIELDS)
        shader = any(g & self.mask("GRBM_STATUS", f) for f in SHADER_FIELDS)
        return "draw (PA/SC/DB/CB)" if draw else "shader only (SPI/TA)" if shader else "front only (GE/SX/BCI/GDS)"

    def bits(self, reg, value):
        return [f for f, m, _ in self.masks.get(reg, []) if m & (m - 1) == 0 and value & m]


def pct(v, q):
    if not v:
        return 0.0
    v = sorted(v)
    return v[min(len(v) - 1, int(q * len(v)))]


def weights(run, idx):
    """Time each kept sample stands for (half the gap to each neighbour), capped at three nominal periods so that a
    late wake or a failed read does not let one sample speak for a long stretch."""
    nominal = 1.0 / run.hz
    ts = [run.t(i) for i in idx]
    w = []
    for k in range(len(ts)):
        left = ts[k] - ts[k - 1] if k > 0 else None
        right = ts[k + 1] - ts[k] if k + 1 < len(ts) else None
        gap = (left + right) / 2 if left is not None and right is not None else (left or right or nominal)
        w.append(min(gap, 3 * nominal))
    return ts, w


def runs_of(flags, ts, period):
    """Lengths (s) of the maximal runs of True in a time-ordered boolean list."""
    out, start = [], None
    for k, f in enumerate(flags):
        if f and start is None:
            start = k
        elif not f and start is not None:
            out.append((k - start) * period)
            start = None
    if start is not None:
        out.append((len(flags) - start) * period)
    return out


def analyze(run, masks, args):
    cl = Classifier(run, masks)
    t0, t1 = (args.window if args.window else (0.0, float("inf")))
    idx = [i for i in range(run.nsamples) if run.status[i] == 0 and t0 <= run.t(i) < t1]
    failed = sum(1 for i in range(run.nsamples) if run.status[i] != 0)
    if not idx:
        raise SystemExit("no valid samples in the window")
    ts, w = weights(run, idx)
    wsum = sum(w)
    span = ts[-1] - ts[0] if len(ts) > 1 else 0.0
    res = {"file": str(args.run), "set": run.set, "samples": len(idx), "failed_reads": failed, "late": run.late,
           "flags": [n for b, n in FLAGS.items() if run.flags & b], "span_s": span,
           "rate_hz": (len(idx) - 1) / span if span > 0 else 0.0, "hz_asked": run.hz,
           "gfx_index": [run.gfx_index_start, run.gfx_index_end], "registers": [n for _, n in run.regs]}
    us = 1e6 / run.freq
    rt = [run.ticks[i] * us for i in idx]
    res["read_us"] = {"mean": sum(rt) / len(rt), "p50": pct(rt, 0.5), "p99": pct(rt, 0.99), "max": max(rt)}

    top_n = collections.Counter()
    top_w = collections.Counter()
    sub_w = collections.Counter()
    kind_w = collections.Counter()
    combos = {"cp_only": collections.Counter(), "pipeline/drain": collections.Counter()}
    mem_w = collections.Counter()
    values_by_class = {c: [collections.Counter() for _ in run.regs] for c in TOP}
    bracket_changes = 0
    anomalies = collections.Counter()
    classes = []
    for k, i in enumerate(idx):
        v = run.values[i]
        top, sub = cl.classify(v)
        classes.append((top, sub))
        top_n[top] += 1
        top_w[top] += w[k]
        if sub:
            sub_w[f"{top}/{sub}"] += w[k]
        if top == "pipeline":
            kind_w[cl.pipe_kind(v[0])] += w[k]
        if top in ("cp_only",) or (top, sub) == ("pipeline", "drain"):
            key = top if top == "cp_only" else "pipeline/drain"
            combo = " ".join(f"{r}.{b}" for r in COMBO_REGS if r in run.index for b in cl.bits(r, v[run.index[r]]))
            combos[key][(sub, combo or "(no CP bits)")] += w[k]
            if top == "cp_only" and cl.test(v, MEMORY_FIELDS):
                mem_w[sub] += w[k]
        for r, x in enumerate(v):
            values_by_class[top][r][x] += 1
        if run.bracket is not None and cl.grbm_class(v[run.bracket]) != top:
            bracket_changes += 1
        if v[0] == 0xFFFFFFFF:
            anomalies["GRBM_STATUS all ones (device not answering)"] += 1
        if top == "idle":
            cs = run.index.get("CP_STAT")
            if cs is not None and v[cs] & cl.mask("CP_STAT", "CP_BUSY"):
                anomalies["idle with CP_STAT.CP_BUSY"] += 1
            if v[0] & sum(cl.mask("GRBM_STATUS", f) for f in PIPE_FIELDS):
                anomalies["idle with a pipeline bit"] += 1

    res["top_share"] = {c: top_w[c] / wsum for c in TOP}
    res["top_share_unweighted"] = {c: top_n[c] / len(idx) for c in TOP}
    res["sub_share"] = {k: x / wsum for k, x in sorted(sub_w.items())}
    res["pipeline_kind_share"] = {k: x / wsum for k, x in sorted(kind_w.items())}
    res["cp_only_with_memory_busy"] = {k: x / wsum for k, x in sorted(mem_w.items())}
    res["bracket_change_share"] = bracket_changes / len(idx) if run.bracket is not None else None
    res["anomalies"] = dict(anomalies)
    res["inputs_missing_for_rules"] = sorted(cl.missing)

    # Batch means: the error bar that respects correlation between neighbouring samples.
    batch = args.batch_s
    nb = max(1, int(span // batch))
    per = [collections.Counter() for _ in range(nb)]
    per_w = [0.0] * nb
    for k in range(len(idx)):
        b = min(nb - 1, int((ts[k] - ts[0]) // batch))
        per[b][classes[k][0]] += w[k]
        per_w[b] += w[k]
    err = {}
    for c in TOP:
        xs = [per[b][c] / per_w[b] for b in range(nb) if per_w[b] > 0]
        if len(xs) > 1:
            m = sum(xs) / len(xs)
            sd = math.sqrt(sum((x - m) ** 2 for x in xs) / (len(xs) - 1))
            err[c] = {"stderr": sd / math.sqrt(len(xs)), "batch_min": min(xs), "batch_max": max(xs)}
    res["batches"] = {"seconds": batch, "count": nb, "per_class": err}

    # Idle and busy runs: does the GPU idle in a few long gaps (a starved frame) or in many short bubbles?
    period = 1.0 / res["rate_hz"] if res["rate_hz"] else 1.0 / run.hz
    idle_runs = runs_of([c[0] == "idle" for c in classes], ts, period)
    busy_runs = runs_of([c[0] != "idle" for c in classes], ts, period)
    buckets = [(0, 0.0015), (0.0015, 0.004), (0.004, 0.008), (0.008, 0.016), (0.016, 1e9)]
    res["idle_runs"] = {
        "per_s": len(idle_runs) / span if span else 0.0,
        "p50_ms": pct(idle_runs, 0.5) * 1e3, "p90_ms": pct(idle_runs, 0.9) * 1e3,
        "time_share_by_length": {f"{lo * 1e3:g}-{hi * 1e3:g} ms" if hi < 1e8 else f">={lo * 1e3:g} ms":
                                 sum(x for x in idle_runs if lo <= x < hi) / max(1e-12, sum(idle_runs))
                                 for lo, hi in buckets}}
    res["busy_runs"] = {"per_s": len(busy_runs) / span if span else 0.0,
                        "p50_ms": pct(busy_runs, 0.5) * 1e3, "p90_ms": pct(busy_runs, 0.9) * 1e3}

    # Per-register field shares, overall and per class (single-bit fields; multi-bit fields as a mean).
    marg = {}
    for r, (_, name) in enumerate(run.regs):
        if run.bracket is not None and r == run.bracket:
            continue
        fields = {}
        for f, m, sh in masks.get(name, []):
            entry = {}
            for c in TOP:
                cnt = values_by_class[c][r]
                n = sum(cnt.values())
                if not n:
                    continue
                if m & (m - 1) == 0:
                    entry[c] = sum(x for val, x in cnt.items() if val & m) / n
                else:
                    entry[c] = sum(((val & m) >> sh) * x for val, x in cnt.items()) / n
            total = sum(top_n[c] * entry.get(c, 0.0) for c in TOP) / len(idx)
            if total or any(entry.values()):
                fields[f] = {"all": total, **entry, "multibit": m & (m - 1) != 0}
        if fields:
            marg[name] = fields
    res["fields"] = marg

    # Other engines and progress.
    for name in ("SDMA0_STATUS_REG", "SDMA1_STATUS_REG"):
        i = run.index.get(name)
        if i is not None:
            m = cl.mask(name, "IDLE")
            res[f"{name}_busy_share"] = sum(1 for k in idx if not run.values[k][i] & m) / len(idx)
    i = run.index.get("CP_RB0_RPTR")
    if i is not None:
        moves = sum(1 for a, b in zip(idx, idx[1:]) if run.values[a][i] != run.values[b][i])
        res["rb0_rptr_moves_per_s"] = moves / span if span else 0.0
    se0, se1 = run.index.get("GRBM_STATUS_SE0"), run.index.get("GRBM_STATUS_SE1")
    if se0 is not None and se1 is not None:
        m = cl.mask("GRBM_STATUS_SE0", "SPI_BUSY")
        res["se_spi_busy_share"] = [sum(1 for k in idx if run.values[k][se0] & m) / len(idx),
                                    sum(1 for k in idx if run.values[k][se1] & m) / len(idx)]
        res["se0_equals_se1_share"] = sum(1 for k in idx if run.values[k][se0] == run.values[k][se1]) / len(idx)

    res["top_combos"] = {k: [{"reason": s, "bits": b, "share": x / wsum} for (s, b), x in c.most_common(args.top)]
                         for k, c in combos.items()}

    # Per frame.
    if args.fps:
        ms = 1000.0 / args.fps
        res["ms_per_frame_at_fps"] = {"fps": args.fps, "frame_ms": ms,
                                      **{c: res["top_share"][c] * ms for c in TOP},
                                      **{k: x * ms for k, x in res["sub_share"].items()}}
    if args.frames:
        res["frames"] = per_frame(run, idx, classes, args)
    return res


def read_frames(path, freq, qpc_start):
    """Frame boundaries as run seconds. A line holds one number: an integer is a QPC tick, a number with a decimal
    point is seconds on the QPC clock (ticks / frequency). '#' starts a comment."""
    out = []
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        x = float(line) * freq if "." in line or "e" in line.lower() else int(line)
        out.append((x - qpc_start) / freq)
    return sorted(out)


def per_frame(run, idx, classes, args):
    bounds = read_frames(args.frames, run.freq, run.qpc_start)
    ts = [run.t(i) for i in idx]
    rows, k = [], 0
    for a, b in zip(bounds, bounds[1:]):
        while k < len(ts) and ts[k] < a:
            k += 1
        j = k
        cnt = collections.Counter()
        while j < len(ts) and ts[j] < b:
            cnt[classes[j][0]] += 1
            cnt[f"{classes[j][0]}/{classes[j][1]}"] += 1
            j += 1
        n = sum(cnt[c] for c in TOP)
        if n >= 3:
            dur = (b - a) * 1e3
            rows.append({"start_s": a, "ms": dur, "samples": n,
                         **{c: cnt[c] / n * dur for c in TOP},
                         **{key: x / n * dur for key, x in cnt.items() if "/" in key and not key.endswith("/")}})
    if args.frames_csv and rows:
        keys = sorted({key for r in rows for key in r})
        lines = [",".join(keys)] + [",".join(f"{r.get(key, 0):.4f}" for key in keys) for r in rows]
        Path(args.frames_csv).write_text("\n".join(lines) + "\n", encoding="utf-8")
    summary = {"frames": len(rows), "skipped_short": max(0, len(bounds) - 1 - len(rows))}
    for key in ["ms"] + TOP + sorted({key for r in rows for key in r if "/" in key}):
        xs = [r.get(key, 0.0) for r in rows]
        if xs:
            summary[key] = {"mean": sum(xs) / len(xs), "p10": pct(xs, 0.1), "p50": pct(xs, 0.5), "p90": pct(xs, 0.9)}
    return summary


def report(res, out=sys.stdout):
    p = lambda *a: print(*a, file=out)  # noqa: E731
    p(f"{res['file']}: set {res['set']}, {res['samples']} samples over {res['span_s']:.2f} s = {res['rate_hz']:.0f}/s"
      f" (asked {res['hz_asked']}), failed reads {res['failed_reads']}, late periods {res['late']}"
      f"{', ' + ', '.join(res['flags']) if res['flags'] else ''}")
    r = res["read_us"]
    p(f"read cost per sample: mean {r['mean']:.1f} us, p50 {r['p50']:.1f}, p99 {r['p99']:.1f}, max {r['max']:.1f};"
      f" GRBM_GFX_INDEX 0x{res['gfx_index'][0]:08X} -> 0x{res['gfx_index'][1]:08X}")
    p("")
    p("GPU time (time-weighted; unweighted in brackets; +- batch-means standard error):")
    for c in TOP:
        e = res["batches"]["per_class"].get(c)
        err = f" +- {e['stderr'] * 100:.1f}" if e else ""
        p(f"  {c:<14} {res['top_share'][c] * 100:6.1f} %{err}   ({res['top_share_unweighted'][c] * 100:.1f})")
    for k, x in res["sub_share"].items():
        p(f"    {k:<24} {x * 100:6.1f} %")
    if res["pipeline_kind_share"]:
        p("  pipeline by what is busy: " + ", ".join(f"{k} {x * 100:.1f} %" for k, x in res["pipeline_kind_share"].items()))
    if res["cp_only_with_memory_busy"]:
        p("  cp_only with EA/UTCL2/TCP/GL2/GL1/CH busy: " +
          ", ".join(f"{k} {x * 100:.1f} %" for k, x in res["cp_only_with_memory_busy"].items()))
    if res["bracket_change_share"] is not None:
        p(f"  class changed between the first and last GRBM_STATUS of a sample: {res['bracket_change_share'] * 100:.2f} %")
    if res["anomalies"]:
        p("  anomalies: " + ", ".join(f"{k} {v}" for k, v in res["anomalies"].items()))
    if res["inputs_missing_for_rules"]:
        p("  registers some rules need but this set did not read: " + ", ".join(res["inputs_missing_for_rules"]))
    if "ms_per_frame_at_fps" in res:
        m = res["ms_per_frame_at_fps"]
        p("")
        p(f"per frame at {m['fps']:.1f} fps ({m['frame_ms']:.2f} ms): " +
          ", ".join(f"{c} {m[c]:.2f} ms" for c in TOP))
        p("  " + ", ".join(f"{k} {x:.2f}" for k, x in m.items() if "/" in k))
    if "frames" in res:
        f = res["frames"]
        p("")
        p(f"per frame from boundaries: {f['frames']} frames ({f['skipped_short']} with under 3 samples skipped)")
        for key, s in f.items():
            if isinstance(s, dict):
                p(f"  {key:<24} mean {s['mean']:6.2f} ms  p10 {s['p10']:6.2f}  p50 {s['p50']:6.2f}  p90 {s['p90']:6.2f}")
    ir, br = res["idle_runs"], res["busy_runs"]
    p("")
    p(f"idle runs: {ir['per_s']:.1f}/s, p50 {ir['p50_ms']:.1f} ms, p90 {ir['p90_ms']:.1f} ms; idle time by run length: " +
      ", ".join(f"{k} {x * 100:.0f} %" for k, x in ir["time_share_by_length"].items()))
    p(f"busy runs: {br['per_s']:.1f}/s, p50 {br['p50_ms']:.1f} ms, p90 {br['p90_ms']:.1f} ms")
    extra = []
    for name in ("SDMA0_STATUS_REG", "SDMA1_STATUS_REG"):
        if f"{name}_busy_share" in res:
            extra.append(f"{name.split('_')[0]} busy {res[name + '_busy_share'] * 100:.1f} %")
    if "rb0_rptr_moves_per_s" in res:
        extra.append(f"CP_RB0_RPTR moved between samples {res['rb0_rptr_moves_per_s']:.0f}/s")
    if "se_spi_busy_share" in res:
        a, b = res["se_spi_busy_share"]
        extra.append(f"SPI busy SE0 {a * 100:.1f} % SE1 {b * 100:.1f} % (SE0 == SE1 in {res['se0_equals_se1_share'] * 100:.0f} %)")
    if extra:
        p("; ".join(extra))
    for key, rows in res["top_combos"].items():
        if rows:
            p("")
            p(f"top CP bit combinations in {key} (share of all time):")
            for row in rows:
                p(f"  {row['share'] * 100:6.2f} %  [{row['reason']}] {row['bits']}")
    p("")
    p("field shares (all / idle / pipeline / cp_only), fields set in at least 0.5 % of samples:")
    for reg, fields in res["fields"].items():
        shown = [(f, e) for f, e in fields.items() if e["multibit"] or e["all"] >= 0.005]
        if not shown:
            continue
        p(f"  {reg}")
        for f, e in shown:
            fmt = (lambda x: f"{x:6.2f}") if e["multibit"] else (lambda x: f"{x * 100:5.1f}%")
            p(f"    {f:<34} " + " ".join(fmt(e.get(c, 0.0)) for c in ("all", "idle", "pipeline", "cp_only")) +
              ("  (mean value)" if e["multibit"] else ""))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run")
    ap.add_argument("--fps", type=float, help="frames per second over the same window: shares become ms per frame")
    ap.add_argument("--frames", help="frame boundaries on the QPC clock, one per line (ticks, or seconds with a '.')")
    ap.add_argument("--frames-csv", help="per-frame rows (ms by class) to this CSV")
    ap.add_argument("--window", type=float, nargs=2, metavar=("S0", "S1"), help="only samples S0 <= t < S1 s into the run")
    ap.add_argument("--batch-s", type=float, default=1.0, help="batch length for the error bar (default 1 s)")
    ap.add_argument("--top", type=int, default=8, help="bit combinations to list per CP class")
    ap.add_argument("--json", help="write the full result as JSON")
    ap.add_argument("--mask-header", default=str(DEFAULT_MASK_HEADER))
    args = ap.parse_args(argv)
    run = Run(args.run)
    res = analyze(run, load_masks(args.mask_header), args)
    report(res)
    if args.json:
        Path(args.json).write_text(json.dumps(res, indent=1), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
