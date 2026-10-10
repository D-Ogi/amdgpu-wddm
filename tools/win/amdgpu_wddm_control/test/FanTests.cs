// Host tests of the case fan card's pure parts (src/FanPlan.cs, KmdReply.ParseFan): the escape layout against the
// header text, the presets and the curve rules against the driver's own source, and every plan and refusal of the
// card's two actions. The driver decides; these tests keep the window's copy of its rules the same sentence.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using AmdgpuWddmControl;

static partial class UnitTests
{
    static FanState FanFixture(uint flags = FanState.FlagEnabled | FanState.FlagControlling)
    {
        uint[] c, pct;
        FanCurves.Preset(FanState.ProfileStandard, out c, out pct);
        var f = new FanState
        {
            Version = 0x000700D5, Flags = flags, Mode = FanState.ModeCurve, State = FanState.StateCurve,
            Profile = FanState.ProfileStandard, Points = (uint)c.Length,
            CurveC = new uint[KmdReply.FanCurveSlots], CurvePct = new uint[KmdReply.FanCurveSlots],
            TargetPct = 70, AppliedPct = 70, Rpm = 1180, Channel = 1, GuardMc = 60000, Generation = 5,
        };
        for (int i = 0; i < c.Length; i++) { f.CurveC[i] = c[i]; f.CurvePct[i] = pct[i]; }
        return f;
    }

    static RecoverySnapshot FanSnapshot(FanState f)
    {
        var s = Tuned();
        s.Fan = f;
        return s;
    }

    static void FanTests(string root, string header)
    {
        Strings.Language = "en";
        FanLayout(header);
        FanRequestText(root, header);
        FanModel(root);
        FanPlans();
        FanBoostPlans(header);
        FanWords();
        FanChartRules();
        FanTestPlans();
        FanSpeedOneSource();
    }

    // The chart moves points only through FanCurves.Move, so every curve it hands back passes the driver's rules.
    static void FanChartRules()
    {
        uint[] c, pct;
        FanCurves.Preset(FanState.ProfileStandard, out c, out pct);       // 40:50, 60:70, 70:85, 76:95, 80:100
        int at;
        FanCurves.Move(c, pct, 1, 90, 5);
        Equal(69u, c[1], "a dragged point stops 1 C under its right neighbour");
        Equal(50u, pct[1], "a dragged duty stops at its left neighbour");
        FanCurves.Move(c, pct, 0, 0, 0);
        Check(c[0] == FanCurves.MinC && pct[0] == FanCurves.FloorPct, "the first point stops at 20 C and the 20 % floor");
        FanCurves.Move(c, pct, 4, 200, 200);
        Check(c[4] == FanCurves.MaxC && pct[4] == FanCurves.FullPct, "the last point stops at 95 C and 100 %");
        FanCurves.Move(c, pct, 2, 30, 99);
        Check(c[2] == c[1] + 1 && pct[2] == pct[3], "a middle point stays between its neighbours");
        Equal(FanCurveError.Ok, FanCurves.Check(c, pct, out at), "a curve moved on the chart is legal");
        var r = new Random(7);
        for (int n = 0; n < 2000; n++)
        {
            int i = r.Next(c.Length);
            FanCurves.Move(c, pct, i, r.Next(0, 120), r.Next(0, 120));
            if (FanCurves.Check(c, pct, out at) != FanCurveError.Ok) { Check(false, "random moves keep the curve legal (" + FanCurves.CurveText(c, pct) + ")"); break; }
        }
        Check(true, "2000 random moves kept the curve legal");
        var before = FanCurves.CurveText(c, pct);
        FanCurves.Move(c, pct, 9, 50, 50);
        Equal(before, FanCurves.CurveText(c, pct), "a point that does not exist moves nothing");

        FanCurves.Preset(FanState.ProfileStandard, out c, out pct);
        Equal(50u, FanCurves.DutyAt(c, pct, 30), "below the first point the first duty applies");
        Equal(100u, FanCurves.DutyAt(c, pct, 90), "above the last point the last duty applies");
        Equal(78u, FanCurves.DutyAt(c, pct, 65), "65 C on the standard curve is 78 % (fan.md)");
        Equal(84u, FanCurves.DutyAt(c, pct, 69), "69 C on the standard curve is 84 % (fan.md)");
        Equal(100u, FanCurves.DutyAt(c, pct, 83), "83 C on the standard curve is 100 % (fan.md)");
        Equal(70u, FanCurves.DutyAt(c, pct, 60), "a point gives its own duty");

        var f = FanFixture(FanState.FlagEnabled | FanState.FlagControlling | FanState.FlagStored);
        f.StoredMode = FanState.ModeCurve; f.StoredProfile = FanState.ProfileStandard;
        Check(!FanCurves.Changed(f, "standard", c, pct), "the stored standard curve is no change");
        Check(FanCurves.Changed(f, "quiet", c, pct), "another preset is a change");
        Check(FanCurves.Changed(f, "board", null, null), "the board is a change while the driver runs the fan");
        var moved = (uint[])pct.Clone(); moved[0] = 45;
        Check(FanCurves.Changed(f, "custom", c, moved), "an edited curve is a change");
        Check(!FanCurves.Changed(null, "quiet", c, pct), "no reading: nothing to apply");
        var leasedNow = FanFixture(f.Flags); leasedNow.StoredMode = FanState.ModeCurve; leasedNow.StoredProfile = FanState.ProfileStandard;
        leasedNow.Mode = FanState.ModeFixed;
        Check(FanCurves.Changed(leasedNow, "standard", c, pct), "a fixed test in force makes the stored curve a change again");
    }

