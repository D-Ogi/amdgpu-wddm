using System;
using System.IO;
using AmdgpuWddmControl;

partial class UnitTests
{
    static void BoardCapabilityTests(string repo, string header)
    {
        int size;
        string flatHeader = header.Replace("BC250_BOARD_GPU_POINT GpuPoints[16]", "unsigned long GpuPoints[48]")
            .Replace("BC250_BOARD_FAN_POINT FanPresets[3][8]", "unsigned long FanPresets[48]");
        var layout = Layout(flatHeader, "BC250_ESCAPE_BOARD_CAPS", out size);
        Equal(BoardCapabilities.Bytes, size, "board capability ABI size");
        string[] fields = ("Magic Command Status NtStatus AbiVersion ProviderId Flags Reason " +
            "GpuMinMHz GpuFloorMHz GpuMaxMHz GpuStepMHz GpuPointCount GpuMinMv GpuMaxMv GpuUndervoltMv GpuHotMc " +
            "CpuMinMHz CpuMaxMHz CpuLabMaxMHz CpuStepMHz CpuUvMaxSteps CpuTempMinC CpuTempMaxC CpuRefuseMv CpuStockMask CpuFullMask CpuCoreCount " +
            "FanMinC FanMaxC FanFloorPct FanFullPct FanEmergencyMc FanPointMin FanPointMax FanLeaseMinMs FanLeaseMaxMs FanLeaseDefaultMs").Split(' ');
        for (int i = 0; i < fields.Length; i++) Equal(i * 4, layout[fields[i]], "capability offset " + fields[i]);
        Equal(152, layout["GpuPoints"], "capability GPU vector offset");
        Equal(344, layout["FanPresetCounts"], "capability preset counts offset");
        Equal(356, layout["FanPresets"], "capability preset vector offset");
        Equal(548, layout["GpuDefaultMaxMHz"], "capability default ceiling offset");
        Equal(552, layout["CpuStockCoreCount"], "capability stock count offset");
        Equal(556, layout["Reserved"], "capability reserved offset");
        var raw = File.ReadAllBytes(Environment.GetEnvironmentVariable("AMDGPU_WDDM_BOARD_FIXTURE"));
        var valid = BoardCapabilities.Parse(raw);
        BoardCapabilities.Replace(null);
        Check(!BoardCapabilities.Current.Allows(BoardCapabilities.Fan), "no implicit legacy board admission");
        Check(Tuner.Points == 0 && DpmSettings.CeilingChoices.Length == 0, "no fallback ranges");
        foreach (string action in new[] { "fan-auto", "fan-boost-off", "tune-reset", "cpu-disable", "core-mask", "set-clocks", "cu-confirm", "undo", "reset-defaults" })
            Check(BoardCapabilities.ActionPermission(action) != 0, "elevated action gated " + action);
        var snapshot = new RecoverySnapshot { DriverInstalled = true };
        foreach (string action in new[] { "fan-auto", "fan-test", "cpu-enable", "core-mask", "set-clocks", "tune-reset", "undo", "reset-defaults" })
            Check(Recovery.Plan(action, snapshot).Refusal != null, "unknown board refuses actual plan " + action);
        Check(!CpuTuning.ValidClock(2800) && !CpuTuning.ValidSteps(0) && !CpuTuning.ValidMask(0), "unknown CPU range never admits zero");
        int rejectedPoint;
        Check(Tuner.Check(new uint[0], null, out rejectedPoint) != CurveError.Ok, "unknown empty curve refused");
        Check(FanCurves.Check(new uint[0], new uint[0], out rejectedPoint) != FanCurveError.Ok, "unknown empty fan refused");
        for (int i = 0; i < 192; i++) {
            if (i < 139 && i != 4 && i != 5 && i != 6 && i != 12 && i != 86) continue;
            byte[] bad = (byte[])raw.Clone();
            Array.Copy(BitConverter.GetBytes(uint.MaxValue), 0, bad, i*4, 4);
            bool refused = false;
            try { BoardCapabilities.Parse(bad); } catch (FormatException) { refused = true; }
            Check(refused, "malformed capability word " + i);
        }
        var unknown = new byte[BoardCapabilities.Bytes];
        Array.Copy(raw, unknown, 20);
        Array.Copy(BitConverter.GetBytes(1u), 0, unknown, 28, 4);
        Check(!BoardCapabilities.Parse(unknown).Allows(BoardCapabilities.Memory), "unknown board unsupported");
        var inactive = (byte[])raw.Clone();
        Array.Copy(BitConverter.GetBytes(1u), 0, inactive, 24, 4);
        Array.Copy(BitConverter.GetBytes(2u), 0, inactive, 28, 4);
        Check(!BoardCapabilities.Parse(inactive).Allows(BoardCapabilities.Gpu), "inactive board cannot mutate");
        BoardCapabilities.Replace(valid);
        Check(valid.Allows(30), "actual provider fixture allows supported controls");
        Check(Tuner.Table().Length == valid.GpuClocks.Length && Tuner.FloorMv == valid[13], "curve uses provider");
        Check(CpuTuning.MinMHz == valid[17] && FanCurves.FloorPct == valid[30], "CPU/fan use provider");
    }
}
