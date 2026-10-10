// Host tests of the tuning page's pure parts: the two escape layouts against the header text, the parsers, the curve
// and processor rules of Tuner.cs (which mirror the driver's), and every plan and refusal of TunerPlan.cs.
//
// The rules live twice on purpose: once in the driver, which decides, and once here, so that the window can say why
// before it asks. These tests are what keeps the two copies the same sentence; the numbers come from the header and
// from docs/design/tuner.md, never from memory.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using AmdgpuWddmControl;

static partial class UnitTests
{
    static CurveState CurveFixture(uint flags = CurveState.FlagValid | CurveState.FlagGoverning | CurveState.FlagDefault)
    {
        return new CurveState
        {
            Version = 0x000700D2, Flags = flags, FirstMHz = Tuner.FirstMHz, StepMHz = Tuner.StepMHz, Points = (uint)Tuner.Points,
            Candidate = new uint[Tuner.Points], Active = Tuner.Table(), Stored = new uint[Tuner.Points],
            Default = Tuner.Table(), Floor = Tuner.Floors(), Level = 5, LevelMHz = 1500, LevelMv = 919,
            CeilingMHz = 1500, Mode = 1, Generation = 5,
        };
    }

    static CpuState CpuFixture(uint flags = CpuState.FlagValid | CpuState.FlagTuneOn | CpuState.FlagQueue3Proven)
    {
        return new CpuState
        {
            Version = 0x000700D2, Flags = flags, CoreMHz = new uint[KmdReply.CpuCoreSlots], PstateMHz = new uint[KmdReply.CpuCoreSlots],
            BaselineMaxMHz = CpuTuning.MaxMHz, BaselineTempC = 95, VoltageMv = 1050, CapC = 95,
            Cores = 6, Threads = 12, CoreMask = CpuTuning.MaskStock, Generation = 5,
        };
    }

    static RecoverySnapshot Tuned(CurveState c = null, CpuState u = null)
    {
        var s = Open();
        s.Parameters["DpmMode"] = 1;
        s.Parameters["CpuTune"] = 1;
        s.Dpm = new DpmState { Mode = 1, MaxMHz = 1500 };
        s.Curve = c ?? CurveFixture();
        s.Cpu = u ?? CpuFixture();
        return s;
    }

    static ActionPlan TunePlan(string action, RecoverySnapshot s, Recovery.PlanArgs more = null)
    {
        return Recovery.Plan(action, s, null, null, new List<BackupRecord>(), false, more);
    }

    // The refusal exists in both languages: English for the log, the window's language for the person.
    static void Refused(ActionPlan p, string englishPart, string what)
    {
        Check(p.Refused, what + ": refused");
        Check(p.Refusal != null && p.Refusal.Contains(englishPart), what + ": English refusal says \"" + englishPart + "\", got \"" + p.Refusal + "\"");
        var plain = PlainPlan.Refusal(p);
        Check(plain.Length > 0 && !plain.StartsWith("[", StringComparison.Ordinal), what + ": plain refusal: " + plain);
        Check(!PlainWords.Findings(new[] { plain }).Any(), what + ": plain refusal names no internals");
    }

    static void TunerTests(string root, string header)
    {
        Strings.Language = "en";
        TunerLayout(header);
        TunerModel();
        TunerCpuModel();
        TunerPlans();
        TunerInputGate(root);
        TunerDesignDoc(root);
    }

    // ---- the two escape structures ---------------------------------------------------------------------------------