    // The short test: one speed from the list under a lease, then the choice in force again; refused while the driver
    // does not run the fan in the normal way.
    static void FanTestPlans()
    {
        Check(Strings.Has("plan.title.fan-test") && Strings.Has("perf.fan.refuse.busy") && Strings.Has("perf.fan.refuse.bad-test"), "the test has its words");
        var s = FanSnapshot(FanFixture());
        var p = TunePlan("fan-test", s, new Recovery.PlanArgs { FanTestPct = 60 });
        Check(!p.Refused, "a test at 60 % is planned: " + p.Refusal);
        Check(p.Tune.Kind == "fan-fixed" && p.Tune.FixedPct == 60 && p.Tune.LeaseMs == FanCurves.TestLeaseMs && p.Tune.TestMs == FanCurves.TestMs, "the plan carries the speed, the lease and the time");
        Check(p.Tune.LeaseMs > p.Tune.TestMs && p.Tune.LeaseMs >= 5000 && p.Tune.LeaseMs <= 300000, "the lease outlives the test and is one the driver takes");
        Check(p.Tune.Then != null && p.Tune.Then.Kind == "fan-curve" && p.Tune.Then.FanProfile == FanState.ProfileStandard && p.Tune.Then.FanC == null, "then the standard curve again");
        Check(p.Writes.Count == 0 && !p.Undoable && p.Preview.Count == 1, "the test writes no setting and lists one line");
        Check(!PlainWords.Findings(p.Preview.Concat(new[] { PlainPlan.Describe(p).Title })).Any(), "G-NOINT: the test dialog");
        Check(p.Text().Contains("tuning request fan-fixed, fixed 60 % for 10000 ms, lease 15000 ms, then fan-curve profile standard"), "the log names the test");
        Check(FanPlan.Arguments(new Recovery.PlanArgs { FanTestPct = 60 }).SequenceEqual(new[] { "--fan-test-pct", "60" }), "the helper gets the speed");

        var custom = FanFixture();
        custom.Profile = FanState.ProfileCustom; custom.Points = 2;
        custom.CurveC[0] = 40; custom.CurvePct[0] = 30; custom.CurveC[1] = 80; custom.CurvePct[1] = 100;
        p = TunePlan("fan-test", FanSnapshot(custom), new Recovery.PlanArgs { FanTestPct = 100 });
        Check(!p.Refused && p.Tune.Then.FanProfile == FanState.ProfileCustom && p.Tune.Then.FanC.SequenceEqual(new uint[] { 40, 80 }) &&
            p.Tune.Then.FanPct.SequenceEqual(new uint[] { 30, 100 }), "a custom curve in force comes back point for point");
        var board = FanFixture(FanState.FlagEnabled); board.Mode = FanState.ModeBoard; board.State = FanState.StateBoard;
        p = TunePlan("fan-test", FanSnapshot(board), new Recovery.PlanArgs { FanTestPct = 30 });
        Check(!p.Refused && p.Tune.Then.Kind == "fan-board", "with the board in force, the board gets the fan back");

        foreach (uint bad in new uint[] { 0, 20, 55, 101 })
            Refused(TunePlan("fan-test", s, new Recovery.PlanArgs { FanTestPct = bad }), "must be one of", "a test at " + bad + " %");
        Refused(TunePlan("fan-test", s), "must be one of", "a test without a speed");
        foreach (var flag in new[] { FanState.FlagLeased, FanState.FlagPaused, FanState.FlagFault, FanState.FlagEmergency, FanState.FlagHeldBack })
        {
            var busy = FanFixture(FanState.FlagEnabled | FanState.FlagControlling | flag);
            Check(!FanCurves.TestAllowed(busy), "no test with flag " + flag);
            var q = TunePlan("fan-test", FanSnapshot(busy), new Recovery.PlanArgs { FanTestPct = 60 });
            Check(q.Refused && q.PlainRefusal == Strings.T("perf.fan.refuse.busy"), "the test is refused in plain words with flag " + flag);
        }
        foreach (var state in new[] { FanState.StateEmergency, FanState.StateDoubt, FanState.StateFault })
        {
            var busy = FanFixture(); busy.State = state;
            Check(!FanCurves.TestAllowed(busy), "no test in state " + state);
        }
        Check(FanCurves.TestAllowed(FanFixture()), "a test is allowed while the curve runs");
        var off = FanFixture(FanState.FlagGated); off.Gate = FanState.GateSetting;
        Refused(TunePlan("fan-test", FanSnapshot(off), new Recovery.PlanArgs { FanTestPct = 60 }), "does not run the fan control", "a test without the fan control");
    }

