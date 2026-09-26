"""Linux peer of piglit-guard: unchanged upstream oracles, serial45s cases."""
import importlib.util
import os
from pathlib import Path
import sys
import time

spec = importlib.util.spec_from_file_location("case_guard", Path(__file__).with_name("piglit-guard.py"))
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)


class LinuxHealth:
    def __init__(self):
        candidates = []
        for node in Path("/sys/class/drm").glob("renderD*/device"):
            if (node / "vendor").read_text().strip() == "0x1002" and (node / "device").read_text().strip() == "0x13fe":
                candidates.append(node.resolve())
        if len(candidates) != 1:
            raise RuntimeError("Expected one BC-250 render device")
        self.device = candidates[0]
        self.boot = Path("/proc/sys/kernel/random/boot_id").read_text()
        self.reset = self.reset_count()
        self.last = 0

    def reset_count(self):
        path = self.device / "gpu_reset_counter"
        return path.read_text() if path.exists() else None

    def __call__(self):
        if Path(os.environ["BC250_STOP_FILE"]).exists():
            raise RuntimeError("Owner STOP")
        if time.monotonic() - self.last < 5:
            return
        if Path("/proc/sys/kernel/random/boot_id").read_text() != self.boot or self.reset_count() != self.reset:
            raise RuntimeError("GPU or boot identity changed")
        sensors = list((self.device / "hwmon").glob("hwmon*"))
        if len(sensors) != 1:
            raise RuntimeError("Expected one GPU hwmon")
        sensor = sensors[0]
        clock = int((sensor / "freq1_input").read_text())
        temp = int((sensor / "temp1_input").read_text())
        voltage = int((sensor / "in0_input").read_text())
        if clock != 1000000000 or temp >= 85000 or not 800 <= voltage <= 840:
            raise RuntimeError("GPU clock/voltage/temperature gate")
        self.last = time.monotonic()


def literal_names(text):
    names = text.splitlines()
    if not names or any(not name or name != name.strip() for name in names) or len(names) != len(set(names)):
        raise ValueError("Invalid exact-case remainder")
    return names


def main():
    from framework.test.base import Test
    import framework.programs.run as program
    import framework.profile as profile
    # Upstream --test-list treats inline '#' as a comment, but real piglit
    # names contain it (e.g. shared-#column_major). Preserve exact names.
    resume_path = os.environ.get("BC250_PIGLIT_RESUME_LIST")
    if resume_path:
        names = literal_names(Path(resume_path).read_text())
        original_load = profile.load_test_profile
        first_load = True
        def load_selected(*args, **kwargs):
            nonlocal first_load
            select = first_load
            first_load = False
            loaded = original_load(*args, **kwargs)
            if select:
                loaded.forced_test_list = names.copy()
            return loaded
        profile.load_test_profile = load_selected
        original_metadata = program._create_metadata
        def selected_metadata(args, name, forced):
            return original_metadata(args, name, names)
        program._create_metadata = selected_metadata
    gate = guard.CaseGate(os.environ["BC250_PIGLIT_EVENTS"], LinuxHealth())
    Test.execute = gate.wrap(Test.execute)
    try:
        program.run(["-1", "-j", "1", "--timeout", "45", "-p", "glx"] + sys.argv[1:])
    finally:
        gate.record({"event": "runner_end", "executed": gate.executed, "stop_reason": gate.reason})
    return 1 if gate.reason else 0


if __name__ == "__main__":
    raise SystemExit(main())