    static void TunerLayout(string header)
    {
        int size;
        var cv = Layout(header, "BC250_ESCAPE_DPM_CURVE", out size);
        Equal(KmdReply.CurveBytes, size, "curve escape size from the header");
        Check(Regex.IsMatch(header, @"#define BC250_DPM_CURVE_POINTS " + KmdReply.CurvePoints + @"u\b"), "curve points from the header");
        Check(Regex.IsMatch(header, @"#define BC250_DPM_CURVE_ABI 1u\b"), "curve ABI 1 in the header");
        // Every field this application reads, at the word the parser takes it from.
        var words = new Dictionary<string, int>
        {
            { "Version", 3 }, { "AbiVersion", 5 }, { "Flags", 7 }, { "TrialMs", 8 }, { "TrialRemainingMs", 9 },
            { "Serial", 10 }, { "Applied", 11 }, { "Error", 12 }, { "ErrorLevel", 13 }, { "FirstMHz", 14 },
            { "StepMHz", 15 }, { "Points", 16 }, { "CandidateMv", 17 }, { "ActiveMv", 28 }, { "StoredMv", 39 },
            { "DefaultMv", 50 }, { "FloorMv", 61 }, { "Level", 72 }, { "LevelMHz", 73 }, { "LevelMv", 74 },
            { "ObservedMHz", 75 }, { "ObservedVid", 76 }, { "TemperatureMc", 77 }, { "CeilingMHz", 78 },
            { "Mode", 79 }, { "Sets", 80 }, { "Keeps", 81 }, { "Cancels", 82 }, { "Reverts", 83 },
        };
        foreach (var kv in words) Equal(kv.Value * 4, cv[kv.Key], "curve " + kv.Key + " offset");
        Equal(336, cv["Generation"], "curve Generation offset");
        var b = new byte[KmdReply.CurveBytes];
        Put(b, cv["Magic"], KmdReply.Magic); Put(b, cv["Command"], KmdReply.CmdCurve); Put(b, cv["AbiVersion"], 1u);
        Put(b, cv["Version"], 0x000700D2u); Put(b, cv["Flags"], CurveState.FlagValid | CurveState.FlagOnTrial | CurveState.FlagGoverning);
        Put(b, cv["FirstMHz"], Tuner.FirstMHz); Put(b, cv["StepMHz"], Tuner.StepMHz); Put(b, cv["Points"], (uint)Tuner.Points);
        Put(b, cv["TrialMs"], 120000u); Put(b, cv["TrialRemainingMs"], 61000u); Put(b, cv["Serial"], 3u);
        Put(b, cv["Error"], 2u); Put(b, cv["ErrorLevel"], 4u); Put(b, cv["Level"], 5u); Put(b, cv["LevelMHz"], 1500u);
        Put(b, cv["LevelMv"], 894u); Put(b, cv["TemperatureMc"], unchecked((uint)62500)); Put(b, cv["Mode"], 1u);
        Put(b, cv["Reverts"], 7u); Put(b, cv["Generation"], 9UL);
        for (int i = 0; i < Tuner.Points; i++)
        {
            Put(b, cv["CandidateMv"] + i * 4, Tuner.FloorAt(i));
            Put(b, cv["ActiveMv"] + i * 4, Tuner.TableMv[i]);
            Put(b, cv["DefaultMv"] + i * 4, Tuner.TableMv[i]);
            Put(b, cv["FloorMv"] + i * 4, Tuner.FloorAt(i));
        }
        var c = KmdReply.ParseCurve(b);
        Check(c.Has(CurveState.FlagOnTrial), "curve on trial");
        Equal(61000u, c.TrialRemainingMs, "curve trial left");
        Equal("61 s left", Tuner.Countdown(c.TrialRemainingMs), "curve countdown rounds up to whole seconds");
        Equal(Tuner.FloorAt(10), c.Candidate[10], "curve candidate vector");
        Equal(Tuner.TableMv[10], c.Default[10], "curve default vector");
        Equal(894u, c.LevelMv, "curve level mV"); Equal(62500, c.TemperatureMc, "curve temperature");
        Equal(2u, c.Error, "curve last reason"); Equal(7u, c.Reverts, "curve reverts"); Equal(9UL, c.Generation, "curve generation");
        Put(b, cv["AbiVersion"], 2u);
        Throws<FormatException>(() => KmdReply.ParseCurve(b), "curve ABI 2 refused");
        Put(b, cv["AbiVersion"], 1u); Put(b, cv["Points"], 10u);
        Throws<FormatException>(() => KmdReply.ParseCurve(b), "curve with 10 points refused");
        Put(b, cv["Points"], (uint)Tuner.Points); Put(b, cv["Command"], 23u);
        Throws<FormatException>(() => KmdReply.ParseCurve(b), "curve wrong command refused");
        Throws<FormatException>(() => KmdReply.ParseCurve(new byte[KmdReply.CurveBytes - 8]), "curve short reply refused");

        var cp = Layout(header, "BC250_ESCAPE_CPU", out size);
        Equal(KmdReply.CpuBytes, size, "CPU escape size from the header");
        Check(Regex.IsMatch(header, @"#define BC250_CPU_CORE_SLOTS " + KmdReply.CpuCoreSlots + @"u\b"), "CPU core slots from the header");
        var cpuWords = new Dictionary<string, int>
        {
            { "Version", 3 }, { "AbiVersion", 5 }, { "Op", 6 }, { "Flags", 7 }, { "TrialMs", 8 }, { "TrialRemainingMs", 9 },
            { "Serial", 10 }, { "Error", 11 }, { "Given", 12 }, { "MaxMHz", 13 }, { "UvSteps", 14 }, { "TempC", 15 },
            { "AppliedMaxMHz", 16 }, { "AppliedUvSteps", 17 }, { "AppliedTempC", 18 },
            { "StoredMaxMHz", 19 }, { "StoredUvSteps", 20 }, { "StoredTempC", 21 },
            { "BaselineMaxMHz", 22 }, { "BaselineUvSteps", 23 }, { "BaselineTempC", 24 },
            { "VoltageMv", 25 }, { "GpuVoltageMv", 26 }, { "CapC", 27 }, { "Features", 28 },
            { "CoreMHz", 29 }, { "PstateMHz", 37 }, { "Cores", 45 }, { "Threads", 46 }, { "CoreMask", 47 },
            { "CoreMaskStored", 48 }, { "LastQueue", 49 }, { "LastMessage", 50 }, { "LastStatus", 51 },
            { "LastParameter", 52 }, { "TemperatureMc", 53 }, { "SearchStep", 54 }, { "SearchBest", 55 },
            { "SearchFail", 56 }, { "SearchTested", 57 }, { "Reads", 58 }, { "Writes", 59 }, { "Refusals", 60 }, { "Reverts", 61 },
            { "RevertRetries", 62 }, { "RevertFailures", 63 }, { "WheaEvents", 64 }, { "ChecksumErrors", 65 },
            { "Loaded", 66 },
        };
        foreach (var kv in cpuWords) Equal(kv.Value * 4, cp[kv.Key], "CPU " + kv.Key + " offset");
        Equal(272, cp["Generation"], "CPU Generation offset");
        Equal(280, cp["ExpectedGeneration"], "CPU ExpectedGeneration offset");
        // The three values a caller must know come from the escape header itself, with the shim as their authority.
        Check(Regex.IsMatch(header, @"#define BC250_CPU_REQUEST_MASK_STOCK 0x" + CpuTuning.MaskStock.ToString("X") + @"u\b"), "stock core mask from the header");
        Check(Regex.IsMatch(header, @"#define BC250_CPU_REQUEST_MASK_FULL 0x" + CpuTuning.MaskFull.ToString("X") + @"u\b"), "full core mask from the header");
        b = new byte[KmdReply.CpuBytes];
        Put(b, cp["Magic"], KmdReply.Magic); Put(b, cp["Command"], KmdReply.CmdCpu); Put(b, cp["AbiVersion"], 1u);
        Put(b, cp["Flags"], CpuState.FlagValid | CpuState.FlagTuneOn | CpuState.FlagQueue3Proven | CpuState.FlagOnTrial);
        Put(b, cp["TrialMs"], 120000u); Put(b, cp["TrialRemainingMs"], 1u); Put(b, cp["VoltageMv"], 1337u);
        Put(b, cp["AppliedUvSteps"], 8u); Put(b, cp["BaselineMaxMHz"], 3600u); Put(b, cp["CapC"], 95u);
        Put(b, cp["Cores"], 6u); Put(b, cp["Threads"], 12u); Put(b, cp["CoreMask"], CpuTuning.MaskStock);
        Put(b, cp["CoreMHz"] + 4, 3550u); Put(b, cp["PstateMHz"], 3600u); Put(b, cp["TemperatureMc"], unchecked((uint)71000));
        Put(b, cp["Refusals"], 2u); Put(b, cp["Generation"], 11UL);
        Put(b, cp["RevertRetries"], 4u); Put(b, cp["RevertFailures"], 1u);
        var u = KmdReply.ParseCpu(b);
        Check(u.Has(CpuState.FlagQueue3Proven) && u.Has(CpuState.FlagOnTrial), "CPU flags");
        // An owed way back and a temperature that was not read on the last message (0.7.211).
        Equal(4u, u.RevertRetries, "CPU revert retries"); Equal(1u, u.RevertFailures, "CPU refused reverts");
        Check(!u.Has(CpuState.FlagRevertOwed) && !u.Has(CpuState.FlagTempValid), "neither new flag is set here");
        Put(b, cp["Flags"], CpuState.FlagValid | CpuState.FlagRevertOwed | CpuState.FlagTempValid);
        var owed = KmdReply.ParseCpu(b);
        Check(owed.Has(CpuState.FlagRevertOwed) && owed.Has(CpuState.FlagTempValid), "the owed revert and the read temperature");
        Put(b, cp["Flags"], CpuState.FlagValid | CpuState.FlagTuneOn | CpuState.FlagQueue3Proven | CpuState.FlagOnTrial);
        Equal(1337u, u.VoltageMv, "CPU voltage"); Equal(8u, u.AppliedUvSteps, "CPU applied steps");
        Equal(3550u, u.CoreMHz[1], "CPU per-core clock"); Equal(3600u, u.PstateMHz[0], "CPU p-state clock");
        Equal(71000, u.TemperatureMc, "CPU temperature"); Equal(2u, u.Refusals, "CPU refusals"); Equal(11UL, u.Generation, "CPU generation");
        Equal("1 s left", Tuner.Countdown(u.TrialRemainingMs), "a window of 1 ms still reads as a second");
        // A voltage over the line this application admits is said plainly, never silently shown as normal.
        Check(CpuTuning.VoltageText(u.VoltageMv).Contains("higher"), "a voltage over the refusal line is named");
        Equal("1050 mV", CpuTuning.VoltageText(1050), "a voltage inside the band");
        Equal(Strings.T("perf.no-reading"), CpuTuning.VoltageText(0), "no voltage reading");
        Put(b, cp["AbiVersion"], 3u);
        Throws<FormatException>(() => KmdReply.ParseCpu(b), "CPU ABI 3 refused");
    }