    // G-SENS: the "Now" card and the Case fan card read the fan speed from one function, so they cannot disagree.
    static void FanSpeedOneSource()
    {
        var control = FanFixture();
        Func<HwmonState, FanState, string> row = (h, c) => Sensors.Rows(null, null, h, c).First(r => r.Id == "fan").Value;
        string level;
        Equal(Strings.T("perf.fan.rpm-only", 1180), row(null, control), "without the reader's sample the fan control's RPM is shown");
        Equal(row(null, control), Sensors.FanValue(null, control, out level), "the Now card and the fan card agree (fixture path)");
        Equal(Strings.T("perf.no-reading"), row(null, null), "no reading at all");
        var h0 = new HwmonState { Flags = HwmonState.FlagValid | HwmonState.FlagFresh };
        h0.Rpm[2] = 1240;
        Equal(Strings.T("perf.fan.rpm-only", 1240), row(h0, control), "the reader's own sample comes first");
        Equal(row(h0, control), Sensors.FanValue(h0, control, out level), "the Now card and the fan card agree (live path)");
        var stopped = new HwmonState { Flags = HwmonState.FlagValid | HwmonState.FlagFresh | HwmonState.FlagStopped };
        var sr = Sensors.Rows(null, null, stopped, control).First(r => r.Id == "fan");
        Check(sr.Level == "hot" && sr.Value == Strings.T("perf.fan.stopped"), "a stopped fan stays hot whatever the control reports");
        var zero = FanFixture(); zero.Rpm = 0;
        Check(Sensors.Rows(null, null, null, zero).First(r => r.Id == "fan").NoReading, "a zero RPM from the control is no reading");
    }

