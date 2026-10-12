"""Extract the production recovery persistence functions for fake-kernel tests."""
import argparse
from pathlib import Path


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    kmd = args.root / "driver/kmd"
    guard = (kmd / "guard.c").read_text(encoding="utf-8")
    gfx = (kmd / "gfx.c").read_text(encoding="utf-8")
    wddm = (kmd / "wddm.c").read_text(encoding="utf-8")
    reset = function(wddm, "static NTSTATUS Bc250WddmResetEngine(")
    ordered = [
        "admittedAt = KeQueryInterruptTime();",
        "GuardRecordHangRecovery(BC250_HANG_VERDICT_PENDING, seq, hungFence, 0, 0, wddm->HangRecoveryWaves);",
        "if (wddm->HangRecoveryWaves) GfxRecordWavesBeforeKill(device, seq, hungFence, vmid);",
        'GuardLog("wddm: soft recovery admit-to-kill',
        "verdict = GfxSoftRecover(device, vmid, &seq, &kills, &micros);",
    ]
    position = -1
    for marker in ordered:
        position = reset.index(marker, position + 1)
    assert wddm.count('GuardReadSetting(L"HangRecoveryWaves", 1)') == 1
    for signature in ("static BOOLEAN WddmSubmitProgress(", "static void WddmTimeoutSnapshot("):
        sample = function(wddm, signature)
        assert sample.index("GfxWaveSampleBegin(") < sample.index("MmioRead(") < sample.index("GfxWaveSampleEnd(")
        assert sample.count("GfxWaveSampleEnd(") == 1
    functions = [function(guard, "static NTSTATUS WriteDword("),
                 function(guard, "void GuardRecordHangRecovery("),
                 function(guard, "NTSTATUS GuardRecordWaveSnapshot("),
                 function(gfx, "void GfxRecordWavesBeforeKill(")]
    template = Path(__file__).with_name("wave_recovery_test.c").read_text(encoding="utf-8")
    assert template.count("/* ACTUAL_SOURCE */") == 1
    args.out.write_text(template.replace("/* ACTUAL_SOURCE */", "\n\n".join(functions)), encoding="utf-8")


if __name__ == "__main__":
    main()