    // ---- the curve rules -------------------------------------------------------------------------------------------

    static void TunerModel()
    {
        Equal(11, Tuner.Points, "11 curve points");
        Equal(2000u, Tuner.MHzAt(Tuner.Points - 1), "the last point is 2000 MHz");
        Equal(Tuner.FloorMv, Tuner.TableMv[0], "the table starts at the lab floor");
        Equal(Tuner.CeilingMv, Tuner.TableMv[Tuner.Points - 1], "the table ends at the ceiling");
        for (int i = 1; i < Tuner.Points; i++)
        {
            Check(Tuner.TableMv[i] > Tuner.TableMv[i - 1], "the table rises at " + Tuner.MHzAt(i) + " MHz");
            Check(Tuner.Vid(Tuner.TableMv[i]) <= Tuner.Vid(Tuner.TableMv[i - 1]), "the encoded voltage never inverts at " + Tuner.MHzAt(i) + " MHz");
        }
        Equal(975u, Tuner.FloorAt(Tuner.Points - 1), "the deepest point is 25 mV under the table's 1000 mV");
        Equal(Tuner.FloorMv, Tuner.FloorAt(0), "the first level keeps the lab floor");
        Equal(Tuner.FloorMv, Tuner.FloorAt(1), "a line 25 mV over the floor cannot go deeper than the floor");

        int level;
        Equal(CurveError.Ok, Tuner.Check(Tuner.Table(), Tuner.Floors(), out level), "the table's own line is a legal curve");
        Equal(-1, level, "a legal curve names no level");
        Equal(CurveError.Ok, Tuner.Check(Tuner.Preset("deep", Tuner.Table(), Tuner.Floors()), Tuner.Floors(), out level), "the deepest preset is legal");
        Equal(CurveError.None, Tuner.Check(null, null, out level), "no curve at all");
        Equal(CurveError.None, Tuner.Check(new uint[3], null, out level), "a curve of the wrong length");

        // The refusal order is the driver's: range, then the first level, then the floor, then the order.
        var mv = Tuner.Table(); mv[4] = 1200;
        Equal(CurveError.Range, Tuner.Check(mv, Tuner.Floors(), out level), "over the ceiling is out of range");
        Equal(4, level, "the range refusal names its level");
        mv = Tuner.Table(); mv[0] = 840;
        Equal(CurveError.Floor, Tuner.Check(mv, Tuner.Floors(), out level), "the first level must keep the floor");
        Equal(0, level, "the floor refusal names level 0");
        mv = Tuner.Table(); mv[6] = Tuner.FloorAt(6) - 1;
        Equal(CurveError.Depth, Tuner.Check(mv, Tuner.Floors(), out level), "one millivolt under the floor is refused");
        Equal(6, level, "the depth refusal names its level");
        mv = Tuner.Table(); mv[7] = mv[6] - 1;
        Equal(CurveError.Order, Tuner.Check(mv, Tuner.Floors(), out level), "a value under its neighbour is refused");
        Equal(7, level, "the order refusal names its level");
        var flat = Enumerable.Repeat(Tuner.FloorMv, Tuner.Points).ToArray();
        Equal(CurveError.Depth, Tuner.Check(flat, Tuner.Floors(), out level), "a flat 820 mV curve is too deep, not out of order");
        Equal(2, level, "the flat curve fails at the first level with a deeper floor");
        foreach (var e in new[] { CurveError.Range, CurveError.Depth, CurveError.Order, CurveError.Floor, CurveError.None, CurveError.Untried })
        {
            var text = Tuner.ErrorText(e, 1500);
            Check(text.Length > 0 && !text.StartsWith("[", StringComparison.Ordinal), "the " + e + " refusal has a sentence");
            Check(!PlainWords.Findings(new[] { text }).Any(), "the " + e + " refusal names no internals");
        }
        Equal("", Tuner.ErrorText(CurveError.Ok, 1500), "a legal curve has no sentence");

        // The presets: each one legal by construction, and recognised again by PresetOf.
        foreach (var name in Tuner.PresetNames)
        {
            var p = Tuner.Preset(name, Tuner.Table(), Tuner.Floors());
            Equal(CurveError.Ok, Tuner.Check(p, Tuner.Floors(), out level), "preset " + name + " is legal");
            Equal(name, Tuner.PresetOf(p, Tuner.Table(), Tuner.Floors()), "preset " + name + " is recognised");
            Equal(Tuner.FloorMv, p[0], "preset " + name + " keeps the first level");
        }
        Check(Tuner.IsDefault(Tuner.Preset("standard", Tuner.Table(), Tuner.Floors()), Tuner.Table()), "the standard preset is the table's line");
        Check(!Tuner.IsDefault(Tuner.Preset("deep", Tuner.Table(), Tuner.Floors()), Tuner.Table()), "the deepest preset is not");
        var deep = Tuner.Preset("deep", Tuner.Table(), Tuner.Floors());
        Equal(Tuner.FloorAt(Tuner.Points - 1), deep[Tuner.Points - 1], "the deepest preset reaches the lowest the driver allows");
        var edited = Tuner.Preset("mild", Tuner.Table(), Tuner.Floors()); edited[9] -= 3;
        Equal(null, Tuner.PresetOf(edited, Tuner.Table(), Tuner.Floors()), "an edited curve is no preset");

        // One knot moved: clamped to its own band, and the first level never moves.
        Equal(Tuner.FloorMv, Tuner.Nudge(999, -1, 0, Tuner.Table(), Tuner.Floors()), "level 0 stays at the floor");
        Equal(Tuner.FloorAt(5), Tuner.Nudge(Tuner.TableMv[5], -200, 5, Tuner.Table(), Tuner.Floors()), "a deep step stops at the floor of its clock");
        Equal(Tuner.TableMv[5], Tuner.Nudge(Tuner.TableMv[5], 200, 5, Tuner.Table(), Tuner.Floors()), "a step up stops at the table's own line");
        Equal(Tuner.TableMv[5] - 5, Tuner.Nudge(Tuner.TableMv[5], -5, 5, Tuner.Table(), Tuner.Floors()), "5 mV down");
        Equal("standard", Tuner.DeltaText(Tuner.TableMv[3], Tuner.TableMv[3]), "no difference reads as standard");
        Check(Tuner.DeltaText(Tuner.TableMv[3] - 10, Tuner.TableMv[3]).Contains("10"), "a difference says how much");
    }