    static void FanLayout(string header)
    {
        int size;
        var fl = Layout(header, "BC250_ESCAPE_FAN", out size);
        Equal(KmdReply.FanBytes, size, "fan escape size from the header");
        Check(Regex.IsMatch(header, @"#define BC250_ESCAPE_RUN_FAN " + KmdReply.CmdFan + @"u\b"), "fan command number from the header");
        Check(Regex.IsMatch(header, @"#define BC250_FAN_ABI 1u\b"), "fan ABI 1 in the header");
        Check(Regex.IsMatch(header, @"#define BC250_FAN_CURVE_SLOTS " + KmdReply.FanCurveSlots + @"u\b"), "fan curve slots from the header");
        var words = new Dictionary<string, int>
        {
            { "Version", 3 }, { "AbiVersion", 5 }, { "Op", 6 }, { "Flags", 7 }, { "Mode", 8 }, { "State", 9 }, { "Reason", 10 },
            { "DoubtReason", 11 }, { "Profile", 12 }, { "Points", 13 }, { "CurveC", 14 }, { "CurvePct", 22 }, { "FixedPct", 30 },
            { "LeaseMs", 31 }, { "Store", 32 }, { "TargetPct", 33 }, { "AppliedPct", 34 }, { "WrittenRaw", 35 }, { "ReadbackRaw", 36 },
            { "GuardMc", 37 }, { "Rpm", 38 }, { "Channel", 39 }, { "SavedMode", 40 }, { "SavedTarget", 41 }, { "Error", 42 },
            { "StoredMode", 43 }, { "StoredProfile", 44 }, { "Gate", 45 },
        };
        foreach (var kv in words) Equal(kv.Value * 4, fl[kv.Key], "fan " + kv.Key + " offset");
        var quads = new Dictionary<string, int>
        {
            { "Takeovers", 184 }, { "Handbacks", 192 }, { "Writes", 200 }, { "Failures", 208 }, { "Emergencies", 216 },
            { "Doubts", 224 }, { "LeaseExpiries", 232 }, { "WatchdogFires", 240 }, { "Generation", 248 }, { "ExpectedGeneration", 256 },
        };
        foreach (var kv in quads) Equal(kv.Value, fl[kv.Key], "fan " + kv.Key + " offset");
        // The flag and gate values the card reads, from the header itself.
        var flags = new Dictionary<string, uint>
        {
            { "ENABLED", FanState.FlagEnabled }, { "CONTROLLING", FanState.FlagControlling }, { "EMERGENCY", FanState.FlagEmergency },
            { "LEASED", FanState.FlagLeased }, { "STORED", FanState.FlagStored }, { "FAULT", FanState.FlagFault },
            { "GATED", FanState.FlagGated }, { "PAUSED", FanState.FlagPaused }, { "RESTORE_SAVED", FanState.FlagRestoreSaved },
            { "SUBSTITUTED", FanState.FlagSubstituted }, { "HELD_BACK", FanState.FlagHeldBack },
        };
        foreach (var kv in flags) Check(Regex.IsMatch(header, @"#define BC250_FAN_FLAG_" + kv.Key + " " + kv.Value + @"u\b"), "fan flag " + kv.Key + " from the header");
        var gates = new Dictionary<string, uint> { { "OK", FanState.GateOk }, { "SETTING", FanState.GateSetting }, { "READER", FanState.GateReader }, { "CHIP", FanState.GateChip } };
        foreach (var kv in gates) Check(Regex.IsMatch(header, @"#define BC250_FAN_GATE_" + kv.Key + " " + kv.Value + @"u\b"), "fan gate " + kv.Key + " from the header");

        var b = new byte[KmdReply.FanBytes];
        Put(b, fl["Magic"], KmdReply.Magic); Put(b, fl["Command"], KmdReply.CmdFan); Put(b, fl["AbiVersion"], 1u);
        Put(b, fl["Flags"], FanState.FlagEnabled | FanState.FlagControlling | FanState.FlagStored);
        Put(b, fl["State"], FanState.StateCurve); Put(b, fl["Mode"], FanState.ModeCurve); Put(b, fl["Profile"], FanState.ProfileQuiet);
        Put(b, fl["Points"], 2u); Put(b, fl["CurveC"], 40u); Put(b, fl["CurveC"] + 4, 80u);
        Put(b, fl["CurvePct"], 30u); Put(b, fl["CurvePct"] + 4, 100u);
        Put(b, fl["Rpm"], 1360u); Put(b, fl["GuardMc"], unchecked((uint)-1500)); Put(b, fl["StoredMode"], FanState.ModeCurve);
        Put(b, fl["StoredProfile"], FanState.ProfileQuiet); Put(b, fl["Gate"], FanState.GateOk);
        Put(b, fl["Handbacks"], 3UL); Put(b, fl["WatchdogFires"], 2UL); Put(b, fl["Generation"], 9UL);
        var f = KmdReply.ParseFan(b);
        Check(f.Has(FanState.FlagStored) && f.Has(FanState.FlagControlling), "fan flags");
        Equal(FanState.ProfileQuiet, f.Profile, "fan profile"); Equal(2u, f.Points, "fan points");
        Equal("40:30,80:100", FanCurves.CurveText(FanCurves.ShownC(f), FanCurves.ShownPct(f)), "fan curve in force");
        Equal(1360u, f.Rpm, "fan rpm"); Equal(-1500, f.GuardMc, "fan guard temperature is signed");
        Equal(3UL, f.Handbacks, "fan handbacks"); Equal(2UL, f.WatchdogFires, "fan watchdog"); Equal(9UL, f.Generation, "fan generation");
        Equal("quiet", FanCurves.StoredChoice(f), "the stored choice reads as quiet");
        Put(b, fl["Points"], 9u);
        Throws<FormatException>(() => KmdReply.ParseFan(b), "a fan curve of 9 points refused");
        Put(b, fl["Points"], 2u); Put(b, fl["AbiVersion"], 2u);
        Throws<FormatException>(() => KmdReply.ParseFan(b), "fan ABI 2 refused");
        Put(b, fl["AbiVersion"], 1u); Put(b, fl["Command"], 29u);
        Throws<FormatException>(() => KmdReply.ParseFan(b), "fan wrong command refused");
        Throws<FormatException>(() => KmdReply.ParseFan(new byte[KmdReply.FanBytes - 8]), "fan short reply refused");

    }

