"""Host test of dpm_trace_summary.py (BD-329): run with `python -m unittest discover -s tools/win/bc250kmd_cli`."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dpm_trace_summary as s  # noqa: E402

HEAD = ("t_ms,ticks,uptime_ms,mode,mhz,target_mhz,want_mhz,cap_mhz,max_mhz,smu_mhz,smu_vid,busy_pm,avg_pm,src,"
        "submit_pm,sdma_pm,temp_mc,temp_ok,throttle,idle,compute,stable,paused,raises,lowers,thermal,errors,power_mw")


def row(t, mhz, busy, temp=65000, throttle="none", raises=0, src="grbm"):
    return "%d,%d,%d,dpm,%d,%d,%d,2000,2000,%d,116,%d,%d,%s,0,0,%d,1,%s,0,0,0,0,%d,0,0,0," % (
        t, t // 25, t, mhz, mhz, mhz, mhz, busy, busy, src, temp, throttle, raises)


def trace(rows):
    return ["# dpm trace: start 2026-11-08T00:00:00.000Z, 20 s, every 50 ms", HEAD] + rows + ["# dpm trace: n rows"]


class Summary(unittest.TestCase):
    def test_ramp_pass(self):
        rows = [row(t, 1000, 0) for t in range(0, 2000, 50)]
        mhz = 1000
        for t in range(2000, 14000, 50):
            if t % 1000 == 0 and mhz < 1800:
                mhz += 100
            rows.append(row(t, mhz, 950))
        lines, verdict = s.summarize(s.read_rows(trace(rows)), reach=1700, within=10)
        self.assertTrue(verdict)
        self.assertTrue(any(l.startswith("BD329-RAMP PASS reached 1700 MHz") for l in lines))
        self.assertTrue(any(l.startswith("load: 2.00 s") for l in lines))

    def test_no_ramp_fail_reports_the_band(self):
        rows = [row(t, 1000, 0) for t in range(0, 2000, 50)]
        for t in range(2000, 13000, 50):
            rows.append(row(t, 1100 if t > 6000 else 1000, 860 if t % 100 else 820, 71000, "thermal-ramp"))
        lines, verdict = s.summarize(s.read_rows(trace(rows)), reach=1700, within=10)
        self.assertFalse(verdict)
        self.assertTrue(any(l.startswith("BD329-RAMP FAIL highest 1100 MHz") for l in lines))
        band = next(l for l in lines if l.startswith("rows at or under 1300 MHz"))
        self.assertIn(">=900 0", band)
        self.assertIn("800-899 220", band)
        self.assertTrue(any("thermal-ramp 220" in l for l in lines))

    def test_submit_source_is_counted(self):
        rows = [row(t, 1000, 700, src="submit") for t in range(0, 3000, 50)]
        lines, _ = s.summarize(s.read_rows(trace(rows)))
        self.assertTrue(any(l == "busy source in the load: submit 60" for l in lines))

    def test_no_load(self):
        lines, verdict = s.summarize(s.read_rows(trace([row(t, 500, 0) for t in range(0, 3000, 50)])), reach=1700)
        self.assertFalse(verdict)
        self.assertTrue(lines[-1].startswith("load: none"))

    def test_not_a_trace(self):
        with self.assertRaises(ValueError):
            s.read_rows(["a,b", "1,2"])


if __name__ == "__main__":
    unittest.main()