    static void TunerCpuModel()
    {
        Check(CpuTuning.ValidClock(CpuTuning.MaxMHz) && CpuTuning.ValidClock(CpuTuning.MinMHz), "the admitted processor clocks");
        Check(!CpuTuning.ValidClock(CpuTuning.MaxMHz + 100), "over the highest admitted clock is refused: this application only lowers");
        Check(!CpuTuning.ValidClock(CpuTuning.MinMHz - 100) && !CpuTuning.ValidClock(3650), "under the band and off the grid are refused");
        Check(CpuTuning.ValidSteps(0) && CpuTuning.ValidSteps(CpuTuning.MaxSteps) && !CpuTuning.ValidSteps(CpuTuning.MaxSteps + 1), "the undervolt steps");
        Check(CpuTuning.ValidTemp(CpuTuning.MinTempC) && CpuTuning.ValidTemp(CpuTuning.MaxTempC) && !CpuTuning.ValidTemp(84) && !CpuTuning.ValidTemp(101), "the temperature cap band");
        Check(CpuTuning.ValidCores(6) && CpuTuning.ValidCores(8) && !CpuTuning.ValidCores(7), "the two core counts");
        Equal(CpuTuning.MaskFull, CpuTuning.MaskFor(8), "8 cores is the full mask");
        Equal(CpuTuning.StockCores, CpuTuning.CoresFor(CpuTuning.MaskStock), "the stock mask is 6 cores");
        var choices = CpuTuning.ClockChoices();
        Equal(CpuTuning.MaxMHz, choices[0], "the clock list starts at the highest admitted clock");
        Equal(CpuTuning.MinMHz, choices[choices.Length - 1], "the clock list ends at the lowest admitted");
        Check(choices.All(CpuTuning.ValidClock), "every clock the window offers is one the driver takes");
        Equal("standard", CpuTuning.StepsText(0), "no undervolt reads as standard");
        Check(CpuTuning.StepsText(8).Contains("8"), "an undervolt says how deep");
    }