    // Native.cs is not part of the host build (P/Invoke), so its request structure and operation numbers are checked as
    // text: the same fields in the same order as BC250_FAN_REQUEST of the DLL's source, and the header's op numbers.
    static void FanRequestText(string root, string header)
    {
        var native = File.ReadAllText(Path.Combine(root, @"tools\win\amdgpu_wddm_control\src\Native.cs"));
        var cli = File.ReadAllText(Path.Combine(root, @"tools\win\bc250kmd_cli\bc250kmd_cli.c"));
        var m = Regex.Match(native, @"public struct FanRequest\s*\{(?<body>.*?)\}", RegexOptions.Singleline);
        Check(m.Success, "Native.cs declares FanRequest");
        var c = Regex.Match(cli, @"typedef struct _BC250_FAN_REQUEST\s*\{(?<body>.*?)\}\s*BC250_FAN_REQUEST;", RegexOptions.Singleline);
        Check(c.Success, "bc250kmd_cli.c declares BC250_FAN_REQUEST");
        if (m.Success && c.Success)
        {
            Func<string, string> names = body => string.Join(",", Regex.Matches(Regex.Replace(body, @"//[^\n]*|\[MarshalAs[^\]]*\]", ""),
                @"(?:uint|ulong|ULONG|ULONGLONG)(?:\[\])?\s+(?<list>[\w\s,\[\]]+?);").Cast<Match>()
                .SelectMany(x => x.Groups["list"].Value.Split(',').Select(n => Regex.Replace(n, @"\[.*?\]", "").Trim())));
            Equal(names(c.Groups["body"].Value), names(m.Groups["body"].Value), "FanRequest has the fields of BC250_FAN_REQUEST in its order");
            Check(Regex.IsMatch(m.Groups["body"].Value, @"SizeConst = 8\)\] public uint\[\] CurveC;") &&
                Regex.IsMatch(m.Groups["body"].Value, @"SizeConst = 8\)\] public uint\[\] CurvePct;"), "FanRequest carries 8 curve slots inline");
            Check(native.Contains("[StructLayout(LayoutKind.Sequential, Pack = 8)]\n        public struct FanRequest") ||
                native.Contains("[StructLayout(LayoutKind.Sequential, Pack = 8)]\r\n        public struct FanRequest"), "FanRequest is sequential with 8-byte packing");
        }
        Check(Regex.IsMatch(cli, @"sizeof\(BC250_FAN_REQUEST\) == 104"), "the DLL holds BC250_FAN_REQUEST at 104 bytes");
        var ops = Regex.Match(native, @"FanOpRead = (\d+), FanOpBoard = (\d+), FanOpCurve = (\d+), FanOpFixed = (\d+), FanOpRenew = (\d+);");
        Check(ops.Success, "Native.cs names the five fan operations");
        if (ops.Success)
        {
            var names = new[] { "READ", "BOARD", "CURVE", "FIXED", "RENEW" };
            for (int i = 0; i < names.Length; i++)
                Check(Regex.IsMatch(header, @"#define BC250_FAN_OP_" + names[i] + " " + ops.Groups[i + 1].Value + @"u\b"), "fan op " + names[i] + " from the header");
        }
    }

    // The presets and the bounds come from the driver's own source, never from memory.
    static void FanModel(string root)
    {
        var hpath = Path.Combine(root, @"driver\shim\include\bc250_fan.h");
        var cpath = Path.Combine(root, @"driver\shim\bc250_fan.c");
        Check(File.Exists(hpath) && File.Exists(cpath), "driver/shim fan sources exist");
        if (!File.Exists(hpath) || !File.Exists(cpath)) return;
        var h = File.ReadAllText(hpath);
        Func<string, uint> def = name =>
        {
            var m = Regex.Match(h, @"#define " + name + @"\s+(\d+)u?\b");
            return m.Success ? uint.Parse(m.Groups[1].Value) : uint.MaxValue;
        };
        Equal(FanCurves.MinPoints, def("BC250_FAN_POINTS_MIN"), "fewest curve points");
        Equal(FanCurves.MaxPoints, def("BC250_FAN_POINTS_MAX"), "most curve points");
        Equal(FanCurves.FloorPct, def("BC250_FAN_FLOOR_PCT"), "the duty floor");
        Equal(FanCurves.FullPct, def("BC250_FAN_FULL_PCT"), "full speed");
        Equal(FanCurves.MinC, def("BC250_FAN_TEMP_MIN_C"), "the lowest curve temperature");
        Equal(FanCurves.MaxC, def("BC250_FAN_TEMP_MAX_C"), "the highest curve temperature");
        Equal(FanCurves.EmergencyC * 1000, def("BC250_FAN_EMERGENCY_ON_MC"), "the emergency temperature the card names");
        var c = File.ReadAllText(cpath);
        var table = Regex.Match(c, @"g_fan_profiles\[[^\]]*\]\s*=\s*\{(?<body>.*?)\n\};", RegexOptions.Singleline);
        Check(table.Success, "g_fan_profiles found in bc250_fan.c");
        if (table.Success)
        {
            var rows = table.Groups["body"].Value.Split('\n').Where(l => l.Contains("{ ")).ToList();
            Equal(4, rows.Count, "four profile rows (custom, standard, quiet, performance)");
            for (uint p = FanState.ProfileStandard; p <= FanState.ProfilePerformance && p < rows.Count; p++)
            {
                var pairs = Regex.Matches(rows[(int)p], @"\{\s*(\d+)u,\s*(\d+)u\s*\}").Cast<Match>()
                    .Select(m => m.Groups[1].Value + ":" + m.Groups[2].Value).ToList();
                uint[] pc, pp;
                Check(FanCurves.Preset(p, out pc, out pp), "preset " + p + " exists");
                Equal(string.Join(",", pairs), FanCurves.CurveText(pc, pp), "preset " + FanCurves.NameOf(p) + " matches the driver");
                int point;
                Equal(FanCurveError.Ok, FanCurves.Check(pc, pp, out point), "preset " + FanCurves.NameOf(p) + " is legal");
            }
        }
        // The standard curve is the default and must not run the fan slower than the board's Standard Mode near the hot
        // line: 95 % or more at 80 C, full speed before 87 C.
        uint[] sc, sp;
        FanCurves.Preset(FanState.ProfileStandard, out sc, out sp);
        Check(sp[Array.IndexOf(sc, 80u)] >= 95 && sp.Last() == 100 && sc.Last() < FanCurves.EmergencyC, "the standard curve is at least as aggressive as the board near 87 C");

        int at;
        Equal(FanCurveError.Points, FanCurves.Check(new uint[] { 40 }, new uint[] { 50 }, out at), "one point is too few");
        Equal(FanCurveError.Points, FanCurves.Check(Enumerable.Range(0, 9).Select(i => (uint)(20 + i * 5)).ToArray(), Enumerable.Repeat(50u, 9).ToArray(), out at), "nine points are too many");
        Equal(FanCurveError.Points, FanCurves.Check(new uint[] { 40, 50 }, new uint[] { 50 }, out at), "unequal lengths");
        Equal(FanCurveError.Temperature, FanCurves.Check(new uint[] { 15, 50 }, new uint[] { 50, 60 }, out at), "under 20 C refused");
        Equal(0, at, "the temperature refusal names its point");
        Equal(FanCurveError.Temperature, FanCurves.Check(new uint[] { 40, 96 }, new uint[] { 50, 60 }, out at), "over 95 C refused");
        Equal(FanCurveError.Temperature, FanCurves.Check(new uint[] { 50, 50 }, new uint[] { 50, 60 }, out at), "a temperature that does not rise is refused");
        Equal(1, at, "the order refusal names the second point");
        Equal(FanCurveError.Duty, FanCurves.Check(new uint[] { 40, 50 }, new uint[] { 19, 60 }, out at), "under the 20 % floor refused");
        Equal(FanCurveError.Duty, FanCurves.Check(new uint[] { 40, 50 }, new uint[] { 60, 101 }, out at), "over 100 % refused");
        Equal(FanCurveError.Duty, FanCurves.Check(new uint[] { 40, 50 }, new uint[] { 60, 55 }, out at), "a falling duty is refused");
        Equal(FanCurveError.Ok, FanCurves.Check(new uint[] { 40, 50 }, new uint[] { 60, 60 }, out at), "a flat duty is legal");
        Equal(-1, at, "a legal curve names no point");
        foreach (var e in new[] { FanCurveError.Points, FanCurveError.Temperature, FanCurveError.Duty })
        {
            var text = FanCurves.ErrorText(e, 2);
            Check(text.Length > 0 && !text.StartsWith("[", StringComparison.Ordinal), "the " + e + " refusal has a sentence");
            Check(!PlainWords.Findings(new[] { text }).Any(), "the " + e + " refusal names no internals");
        }

        uint[] tc, tp;
        Check(FanCurves.ParseCurve("40:30,60:45,85:100", out tc, out tp) && tc.SequenceEqual(new uint[] { 40, 60, 85 }) && tp.SequenceEqual(new uint[] { 30, 45, 100 }), "a curve survives the command line");
        Check(!FanCurves.ParseCurve("40:30", out tc, out tp), "one pair is no curve");
        Check(!FanCurves.ParseCurve("40-30,60:45", out tc, out tp), "another separator is no curve");
        Check(!FanCurves.ParseCurve("40:30,60:-1", out tc, out tp), "a negative value is no curve");
        Check(!FanCurves.ParseCurve(string.Join(",", Enumerable.Range(0, 9).Select(i => (20 + i * 5) + ":50")), out tc, out tp), "nine pairs are no curve");
    }

    static void FanPlans()
    {
        foreach (var a in FanPlan.Actions)
        {
            Check(Recovery.Actions.Contains(a), "the window offers " + a);
            Check(Strings.Has("plan.title." + a), "a dialog title for " + a);
        }
        Check(FanPlan.Owns("fan-auto") && !FanPlan.Owns("tune-trial"), "FanPlan owns its own actions only");
        foreach (var name in FanCurves.Choices.Concat(new[] { "custom" }))
            Check(Strings.Has("perf.fan.choice." + name), "a label for the choice " + name);

        var s = FanSnapshot(FanFixture());
        var p = TunePlan("fan-curve", s, new Recovery.PlanArgs { FanProfile = "quiet" });
        Check(!p.Refused, "quiet is planned: " + p.Refusal);
        Check(p.Writes.Count == 0 && p.GameWrites.Count == 0, "the fan choice writes no setting itself");
        Check(p.Tune != null && p.Tune.Kind == "fan-curve" && p.Tune.FanProfile == FanState.ProfileQuiet && p.Tune.FanC == null, "the plan carries the profile");
        Equal("at once", p.Effect, "a fan choice applies at once");
        Check(p.Preview.Count == 2 && p.Preview.All(x => !x.StartsWith("[", StringComparison.Ordinal)), "the dialog lists the choice and its speeds");
        Check(!PlainWords.Findings(p.Preview.Concat(p.PlainNotes).Concat(new[] { PlainPlan.Describe(p).Title }).ToList()).Any(), "G-NOINT: the fan dialog");
        Check(p.Text().Contains("tuning request fan-curve, fan profile quiet"), "the log names the request");

        var custom = new Recovery.PlanArgs { FanProfile = "custom", FanCurve = "40:30,70:60,85:100" };
        p = TunePlan("fan-curve", s, custom);
        Check(!p.Refused && p.Tune.FanProfile == FanState.ProfileCustom && p.Tune.FanC.SequenceEqual(new uint[] { 40, 70, 85 }) &&
            p.Tune.FanPct.SequenceEqual(new uint[] { 30, 60, 100 }), "a custom curve is planned with its points");
        Check(FanPlan.Arguments(custom).SequenceEqual(new[] { "--fan-profile", "custom", "--fan-curve", "40:30,70:60,85:100" }), "the helper gets the same curve");
        var bad = TunePlan("fan-curve", s, new Recovery.PlanArgs { FanProfile = "custom", FanCurve = "40:30,30:60" });
        Check(bad.Refused && bad.PlainRefusal == FanCurves.ErrorText(FanCurveError.Temperature, 1), "a custom curve that falls back in temperature is refused with its rule");
        Refused(TunePlan("fan-curve", s, new Recovery.PlanArgs { FanProfile = "custom", FanCurve = "40:30" }), "not 2 to 8", "a one-point curve");
        Refused(TunePlan("fan-curve", s, new Recovery.PlanArgs { FanProfile = "turbo" }), "is not a fan curve", "an unknown profile");
        Refused(TunePlan("fan-curve", s, new Recovery.PlanArgs { FanProfile = "quiet", FanCurve = "40:30,80:100" }), "carries no curve", "a preset with a curve");

        p = TunePlan("fan-auto", s);
        Check(!p.Refused && p.Tune.Kind == "fan-board" && p.Preview.Count == 1, "the board's own setting is planned");

        var storedStandard = FanFixture(FanState.FlagEnabled | FanState.FlagControlling | FanState.FlagStored);
        storedStandard.StoredMode = FanState.ModeCurve; storedStandard.StoredProfile = FanState.ProfileStandard;
        Refused(TunePlan("fan-curve", FanSnapshot(storedStandard), new Recovery.PlanArgs { FanProfile = "standard" }), "in use already", "the stored standard curve again");
        var leased = FanFixture(storedStandard.Flags | FanState.FlagLeased);
        leased.StoredMode = FanState.ModeCurve; leased.StoredProfile = FanState.ProfileStandard;
        Check(!TunePlan("fan-curve", FanSnapshot(leased), new Recovery.PlanArgs { FanProfile = "standard" }).Refused, "a curve under a lease can be made durable");
        var board = FanFixture(FanState.FlagEnabled | FanState.FlagStored);
        board.Mode = FanState.ModeBoard; board.State = FanState.StateBoard; board.StoredMode = FanState.ModeBoard;
        Refused(TunePlan("fan-auto", FanSnapshot(board)), "already", "the board again");
        Check(!TunePlan("fan-curve", FanSnapshot(board), new Recovery.PlanArgs { FanProfile = "standard" }).Refused, "from the board back to the driver curve");

        var off = FanFixture(FanState.FlagGated); off.Gate = FanState.GateSetting;
        Refused(TunePlan("fan-auto", FanSnapshot(off)), "does not run the fan control", "a start without the fan control");
        Refused(TunePlan("fan-curve", FanSnapshot(null), new Recovery.PlanArgs { FanProfile = "quiet" }), "did not answer the fan read", "no fan reading");
        var stopped = FanSnapshot(FanFixture()); stopped.Dpm = null; stopped.Health = null; stopped.Interop = null; stopped.DriverError = "not loaded";
        Check(TunePlan("fan-auto", stopped).Refused, "no running driver: refused");

        // reset-defaults puts a stored choice other than the standard curve back, with the other standard steps.
        var quiet = FanFixture(FanState.FlagEnabled | FanState.FlagControlling | FanState.FlagStored);
        quiet.StoredMode = FanState.ModeCurve; quiet.StoredProfile = FanState.ProfileQuiet;
        var rs = TunePlan("reset-defaults", FanSnapshot(quiet));
        Check(!rs.Refused && rs.TuneSteps.Any(t => t.Kind == "fan-curve" && t.FanProfile == FanState.ProfileStandard), "reset-defaults takes the fan back to the standard curve: " + rs.Refusal);
        rs = TunePlan("reset-defaults", FanSnapshot(storedStandard));
        Check(!rs.Refused && !rs.TuneSteps.Any(t => t.Kind.StartsWith("fan-", StringComparison.Ordinal)), "reset-defaults leaves the standard curve alone");
    }