    // ---- the plans -------------------------------------------------------------------------------------------------

    static void TunerPlans()
    {
        foreach (var a in TunerPlan.Actions) Check(Recovery.Actions.Contains(a), "the window offers " + a);
        Check(TunerPlan.Owns("tune-trial") && !TunerPlan.Owns("set-clocks"), "TunerPlan owns its own actions only");
        Equal(null, TunerPlan.ParseCurve("820,840"), "a short curve text is no curve");
        Equal(null, TunerPlan.ParseCurve("820,840,860,880,899,919,935,952,968,984,1100"), "a value over the ceiling is no curve");
        Equal(null, TunerPlan.ParseCurve("820,840,860,880,899,919,935,952,968,984,-1"), "a negative value is no curve");
        Equal(null, TunerPlan.ParseCurve("820;840"), "another separator is no curve");
        var text = TunerPlan.CurveText(Tuner.Table());
        Check(TunerPlan.ParseCurve(text).SequenceEqual(Tuner.Table()), "a curve survives the command line");

        var mild = Tuner.Preset("mild", Tuner.Table(), Tuner.Floors());
        var args = new Recovery.PlanArgs { Curve = TunerPlan.CurveText(mild), Window = TunerPlan.DefaultWindowMs };
        var s = Tuned();
        var p = TunePlan("tune-trial", s, args);
        Check(!p.Refused, "a legal curve is planned: " + p.Refusal);
        Check(p.Writes.Count == 0 && p.GameWrites.Count == 0, "a trial writes no setting");
        Check(p.Tune != null && p.Tune.Kind == "curve-trial" && p.Tune.Mv.SequenceEqual(mild), "the plan carries the curve");
        Equal(TunerPlan.DefaultWindowMs, p.Tune.WindowMs, "the plan carries the window");
        Check(!p.Undoable, "a trial has no backup to undo");
        Equal("at once", p.Effect, "a trial applies at once");
        Check(p.Preview.Count >= 2 && p.Preview.All(x => !x.StartsWith("[", StringComparison.Ordinal)), "the dialog lists the change");
        Check(!PlainWords.Findings(p.Preview.Concat(new[] { PlainPlan.Describe(p).Title }).ToList()).Any(), "G-NOINT: the trial dialog");
        Check(p.Text().Contains("tuning request curve-trial"), "the log names the request");
        Check(TunerPlan.Arguments(args).SequenceEqual(new[] { "--curve", TunerPlan.CurveText(mild), "--window", TunerPlan.DefaultWindowMs.ToString() }),
            "the helper gets the same curve on its command line");

        Refused(TunePlan("tune-trial", s, new Recovery.PlanArgs { Curve = "820,840" }), "not 11 values", "a malformed curve");
        Refused(TunePlan("tune-trial", s, new Recovery.PlanArgs { Curve = TunerPlan.CurveText(Enumerable.Repeat(Tuner.FloorMv, Tuner.Points).ToArray()) }),
            "breaks rule Depth", "a curve under the floor");
        Refused(TunePlan("tune-trial", s, new Recovery.PlanArgs { Curve = TunerPlan.CurveText(Tuner.Table()) }), "is the one in use", "the curve in force");
        Refused(TunePlan("tune-trial", s, new Recovery.PlanArgs { Curve = TunerPlan.CurveText(mild), Window = 5000 }), "trial window", "too short a window");
        Refused(TunePlan("tune-trial", s, new Recovery.PlanArgs { Curve = TunerPlan.CurveText(mild), Window = 200000 }), "trial window", "too long a window");
        Refused(TunePlan("tune-keep", s), "No curve trial is running", "a keep without a trial");
        Refused(TunePlan("tune-stop", s), "No curve trial is running", "a stop without a trial");
        Refused(TunePlan("tune-reset", s), "default curve is in use already", "a reset with nothing stored");

        // Not governing: the clock is fixed, so a curve would change nothing and the window says what to do first.
        var fixedClock = Tuned(CurveFixture(CurveState.FlagValid));
        Refused(TunePlan("tune-trial", fixedClock, args), "does not govern the clock", "a curve without automatic clocks");
        var noRead = Tuned(); noRead.Curve = null;
        Refused(TunePlan("tune-trial", noRead, args), "did not answer", "a curve without a reading");
        var notValid = Tuned(CurveFixture(0));
        Refused(TunePlan("tune-trial", notValid, args), "does not carry the curve surface", "a start without the surface");
        var notRunning = Tuned(); notRunning.DriverError = "no answer"; notRunning.Interop = null; notRunning.Health = null; notRunning.Dpm = null;
        Refused(TunePlan("tune-trial", notRunning, args), "driver is not running", "a curve without a driver");

        // A candidate the governor has not put into the chip has proved nothing: a Keep of it would run it at
        // every later start. The driver refuses the same case, and the window says so first (0.7.211).
        var untried = CurveFixture(CurveState.FlagValid | CurveState.FlagGoverning | CurveState.FlagOnTrial);
        untried.Candidate = mild; untried.TrialRemainingMs = 30000;
        Refused(TunePlan("tune-keep", Tuned(untried)), "has not applied the candidate", "a keep before the governor applied it");
        Check(!TunePlan("tune-stop", Tuned(untried)).Refused, "a stop of an unapplied trial is still allowed");

        // On trial: keep, stop and reset are the three ways out, and each one names the curve it leaves behind.
        var onTrial = CurveFixture(CurveState.FlagValid | CurveState.FlagGoverning | CurveState.FlagOnTrial | CurveState.FlagApplied);
        onTrial.Candidate = mild; onTrial.Active = mild; onTrial.TrialRemainingMs = 30000;
        var t = Tuned(onTrial);
        foreach (var a in new[] { "tune-keep", "tune-stop", "tune-reset" })
        {
            var q = TunePlan(a, t);
            Check(!q.Refused, a + " during a trial is planned: " + q.Refusal);
            Check(q.Tune != null && q.Tune.Mv == null, a + " sends no curve of its own");
            Check(q.Preview.Count > 0 && !PlainWords.Findings(q.Preview).Any(), "G-NOINT: the " + a + " dialog");
        }
        Equal("curve-keep", TunePlan("tune-keep", t).Tune.Kind, "keep is the keep operation");
        Equal("curve-stop", TunePlan("tune-stop", t).Tune.Kind, "stop ends the trial");
        Equal("curve-reset", TunePlan("tune-reset", t).Tune.Kind, "reset removes the stored curve");

        // The processor. Nothing reaches its mailbox queue before a readback of this start has answered.
        var unproven = Tuned(null, CpuFixture(CpuState.FlagValid | CpuState.FlagTuneOn));
        Refused(TunePlan("cpu-trial", unproven, new Recovery.PlanArgs { CpuUv = 4 }), "has not answered a readback", "a write before a readback");
        var readback = TunePlan("cpu-readback", unproven);
        Check(!readback.Refused && readback.Tune.Kind == "cpu-readback", "a readback is allowed before anything else");
        var off = Tuned(null, CpuFixture(CpuState.FlagValid));
        Refused(TunePlan("cpu-readback", off), "Processor tuning is off", "the processor surface off");
        var busy = Tuned(null, CpuFixture(CpuState.FlagValid | CpuState.FlagTuneOn | CpuState.FlagQueue3Proven | CpuState.FlagBusy));
        Refused(TunePlan("cpu-readback", busy), "busy with another request", "the processor surface busy");

        var trial = TunePlan("cpu-trial", s, new Recovery.PlanArgs { CpuClock = 3400, CpuUv = 8, CpuTemp = 90, Window = TunerPlan.DefaultWindowMs });
        Check(!trial.Refused, "a processor trial is planned: " + trial.Refusal);
        Equal(3400u, trial.Tune.MaxMHz.Value, "the trial carries the clock");
        Equal(8u, trial.Tune.UvSteps.Value, "the trial carries the undervolt");
        Equal(90u, trial.Tune.TempC.Value, "the trial carries the cap");
        Check(trial.Preview.Count >= 4 && !PlainWords.Findings(trial.Preview).Any(), "G-NOINT: the processor trial dialog");
        Check(trial.Notes.Any(x => x.Contains("community")), "the dialog's log says where these numbers come from");
        Refused(TunePlan("cpu-trial", s), "carries no processor value", "an empty processor trial");
        Refused(TunePlan("cpu-trial", s, new Recovery.PlanArgs { CpuClock = 4000 }), "processor clock must be", "a clock over stock");
        Refused(TunePlan("cpu-trial", s, new Recovery.PlanArgs { CpuUv = 20 }), "undervolt must be", "too deep an undervolt");
        Refused(TunePlan("cpu-trial", s, new Recovery.PlanArgs { CpuTemp = 101 }), "temperature cap must be", "a cap over the band");
        Refused(TunePlan("cpu-keep", s), "No processor trial is running", "a processor keep without a trial");
        Refused(TunePlan("cpu-reset", s), "default settings already", "a processor reset with nothing applied");

        var cpuTrial = CpuFixture(CpuState.FlagValid | CpuState.FlagTuneOn | CpuState.FlagQueue3Proven | CpuState.FlagOnTrial);
        cpuTrial.AppliedUvSteps = 8; cpuTrial.TrialRemainingMs = 45000;
        var ct = Tuned(null, cpuTrial);
        foreach (var a in new[] { "cpu-keep", "cpu-stop", "cpu-reset" })
            Check(!TunePlan(a, ct).Refused, a + " during a processor trial is planned");

        // The core count: a restart applies it, and the dialog says so.
        var cores = TunePlan("core-mask", s, new Recovery.PlanArgs { Cores = 8 });
        Check(!cores.Refused, "8 cores is planned: " + cores.Refusal);
        Equal(CpuTuning.MaskFull, cores.Tune.CoreMask, "the plan carries the full mask");
        Equal("at the next restart of Windows", cores.Effect, "the core count applies at a restart");
        Check(cores.OfferRestart, "the window offers the restart");
        Check(PlainPlan.Describe(cores).Restart, "the dialog says a restart is needed");
        Refused(TunePlan("core-mask", s, new Recovery.PlanArgs { Cores = 6 }), "chosen already", "the core count in force");
        Refused(TunePlan("core-mask", s, new Recovery.PlanArgs { Cores = 7 }), "core count must be", "a core count this board does not take");
        Refused(TunePlan("core-mask", s), "core count must be", "no core count at all");

        // The one switch of this page that writes a setting.
        var noTune = Tuned(); noTune.Parameters.Remove("CpuTune"); noTune.Cpu = CpuFixture(CpuState.FlagValid);
        var enable = TunePlan("cpu-enable", noTune);
        Check(!enable.Refused && Writes(enable, "CpuTune=1"), "turning processor tuning on writes one value");
        Equal("at the next restart of Windows", enable.Effect, "the driver reads it at its next start");
        Check(enable.Undoable, "that one is undoable");
        Check(PlainPlan.LineId(enable.Writes[0]) != null, "G-PLAN: the switch has a plain sentence");
        var disable = TunePlan("cpu-disable", s);
        Check(!disable.Refused && Writes(disable, "CpuTune-"), "turning it off removes the value");
        Refused(TunePlan("cpu-enable", s), "on already", "turning it on twice");
        Refused(TunePlan("cpu-disable", noTune), "off already", "turning it off twice");

        // Every action of the page has a dialog title in every language the application carries.
        var saved = Strings.Language;
        foreach (var lang in Strings.Languages)
        {
            Strings.Language = lang;
            foreach (var a in TunerPlan.Actions)
                Check(Strings.Has("plan.title." + a), lang + ": " + a + " has a dialog title");
            var q = TunePlan("tune-trial", s, args);
            Check(!PlainWords.Findings(q.Preview).Any(), "G-NOINT: " + lang + " trial dialog");
            Refused(TunePlan("tune-keep", s), "No curve trial is running", lang + ": a keep without a trial");
        }
        Strings.Language = saved;
        TunerStandardSteps();
    }