    // The one registry switch of the card: full fan speed under a sustained heavy load (docs/design/fan.md rule
    // 10, the driver's FanLoadBoost). It is on unless somebody switched it off, so "on" removes the value, and
    // the driver reads it when it starts.
    static void FanBoostPlans(string header)
    {
        Check(Regex.IsMatch(header, @"#define BC250_FAN_FLAG_BOOST " + FanState.FlagBoost + @"u\b"), "FlagBoost = BC250_FAN_FLAG_BOOST");
        Check(Regex.IsMatch(header, @"#define BC250_FAN_FLAG_BOOST_OFF " + FanState.FlagBoostOff + @"u\b"), "FlagBoostOff = BC250_FAN_FLAG_BOOST_OFF");
        Check(FanCurves.BoostOn(null) && FanCurves.BoostOn(1) && !FanCurves.BoostOn(0) && !FanCurves.BoostOn(7),
            "nothing stored and 1 are on, 0 and any other value off, as the driver reads the value");

        var clean = FanSnapshot(FanFixture());
        clean.Parameters.Remove("FanLoadBoost");
        Refused(TunePlan("fan-boost-on", clean), "already", "turning it on with nothing stored");
        var off = TunePlan("fan-boost-off", clean);
        Check(!off.Refused && Writes(off, "FanLoadBoost=0"), "turning it off writes one value: " + off.Refusal);
        Equal("at the next restart of Windows", off.Effect, "the driver reads the switch at its next start");
        Check(off.Undoable && off.OfferRestart, "the switch is undoable and offers the restart");
        Equal("plan.line.fan-boost-off", PlainPlan.LineId(off.Writes[0]), "G-PLAN: the off write has its own sentence");
        Check(off.Preview.Count == 1 && off.Tune == null && off.GameWrites.Count == 0, "it is one write and no request");
        Check(!PlainWords.Findings(off.Preview.Concat(new[] { PlainPlan.Describe(off).Title }).ToList()).Any(), "G-NOINT: the switch's dialog");

        var stored = FanSnapshot(FanFixture());
        stored.Parameters["FanLoadBoost"] = 0;
        Refused(TunePlan("fan-boost-off", stored), "already", "turning it off twice");
        var on = TunePlan("fan-boost-on", stored);
        Check(!on.Refused && Writes(on, "FanLoadBoost-"), "turning it on again removes the value");
        Equal("plan.line.fan-boost-on", PlainPlan.LineId(on.Writes[0]), "G-PLAN: the on write is the delete form");
        Check(Recovery.Allowed(Recovery.ParametersPath, "FanLoadBoost"), "the value is in the allowlist of the actions");

        // The switch is a setting and not a fan request, so it needs neither a reading nor a start that runs the
        // fan: the driver reads the value when it starts.
        var noRead = FanSnapshot(null);
        noRead.Parameters["FanLoadBoost"] = 0;
        Check(!TunePlan("fan-boost-on", noRead).Refused, "the switch needs no fan reading");
        var gated = FanSnapshot(FanFixture(FanState.FlagGated));
        gated.Parameters["FanLoadBoost"] = 0;
        gated.Fan.Gate = FanState.GateSetting;
        Check(!TunePlan("fan-boost-on", gated).Refused, "nor a start whose fan control is enabled");

        var words = new List<string> { Strings.T("perf.fan.boost"), Strings.T("perf.fan.boost.help"),
            Strings.T("perf.fan.boost.now"), Strings.T("perf.fan.boost.after-restart") };
        foreach (var t in words) Check(t.Length > 0 && !t.StartsWith("[", StringComparison.Ordinal), "fan boost sentence: " + t);
        Check(!PlainWords.Findings(words).Any(), "G-NOINT: the switch's own words");
    }

    // Every state and gate has a plain sentence.
    static void FanWords()
    {
        var texts = new List<string>();
        foreach (var state in new[] { FanState.StateOff, FanState.StateBoard, FanState.StateCurve, FanState.StateFixed, FanState.StateEmergency, FanState.StateDoubt, FanState.StateFault })
        {
            var f = FanFixture(); f.State = state;
            texts.Add(FanCurves.StateText(f));
        }
        var held = FanFixture(FanState.FlagEnabled | FanState.FlagHeldBack); held.State = FanState.StateBoard;
        texts.Add(FanCurves.StateText(held));
        var paused = FanFixture(FanState.FlagEnabled | FanState.FlagPaused);
        texts.Add(FanCurves.StateText(paused));
        Check(texts.Distinct().Count() == texts.Count - 1, "each state has its own sentence (off and board share one)");
        foreach (var gate in new[] { FanState.GateSetting, FanState.GateReader, FanState.GateChip })
        {
            var f = FanFixture(FanState.FlagGated); f.Gate = gate;
            texts.Add(FanCurves.GateText(f));
        }
        Equal(null, FanCurves.GateText(FanFixture()), "an enabled start has no gate sentence");
        texts.Add(FanCurves.Describe(new uint[] { 40, 85 }, new uint[] { 50, 100 }));
        foreach (var t in texts) Check(t.Length > 0 && !t.StartsWith("[", StringComparison.Ordinal), "fan sentence: " + t);
        Check(!PlainWords.Findings(texts).Any(), "G-NOINT: the fan card's sentences");
        Check(FanCurves.ReportLine(FanFixture()).StartsWith("fan control: state 2, mode 1, profile 1", StringComparison.Ordinal), "the support report line");
    }
}