    // WU-042: one control puts the standard settings back. The driver stores the curve, the processor values and
    // the core mask itself, so they are not in the release's manifest.json: "Reset driver settings" has to send
    // the same escapes the tuning page sends, or it leaves a tuned machine behind and says it reset it.
    static void TunerStandardSteps()
    {
        var c = CurveFixture(CurveState.FlagValid | CurveState.FlagGoverning | CurveState.FlagStored);
        c.Stored = Tuner.Preset("deep", Tuner.Table(), Tuner.Floors());
        c.Active = c.Stored;
        var u = CpuFixture(CpuState.FlagValid | CpuState.FlagTuneOn | CpuState.FlagStored);
        u.StoredMaxMHz = 3200; u.StoredUvSteps = 6; u.AppliedMaxMHz = 3200; u.AppliedUvSteps = 6;
        u.CoreMaskStored = CpuTuning.MaskFull;
        var s = Tuned(c, u);
        var p = TunePlan("reset-defaults", s, new Recovery.PlanArgs { Games = "keep" });
        Check(!p.Refused, "a tuned machine can be reset: " + p.Refusal);
        var kinds = p.TuneSteps.Select(x => x.Kind).ToArray();
        Check(kinds.SequenceEqual(new[] { "curve-reset", "cpu-readback", "cpu-reset", "core-mask" }),
            "the reset takes the curve, the processor values and the core mask back: " + string.Join(", ", kinds));
        Equal(CpuTuning.MaskStock, p.TuneSteps.Last().CoreMask, "the core mask goes back to the stock one");
        Check(p.Writes.Any(w => w.Name == "CpuTune" && w.Delete), "processor tuning itself goes back off");
        Check(p.OfferRestart, "a core count takes a restart, and the dialog says so");
        Check(p.Text().Contains("tuning step curve-reset") && p.Text().Contains("tuning step core-mask"),
            "the log names every step the helper will send");
        var plain = PlainPlan.Describe(p);
        Check(plain.Changes.Count >= 4 && plain.Changes.All(x => !x.StartsWith("[", StringComparison.Ordinal)),
            "the dialog lists the tuning steps as changes");
        Check(!PlainWords.Findings(plain.Changes.Concat(plain.Notes).ToList()).Any(), "G-NOINT: the reset dialog");

        // A start that cannot take them back says so instead of claiming a reset it did not do.
        var fixedClock = Tuned(CurveFixture(CurveState.FlagValid | CurveState.FlagStored), u);
        fixedClock.Curve.Stored = c.Stored;
        var q = TunePlan("reset-defaults", fixedClock, new Recovery.PlanArgs { Games = "keep" });
        Check(!q.Refused && !q.TuneSteps.Any(x => x.Kind == "curve-reset"), "a start that does not govern sends no curve reset");
        Check(q.Notes.Any(x => x.Contains("stay stored")), "and the plan says the curve stays stored");

        // Nothing stored: the action is the one it was before this fix, with no tuning step at all.
        var clean = Tuned();
        clean.Parameters.Remove("CpuTune");
        var r = TunePlan("reset-defaults", clean, new Recovery.PlanArgs { Games = "keep" });
        Check(r.TuneSteps.Count == 0, "an untuned machine needs no tuning step");
        Check(!r.Writes.Any(w => w.Name == "CpuTune"), "and CpuTune is not touched when it is not there");

        // WU-042 asks for the risk before the change: the dialog says it in the window's language.
        var trial = TunePlan("tune-trial", Tuned(), new Recovery.PlanArgs
            { Curve = TunerPlan.CurveText(Tuner.Preset("mild", Tuner.Table(), Tuner.Floors())) });
        Check(trial.PlainNotes.Count == 1, "a curve trial carries one plain risk sentence");
        var dialog = PlainPlan.Describe(trial);
        Check(dialog.Notes.Contains(trial.PlainNotes[0]), "and the dialog shows it");
        Check(dialog.Changes.Count >= 2, "and still lists what changes");
        var cpu = TunePlan("cpu-trial", Tuned(), new Recovery.PlanArgs { CpuUv = 8 });
        Check(cpu.PlainNotes.Count == 1 && PlainPlan.Describe(cpu).Notes.Contains(cpu.PlainNotes[0]),
            "a processor trial says its own risk");
        var cores = TunePlan("core-mask", Tuned(), new Recovery.PlanArgs { Cores = 8 });
        Check(cores.PlainNotes.Count == 1, "eight cores say what they cost");
        Check(!PlainWords.Findings(trial.PlainNotes.Concat(cpu.PlainNotes).Concat(cores.PlainNotes).ToList()).Any(),
            "G-NOINT: the risk sentences");
    }

    // The chart lives on the page it rebuilds. A knot that reported every pixel of a drag would dispose the control
    // in the middle of its own mouse handler, so the gate holds the report until the handler ends. These checks are
    // the pure half; the source checks below keep the control using it the only safe way.
    static void TunerInputGate(string root)
    {
        var g = new Tuner.ChangeGate();
        Check(!g.Held, "gate: open before a handler");
        Check(g.Mark(true) == Tuner.Raise.Changed, "gate: a change outside a handler reports at once");
        Check(g.Release() == Tuner.Raise.None, "gate: nothing is left over");

        g = new Tuner.ChangeGate();
        g.Hold();
        Check(g.Held, "gate: a handler holds it");
        Check(g.Mark(false) == Tuner.Raise.None, "gate: the selection waits");
        for (int i = 0; i < 40; i++) Check(g.Mark(true) == Tuner.Raise.None, "gate: drag pixel " + i + " waits");
        Check(g.Release() == Tuner.Raise.Changed, "gate: the drag reports once, at the end");
        Check(!g.Held, "gate: open again after the drag");
        Check(g.Release() == Tuner.Raise.None, "gate: a second release reports nothing");

        g = new Tuner.ChangeGate();
        g.Hold();
        g.Mark(false);
        Check(g.Release() == Tuner.Raise.Picked, "gate: a key that only moves the selection reports Picked");
        g.Hold();
        g.Mark(false);
        g.Mark(true);
        Check(g.Release() == Tuner.Raise.Changed, "gate: a moved knot outranks a moved selection");

        var src = Path.Combine(root, @"tools\win\amdgpu_wddm_control\src\TunerUi.cs");
        Check(File.Exists(src), "src/TunerUi.cs exists");
        if (!File.Exists(src)) return;
        var text = File.ReadAllText(src);
        Check(Regex.Matches(text, @"Changed\(this,").Count == 1, "TunerUi.cs raises Changed in one place only");
        Check(Regex.Matches(text, @"Picked\(this,").Count == 1, "TunerUi.cs raises Picked in one place only");
        foreach (var handler in new[] { "OnMouseDown", "OnMouseMove" })
            Check(!Body(text, handler).Contains("Fire("), "TunerUi.cs " + handler + " reports nothing while the drag runs");
        foreach (var handler in new[] { "OnMouseUp", "OnMouseCaptureChanged" })
        {
            var body = Body(text, handler);
            int fire = body.IndexOf("Fire(", StringComparison.Ordinal), rest = body.IndexOf("base.", StringComparison.Ordinal);
            Check(fire > 0 && rest > 0 && fire > rest, "TunerUi.cs " + handler + " reports after the control is done with itself");
        }
        var keys = Body(text, "OnKeyDown");
        Check(keys.IndexOf("Fire(", StringComparison.Ordinal) > keys.IndexOf("e.Handled", StringComparison.Ordinal),
            "TunerUi.cs OnKeyDown reports after it has taken the key");
    }

    // The text of one method, from its signature to the line that closes it at the same indentation.
    static string Body(string text, string name)
    {
        var m = Regex.Match(text, @"(?m)^(?<pad>[ ]+)[\w ]*\b" + name + @"\(.*?\n(?<body>(.|\n)*?)^\k<pad>\}");
        return m.Success ? m.Groups["body"].Value : "";
    }

    // The design document is the contract this page implements: it has to name the numbers the code uses.
    static void TunerDesignDoc(string root)
    {
        var path = Path.Combine(root, @"docs\design\tuner.md");
        Check(File.Exists(path), "docs/design/tuner.md exists");
        if (!File.Exists(path)) return;
        var doc = File.ReadAllText(path);
        foreach (var want in new[] { Tuner.Points.ToString(), Tuner.BandMv + " mV", Tuner.FloorMv + " mV", Tuner.CeilingMv + " mV",
            CpuTuning.MinMHz.ToString(), CpuTuning.MaxMHz.ToString(), CpuTuning.MaxSteps.ToString(), CpuTuning.RefuseMv + " mV" })
            Check(doc.Contains(want), "docs/design/tuner.md names " + want);
        Check(!doc.Contains("—"), "docs/design/tuner.md has no em dash");
    }
}
