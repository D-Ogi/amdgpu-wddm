// Host test of the operating-point and measurement-guard panels: the real rules, the real expectations reader,
// the real DpmFeed and the real providers against built views and a scripted source. No control DLL, adapter,
// escape, window or UI thread is involved; the registry is only read where a provider reads it anyway, and a
// key that does not exist on this computer is one of the cases under test.
//
// Every amber and every red rule of the two panels has a check here, because these rules are what stops a
// measurement being taken on a machine that is not in the state the measurement assumes.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Threading;
using Bc250Mon;

sealed class FakeLabSource : ILabStateSource
{
    public object CuMode, Health, Interop;
    public int CuCalls, HealthCalls, InteropCalls;
    public CuModeSnapshot ReadCuMode()
    {
        CuCalls++;
        if (CuMode is Exception) throw (Exception)CuMode;
        return (CuModeSnapshot)CuMode;
    }
    public StartHealthSnapshot ReadStartHealthSnapshot()
    {
        HealthCalls++;
        if (Health is Exception) throw (Exception)Health;
        return (StartHealthSnapshot)Health;
    }
    public InteropSnapshot ReadInterop()
    {
        InteropCalls++;
        if (Interop is Exception) throw (Exception)Interop;
        return (InteropSnapshot)Interop;
    }
}

sealed class FakeDpmSource : ITelemetrySource
{
    public object Dpm;
    public ClockSnapshot Clock = new ClockSnapshot { AbiVersion = 1, Ready = 1, ObservedMHz = 1000, TemperatureMc = 60000 };
    public VideoMemorySnapshot Vram = new VideoMemorySnapshot { Size = 264 };
    public DpmSnapshot ReadDpm()
    {
        if (Dpm is Exception) throw (Exception)Dpm;
        return (DpmSnapshot)Dpm;
    }
    public ClockSnapshot ReadClock() { return Clock; }
    public VideoMemorySnapshot ReadVideoMemory() { return Vram; }
    // The gate is closed by every install, so that is what this source answers: no fan reading, and no note.
    public HwmonSnapshot ReadHwmon() { return LabStateTest.ClosedGateFan(); }
}

static class LabStateTest
{
    static int checks, failures;
    static void Check(bool value, string name) { checks++; if (!value) { failures++; Console.WriteLine("FAIL " + name); } }

    internal static HwmonSnapshot ClosedGateFan()
    {
        return new HwmonSnapshot
        {
            Magic = 0x30353242, Command = 27, AbiVersion = 1, Op = 0, Flags = HwmonSnapshot.FlagGated, Reason = 1,
            Rpm = new uint[8], DutyPermille = new uint[8], TemperatureMc = new int[4], TemperatureSource = new uint[4],
        };
    }

    static readonly DateTime Now = new DateTime(2026, 10, 5, 12, 0, 0, DateTimeKind.Utc);

    static Row Find(IEnumerable<Row> rows, string label)
    {
        return rows.FirstOrDefault(r => r.Label == label);
    }
    static string Value(IEnumerable<Row> rows, string label)
    {
        Row row = Find(rows, label);
        return row == null ? null : row.Value;
    }
    static Level LevelOf(IEnumerable<Row> rows, string label)
    {
        Row row = Find(rows, label);
        return row == null ? Level.Info : row.Level;
    }

    // ---- builders -------------------------------------------------------------------------------------------

    static CuModeSnapshot Escape(uint requested, uint applied, uint counted, uint flags, uint reason)
    {
        return new CuModeSnapshot
        {
            Magic = 0x30353242, Command = 22, AbiVersion = 1, Op = 0,
            Requested = requested, Applied = applied, ActiveCus = counted, Flags = flags, Reason = reason,
            Generation = 3, PciId = 0x100213FE,
        };
    }
    // The lab's healthy state since 2026-10-05 02:12Z: 40 applied, 40 counted, valid, confirmed, consistent.
    const uint GoodCuFlags = CuModeSnapshot.FlagValid | CuModeSnapshot.FlagConfirmed
                             | CuModeSnapshot.FlagConsistent | CuModeSnapshot.FlagWrote;

    static CuModeView Cu40()
    {
        return new CuModeView
        {
            ParametersPresent = true, CuMode = 40, Confirmed = 40, LastApplied = 40, LastReason = 0,
            HaveEscape = true, Escape = Escape(40, 40, 40, GoodCuFlags, 0), EscapeUtc = Now,
        };
    }
    static DpmView Dpm(uint mode, uint flags, uint max, uint cap, uint throttle, uint errors, DateTime utc)
    {
        return new DpmView
        {
            Have = true, Utc = utc,
            Snapshot = new DpmSnapshot
            {
                Version = 0x000700CD, Mode = mode, Flags = flags, MaxMHz = max, CapMHz = cap,
                Throttle = throttle, Errors = errors, CurrentMHz = cap, TargetMHz = cap,
            },
        };
    }
    static DpmView DpmGood()
    {
        return Dpm(DpmView.ModeDpm, DpmView.FlagRunning | DpmView.FlagGoverning | DpmView.FlagConfirmed
                   | DpmSnapshot.FlagTemperature | DpmSnapshot.FlagClock, 1500, 1500, 0, 0, Now);
    }
    static StartHealthView Health(uint flags, ulong generation, ulong epoch, ulong completionAgeMs,
                                  ulong firstGeneration, ulong firstEpoch)
    {
        return new StartHealthView
        {
            Have = true, Utc = Now, FirstGeneration = firstGeneration, FirstEpoch = firstEpoch,
            Snapshot = new StartHealthSnapshot
            {
                Magic = 0x30353242, Command = 21, AbiVersion = 1, Flags = flags,
                Generation = generation, Epoch = epoch, Completed = 1234, LastCompletionAgeMs = completionAgeMs,
            },
        };
    }
    static StartHealthView HealthGood() { return Health(15, 3, 1, 400, 3, 1); }
    // BC250_ESCAPE_INTEROP as a full WDDM start publishes it. The lab's healthy state: both switches effective,
    // the session live because DWM's device presents through the Blt path, nothing unclean.
    static InteropSnapshot InteropEscape(uint effective, uint requested, uint flags, uint users, uint closedReason)
    {
        return new InteropSnapshot
        {
            Magic = 0x30353242, Command = 25, AbiVersion = 1, Op = 0,
            Flags = flags, Requested = requested, Effective = effective, Users = users,
            ClosedReason = closedReason, BootId = 41, Generation = 3,
        };
    }
    static InteropView InteropLive()
    {
        return new InteropView
        {
            ParametersPresent = true, LastState = 3 | (3 << 8), Session = 41, LastEnd = 1,
            HaveEscape = true, EscapeUtc = Now,
            Escape = InteropEscape(3, 3, InteropSnapshot.FlagValid | InteropSnapshot.FlagSession
                                   | InteropSnapshot.FlagPowerCallback, 1, 0),
        };
    }
    // InteropLastState as driver/kmd/interop.c writes it: effective | requested << 8. The lab reads 771. Used
    // for the fallback path of a control DLL without the Bc250Interop export.
    static InteropView Interop(int effective, int requested)
    {
        return new InteropView { ParametersPresent = true, LastState = effective | (requested << 8), LastEnd = 3 };
    }
    static InteropView InteropEscaped(uint effective, uint requested, uint flags, uint users, uint closedReason)
    {
        var v = InteropLive();
        v.Escape = InteropEscape(effective, requested, flags, users, closedReason);
        return v;
    }
    static OperatingPointView Point()
    {
        return new OperatingPointView
        {
            Cu = Cu40(), Dpm = DpmGood(), Health = HealthGood(), Interop = InteropLive(), NowUtc = Now,
        };
    }
    static LabExpectations Expect40() { return new LabExpectations(); }

    static List<Row> CuRows(CuModeView v, LabExpectations x)
    {
        var panel = new Panel();
        OperatingPointRules.AddCuRows(panel, v, x);
        return panel.Rows;
    }
    static List<Row> DpmRows(DpmView v)
    {
        var panel = new Panel();
        OperatingPointRules.AddDpmRows(panel, v, Now);
        return panel.Rows;
    }
    static List<Row> HealthRows(StartHealthView v)
    {
        var panel = new Panel();
        OperatingPointRules.AddStartHealthRows(panel, v);
        return panel.Rows;
    }
    static List<Row> InteropRows(InteropView v)
    {
        var panel = new Panel();
        OperatingPointRules.AddInteropRows(panel, v);
        return panel.Rows;
    }
    static List<Row> GuardRows(MeasurementGuardView v, LabExpectations x)
    {
        return MeasurementGuardRules.Rows(v, x);
    }

    static MeasurementGuardView Guard()
    {
        return new MeasurementGuardView
        {
            NowUtc = Now,
            Parameters = Parameters(new Dictionary<string, int>
            {
                { "CuMode", 40 }, { "DpmMode", 1 }, { "DpmMaxMHz", 1500 },
                { "EnableGpuPresentBlit", 1 }, { "EnableCddDwmInterop", 1 }, { "EnableGpuSubmit", 1 },
            }),
            Installed = InstalledDefaults.Parse(
                "{\"parameters\":{\"DpmMode\":1,\"DpmMaxMHz\":1500,\"EnableGpuPresentBlit\":1," +
                "\"EnableCddDwmInterop\":1,\"EnableGpuSubmit\":1},\"app_router\":{\"Mode\":\"gpu-default\"}}"),
            Markers = new List<LabMarker>
            {
                new LabMarker("STOP", false), new LabMarker("graphics-summary.pause", false),
                new LabMarker { Name = "radv-perftest.txt", Present = false, Value = "" },
            },
            Release = new ReleaseView
            {
                Version = "0.7.205.100-tester.11", InstallDir = @"C:\Program Files\amdgpu-wddm",
                InstalledUtc = "2026-10-04T23:34:00.0000000Z",
                ManifestKmdAbi = "0x000700CD", ManifestKmdBuild = "0.7.205.1", LiveKmdVersion = 0x000700CD,
                // The witness the start-confirm task wrote after this boot's logon: the loaded image is the
                // release's own .sys, by SHA-256, and the record is of this boot.
                WitnessVersion = "0.7.205.100-tester.11", WitnessKmdBuild = "0.7.205.1",
                WitnessKmdAbi = "0x000700CD", WitnessBootId = 190, BootId = 190,
            },
        };
    }
    static RegistryDwords Parameters(Dictionary<string, int> values)
    {
        var r = new RegistryDwords { Present = true };
        foreach (var pair in values) r.Values[pair.Key] = pair.Value;
        return r;
    }

    static int Main(string[] args)
    {
        // The lab runs a Polish Windows: every number on this panel must still be written the invariant way.
        Thread.CurrentThread.CurrentCulture = new CultureInfo("pl-PL");
        string root = args.Length > 0 ? args[0] : Path.GetTempPath();
        string dir = Path.Combine(root, "labstate-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try
        {
            ExpectationChecks(dir);
            CuChecks();
            DpmChecks();
            HealthChecks();
            InteropChecks();
            GuardChecks();
            KeyChecks();
            FeedChecks(dir);
            ProviderChecks(dir);
            CostCheck();
        }
        finally { try { Directory.Delete(dir, true); } catch { } }
        Console.WriteLine("Lab state: " + checks + " checks, " + failures + " failures");
        return failures == 0 ? 0 : 1;
    }

    // ---- expectations ---------------------------------------------------------------------------------------

    static void ExpectationChecks(string dir)
    {
        LabExpectations none = ExpectationsFile.Parse("{}");
        Check(none.CuMode == 40 && none.Parameters.Count == 0 && none.Error == null, "empty file: 40 CU expected");
        LabExpectations x = ExpectationsFile.Parse(
            "{\"schema\":1,\"cuMode\":24,\"parameters\":{\"DpmMaxMHz\":2000,\"KeepLog\":null}}");
        Check(x.CuMode == 24 && x.Parameters["DpmMaxMHz"] == 2000 && x.Parameters["KeepLog"] == null && x.Error == null,
              "file values, a null switches a check off");
        Check(x.Parameters.ContainsKey("dpmmaxmhz"), "parameter names are compared without case");
        LabExpectations broken = ExpectationsFile.Parse("{not json");
        Check(broken.CuMode == 40 && broken.Error != null, "a broken file keeps the defaults and says why");

        var file = new ExpectationsFile(dir);
        string path = Path.Combine(dir, ExpectationsFile.FileName);
        Check(file.Read().CuMode == 40 && !file.Read().FromFile, "no file: the default expectation, not an error");
        File.WriteAllText(path, "{\"cuMode\":24}");
        Check(file.Read().CuMode == 24 && file.Read().FromFile, "the file is picked up");
        // Two writes of the same length inside one filesystem timestamp tick would look unchanged, so the
        // test gives the second one its own write time rather than relying on the clock's resolution.
        File.WriteAllText(path, "{\"cuMode\":40}");
        File.SetLastWriteTimeUtc(path, DateTime.UtcNow.AddSeconds(2));
        Check(file.Read().CuMode == 40, "a rewritten file is re-read");
        File.WriteAllText(path, "{\"cuMode\":24,\"parameters\":{}}");
        Check(file.Read().CuMode == 24, "a rewrite of a different length is re-read whatever the clock did");
        // A transient failure (the operator's editor holding the file) must not latch the default expectations
        // until the file's write time or length changes again: the cache keys are stored after a successful read.
        File.WriteAllText(path, "{\"cuMode\":40,\"parameters\":{\"DpmMaxMHz\":1500}}");
        File.SetLastWriteTimeUtc(path, DateTime.UtcNow.AddSeconds(4));
        using (new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.None))
            Check(file.Read().Error != null, "a file that cannot be read is reported, not hidden");
        LabExpectations after = file.Read();
        Check(after.Error == null && after.Parameters.ContainsKey("DpmMaxMHz"),
              "and the next poll reads it again instead of latching the failure");

        File.Delete(path);
        Check(file.Read().CuMode == 40 && !file.Read().FromFile, "a deleted file falls back to the default");
    }

    // ---- compute units: the row the owner asked for ---------------------------------------------------------

    static void CuChecks()
    {
        LabExpectations x = Expect40();
        var rows = CuRows(Cu40(), x);
        Check(Value(rows, "CU") == "40 CU, 40 counted, confirmed" && LevelOf(rows, "CU") == Level.Good,
              "40 CU confirmed is green: " + Value(rows, "CU"));
        Check(Value(rows, "CU setting") == "CuMode 40, confirmed" && LevelOf(rows, "CU setting") == Level.Good,
              "the setting row names what the next start applies");
        Check(Find(rows, "CU reason") == null && Find(rows, "CU snapshot") == null, "a healthy state has two CU rows");

        // The regression of 2026-10-05: the tester install left CuMode out, the driver harvested to 24, and
        // every Witcher 3 and Rise of the Tomb Raider number of three days was a 24 CU number.
        var lost = new CuModeView
        {
            ParametersPresent = true, CuMode = null, LastApplied = 24, LastReason = 0,
            HaveEscape = true, Escape = Escape(0, 24, 24, CuModeSnapshot.FlagValid | CuModeSnapshot.FlagConsistent, 0),
        };
        rows = CuRows(lost, x);
        Check(LevelOf(rows, "CU") == Level.Warn && Value(rows, "CU").Contains("24 CU") && Value(rows, "CU").Contains("expected 40"),
              "24 CU against an expectation of 40 is amber: " + Value(rows, "CU"));
        Check(LevelOf(rows, "CU setting") == Level.Warn &&
              Value(rows, "CU setting") == "CuMode absent: the next start harvests to 24",
              "an absent CuMode is amber and says what the next start does");

        // The automatic fallback: 40 was asked for, 24 came out. Red, with the driver's own reason.
        var fallback = new CuModeView
        {
            ParametersPresent = true, CuMode = 24, LastApplied = 24, LastReason = 3,
            HaveEscape = true, Escape = Escape(40, 24, 24, CuModeSnapshot.FlagValid | CuModeSnapshot.FlagConsistent, 3),
        };
        rows = CuRows(fallback, x);
        Check(LevelOf(rows, "CU") == Level.Error, "requested 40, applied 24 is red");
        Check(LevelOf(rows, "CU reason") == Level.Error &&
              Value(rows, "CU reason") == "an earlier 40 CU start was never confirmed",
              "the fallback reason is named and red: " + Value(rows, "CU reason"));
        foreach (uint reason in new uint[] { 3, 6, 7, 8, 9 })
            Check(OperatingPointRules.IsFallbackReason(reason), "reason " + reason + " is a fallback");
        foreach (uint reason in new uint[] { 0, 1, 2, 4, 5, 10, 11 })
            Check(!OperatingPointRules.IsFallbackReason(reason), "reason " + reason + " is not a fallback");

        // 40 applied and nobody confirmed it: the next restart falls back, so it is amber now, not later.
        var pending = Cu40();
        pending.Confirmed = null;
        pending.Pending = 40;
        pending.Escape = Escape(40, 40, 40, CuModeSnapshot.FlagValid | CuModeSnapshot.FlagPending | CuModeSnapshot.FlagConsistent, 0);
        rows = CuRows(pending, x);
        Check(LevelOf(rows, "CU") == Level.Warn && Value(rows, "CU").Contains("PENDING"),
              "pending without confirmed is amber: " + Value(rows, "CU"));
        Check(Value(rows, "CU setting") == "CuMode 40, pending", "the registry pending mark is shown");

        // Nothing applied: the stage never ran, or the stock restore failed as well.
        var nothing = new CuModeView
        {
            ParametersPresent = true, CuMode = 40, LastApplied = 0, LastReason = 9,
            HaveEscape = true, Escape = Escape(40, 0, 0, CuModeSnapshot.FlagValid, 9),
        };
        Check(LevelOf(CuRows(nothing, x), "CU") == Level.Error, "0 CU applied is red");

        // CC and SPI name different WGPs: the count the driver reports and the dispatch mask disagree.
        var inconsistent = Cu40();
        inconsistent.Escape = Escape(40, 40, 40, CuModeSnapshot.FlagValid | CuModeSnapshot.FlagConfirmed, 0);
        Check(LevelOf(CuRows(inconsistent, x), "CU") == Level.Warn, "valid without consistent is amber");

        // The deployed control DLL of 2026-09-30 has no Bc250CuMode export. The registry record still answers.
        var old = new CuModeView
        {
            ParametersPresent = true, CuMode = 40, Confirmed = 40, LastApplied = 40, LastReason = 0,
            HaveEscape = false, EscapeError = "control DLL too old for CU mode",
        };
        rows = CuRows(old, x);
        Check(LevelOf(rows, "CU") == Level.Info && Value(rows, "CU") == "40 CU (driver record, no snapshot)",
              "no snapshot: the registry record, marked as such: " + Value(rows, "CU"));
        Check(Value(rows, "CU snapshot") == "control DLL too old for CU mode" && LevelOf(rows, "CU snapshot") == Level.Warn,
              "the old control DLL is named, the panel keeps working");

        // Neither a snapshot nor a record.
        var blind = new CuModeView { ParametersPresent = false, HaveEscape = false, EscapeError = "KMD CU mode snapshot unavailable (0xC00000BB)" };
        rows = CuRows(blind, x);
        Check(LevelOf(rows, "CU") == Level.Warn && Value(rows, "CU").Contains("unknown"), "no source at all is amber, never green");
        Check(Value(rows, "CU setting") == "no Parameters key", "no Parameters key is stated");

        // A lab that deliberately runs the harvest: the expectation file decides, not the code.
        var expect24 = new LabExpectations { CuMode = 24 };
        Check(LevelOf(CuRows(lost, expect24), "CU") == Level.Good, "expectations.json cuMode 24 makes 24 CU green");
        Check(LevelOf(CuRows(Cu40(), expect24), "CU") == Level.Warn, "and 40 CU amber against that expectation");

        // CuDisableWgp is a supported setting: mode 40 applies, one WGP stays masked, and the part runs 38 of
        // 40 units. The mode word says 40, so only the counted units answer the question the panel exists for.
        var masked = Cu40();
        masked.CuDisableWgp = 0x10;
        masked.Escape = Escape(40, 40, 38, GoodCuFlags, 0);
        rows = CuRows(masked, x);
        Check(LevelOf(rows, "CU") == Level.Warn && Value(rows, "CU").Contains("38 counted") &&
              Value(rows, "CU").Contains("expected 40"),
              "mode 40 with a masked WGP is judged on the counted units: " + Value(rows, "CU"));

        // The escape answered, but this start never reached the CU stage (a display-only start, or a full one
        // stopped at an earlier gate). CuModeLastApplied then still names the previous full start's 40, which
        // describes hardware this start never touched: unknown, never green.
        var notRun = new CuModeView
        {
            ParametersPresent = true, CuMode = 40, Confirmed = 40, LastApplied = 40, LastReason = 10,
            HaveEscape = true, Escape = Escape(0, 0, 0, 0, 10), EscapeUtc = Now,
        };
        rows = CuRows(notRun, x);
        Check(LevelOf(rows, "CU") == Level.Warn && Value(rows, "CU").Contains("did not run the CU stage") &&
              Value(rows, "CU").Contains("an earlier start applied 40"),
              "a start without the CU stage is unknown, not the old registry record: " + Value(rows, "CU"));
        Check(!notRun.Counted.HasValue && !notRun.Applied.HasValue, "neither a count nor a mode is claimed");
        Check(LevelOf(rows, "CU reason") == Level.Warn, "and the driver's NOT_RUN reason is beside it");

        // CuModePending and CuModeConfirmed hold bc250_cu_encode(mode, disable). A confirmation of plain 40
        // does not cover 40 with a WGP masked: the next start is PENDING, and a crash in it falls back to 24.
        var otherMark = Cu40();
        otherMark.CuDisableWgp = 0x10;
        otherMark.Confirmed = 40;
        rows = CuRows(otherMark, x);
        Check(LevelOf(rows, "CU setting") == Level.Warn &&
              Value(rows, "CU setting").Contains("names another request") &&
              Value(rows, "CU setting").Contains("WGP mask 0x10"),
              "a confirmation of another request is amber and named: " + Value(rows, "CU setting"));
        var encoded = Cu40();
        encoded.CuDisableWgp = 0x10;
        encoded.Confirmed = 40 | (0x10 << 8);
        Check(Value(CuRows(encoded, x), "CU setting").EndsWith(", confirmed"),
              "the matching encoded confirmation reads confirmed: " + Value(CuRows(encoded, x), "CU setting"));
        Check(LevelOf(CuRows(encoded, x), "CU setting") == Level.Warn,
              "but a mask keeps the next start under a 40 unit expectation, so the row stays amber");
        var unmarked = Cu40();
        unmarked.Confirmed = null;
        Check(Value(CuRows(unmarked, x), "CU setting") == "CuMode 40, the next start marks it pending",
              "a 40 request nobody confirmed says what the next start does: " + Value(CuRows(unmarked, x), "CU setting"));
        var stock = new CuModeView
        {
            ParametersPresent = true, CuMode = 24, Confirmed = 40, LastApplied = 24, LastReason = 0,
            HaveEscape = true, Escape = Escape(24, 24, 24, CuModeSnapshot.FlagValid | CuModeSnapshot.FlagConsistent, 0),
        };
        Check(Value(CuRows(stock, expect24), "CU setting") == "CuMode 24" &&
              LevelOf(CuRows(stock, expect24), "CU setting") == Level.Good,
              "a 24 request carries no mark at all: " + Value(CuRows(stock, expect24), "CU setting"));
    }

    // ---- the DPM governor -----------------------------------------------------------------------------------

    static void DpmChecks()
    {
        var rows = DpmRows(DpmGood());
        Check(Value(rows, "DPM") == "dpm, governing, confirmed" && LevelOf(rows, "DPM") == Level.Good,
              "a governing confirmed DPM start is green: " + Value(rows, "DPM"));
        Check(Value(rows, "DPM cap") == "cap 1500 of 1500 MHz, throttle none" && LevelOf(rows, "DPM cap") == Level.Good,
              "the cap row names cap, ceiling and throttle: " + Value(rows, "DPM cap"));

        DpmView pending = Dpm(DpmView.ModeDpm, DpmView.FlagRunning | DpmView.FlagGoverning | DpmView.FlagPending,
                              1500, 1500, 0, 0, Now);
        rows = DpmRows(pending);
        Check(LevelOf(rows, "DPM") == Level.Error && Value(rows, "DPM").Contains("PENDING"),
              "DPM pending without confirmed is red: a restart falls back to fixed-lab");

        DpmView notGoverning = Dpm(DpmView.ModeDpm, DpmView.FlagRunning | DpmView.FlagConfirmed, 1500, 1500, 7, 0, Now);
        Check(LevelOf(DpmRows(notGoverning), "DPM") == Level.Error, "mode DPM without governing is red");

        DpmView fixedLab = Dpm(DpmView.ModeFixed, DpmView.FlagRunning, 1500, 1000, 7, 0, Now);
        rows = DpmRows(fixedLab);
        Check(Value(rows, "DPM").StartsWith("fixed-lab") && Value(rows, "DPM cap").Contains("throttle fixed"),
              "a fixed-lab start is named as such: " + Value(rows, "DPM"));

        foreach (uint throttle in new uint[] { 1, 2, 8, 9 })
            Check(LevelOf(DpmRows(Dpm(DpmView.ModeDpm, DpmView.FlagRunning | DpmView.FlagGoverning | DpmView.FlagConfirmed,
                                      1500, 1500, throttle, 0, Now)), "DPM cap") == Level.Warn,
                  "thermal throttle " + throttle + " is amber");
        Check(LevelOf(DpmRows(Dpm(DpmView.ModeDpm, DpmView.FlagRunning | DpmView.FlagGoverning | DpmView.FlagConfirmed,
                                  1500, 1500, 6, 0, Now)), "DPM cap") == Level.Error, "throttle smu is red");
        Check(LevelOf(DpmRows(Dpm(DpmView.ModeDpm, DpmView.FlagRunning | DpmView.FlagGoverning | DpmView.FlagConfirmed,
                                  1500, 1000, 1, 0, Now)), "DPM cap") == Level.Warn, "a thermal cap under the ceiling is amber");
        // Errors is cumulative for the start and the governor gives up only after three failures in a row, so a
        // recovered SMU retry during a game must not paint the panel red for the rest of the start.
        rows = DpmRows(Dpm(DpmView.ModeDpm, DpmView.FlagRunning | DpmView.FlagGoverning | DpmView.FlagConfirmed,
                           1500, 1500, 0, 4, Now));
        Check(LevelOf(rows, "DPM") == Level.Good && LevelOf(rows, "DPM cap") == Level.Warn &&
              Value(rows, "DPM cap").Contains("4 SMU errors"),
              "recovered SMU errors are counted and amber, not red: " + Value(rows, "DPM cap"));
        Check(LevelOf(DpmRows(Dpm(DpmView.ModeDpm, DpmView.FlagRunning | DpmView.FlagConfirmed, 1500, 1500, 6, 9, Now)),
                      "DPM cap") == Level.Error,
              "the governor giving up is red: throttle smu, whatever the count");

        foreach (uint flag in new uint[] { DpmView.FlagPaused, DpmView.FlagStable })
            Check(LevelOf(DpmRows(Dpm(DpmView.ModeDpm, DpmView.FlagRunning | DpmView.FlagGoverning | DpmView.FlagConfirmed | flag,
                                      1500, 1500, 0, 0, Now)), "DPM") == Level.Warn, "flag " + flag + " is amber");
        Check(Value(DpmRows(Dpm(DpmView.ModeDpm, DpmView.FlagRunning | DpmView.FlagGoverning | DpmView.FlagConfirmed
                                | DpmView.FlagStable, 1500, 1500, 0, 0, Now)), "DPM").Contains("SetStablePowerState"),
              "SetStablePowerState is named, not just flagged");

        rows = DpmRows(Dpm(DpmView.ModeDpm, DpmView.FlagRunning | DpmView.FlagGoverning | DpmView.FlagConfirmed,
                           1500, 1500, 0, 0, Now.AddSeconds(-45)));
        Check(LevelOf(rows, "DPM") == Level.Warn && Value(rows, "DPM").Contains("45 s old"),
              "a stale snapshot is amber with its age: " + Value(rows, "DPM"));
        Check(LevelOf(DpmRows(new DpmView { Error = "KMD DPM snapshot unavailable (0xC00000A3)" }), "DPM") == Level.Warn,
              "no snapshot is amber with the reason");
        Check(Value(DpmRows(new DpmView()), "DPM") == "no snapshot yet", "before the first sample the row says so");
        // A failure after a good snapshot keeps the snapshot in the feed; the reason must still reach the row.
        DpmView failing = DpmGood();
        failing.Error = "KMD DPM snapshot unavailable (0xC00000A3)";
        rows = DpmRows(failing);
        Check(LevelOf(rows, "DPM") == Level.Warn && Value(rows, "DPM").Contains("0xC00000A3"),
              "a driver that stopped answering says why, not only how old the snapshot is: " + Value(rows, "DPM"));
    }

    // ---- start health ---------------------------------------------------------------------------------------

    static void HealthChecks()
    {
        var rows = HealthRows(HealthGood());
        Check(LevelOf(rows, "Start health") == Level.Good &&
              Value(rows, "Start health") == "flags 15 (full ready visible confirmed), gen 3 epoch 1",
              "flags 15 is green: " + Value(rows, "Start health"));
        Check(LevelOf(HealthRows(Health(7, 3, 1, 400, 3, 1)), "Start health") == Level.Warn,
              "flags 7 (not confirmed) is amber");
        rows = HealthRows(Health(15, 3, 1, 40000, 3, 1));
        Check(LevelOf(rows, "Start health") == Level.Warn && Value(rows, "Start health").Contains("40 s ago"),
              "presentation stalled over 15 s is amber with the age: " + Value(rows, "Start health"));
        Check(LevelOf(HealthRows(Health(1, 3, 1, 400, 3, 1)), "Start health") == Level.Error,
              "full table without ready is red");
        rows = HealthRows(Health(15, 9, 1, 400, 5, 1));
        Check(LevelOf(rows, "Start health") == Level.Error && Value(rows, "Start health").Contains("changed in this session"),
              "a generation change inside a session is red: every number taken before it is suspect");
        // The 2026-10-11 lab: gen 28360456 kept, epoch 5 -> 13 from W3's mode changes, and the row stayed red
        // until the next boot. A newer epoch of the same start is a note, not an alarm.
        rows = HealthRows(Health(15, 3, 9, 400, 3, 1));
        Check(LevelOf(rows, "Start health") == Level.Good &&
              Value(rows, "Start health").Contains("epoch +8 since logon") &&
              !Value(rows, "Start health").Contains("changed in this session"),
              "an epoch change of the same start keeps the level and says the count: " + Value(rows, "Start health"));
        Check(LevelOf(HealthRows(Health(15, 3, 9, 40000, 3, 1)), "Start health") == Level.Warn,
              "an epoch change does not hide a stall");
        Check(Health(15, 9, 4, 400, 5, 1).EpochsSinceFirst == 0,
              "a new generation is not counted as epochs: it is the red case above");
        Check(!Health(15, 3, 1, 400, 0, 0).IdentityChanged, "before the first identity nothing has changed");
        // start_health.c returns ~0ull while nothing has completed, and HealthInvalidate zeroes the record at
        // every visibility or mode change. Printed as an age that read "18446744073709552 s ago", and on the
        // CPU desktop route, which completes no GPU presentation at all, it read that for ever.
        rows = HealthRows(Health(15, 3, 1, ulong.MaxValue, 3, 1));
        Check(LevelOf(rows, "Start health") == Level.Good &&
              Value(rows, "Start health").Contains("no completed presentation yet") &&
              !Value(rows, "Start health").Contains("18446744073709552"),
              "no completion yet is said in words, not as an age: " + Value(rows, "Start health"));
        Check(!Health(15, 3, 1, ulong.MaxValue, 3, 1).Stalled && Health(15, 3, 1, ulong.MaxValue, 3, 1).NothingCompleted,
              "the sentinel is not a stall");
        Check(Health(15, 3, 1, 40000, 3, 1).Stalled, "a real age over the bound is one");
        Check(LevelOf(HealthRows(new StartHealthView { Error = "KMD start health failed" }), "Start health") == Level.Warn,
              "a failed read is amber with the reason");
        Check(Value(HealthRows(Health(0, 0, 0, 0, 0, 0)), "Start health").Contains("(none)"),
              "no flags at all reads as none, not as an empty bracket");
    }

    // ---- GPU desktop interop switches -----------------------------------------------------------------------

    static void InteropChecks()
    {
        // The state the lab runs in: DWM's device presents through the Blt path, so InteropSession is on disk
        // and InteropLastEnd keeps the end of some earlier session for ever. Both are normal, and the first
        // version of this panel turned them into a permanent amber row on exactly this configuration.
        var rows = InteropRows(InteropLive());
        Check(LevelOf(rows, "Interop") == Level.Good &&
              Value(rows, "Interop") == "effective blit+cdd, as requested; session live, 1 device",
              "a live session on the GPU desktop route is green: " + Value(rows, "Interop"));
        Check(Interop(3, 3).LastState == 771, "the lab's InteropLastState 771 is effective 3, requested 3");
        Check(LevelOf(InteropRows(InteropEscaped(3, 3, InteropSnapshot.FlagValid | InteropSnapshot.FlagSession, 3, 0)),
                      "Interop") == Level.Good &&
              Value(InteropRows(InteropEscaped(3, 3, InteropSnapshot.FlagValid | InteropSnapshot.FlagSession, 3, 0)),
                    "Interop").Contains("3 devices"), "the device count is text, never a level");

        rows = InteropRows(InteropEscaped(0, 3, InteropSnapshot.FlagValid, 0, 0));
        Check(LevelOf(rows, "Interop") == Level.Error && Value(rows, "Interop").Contains("closed the GPU desktop path"),
              "effective different from requested is red: " + Value(rows, "Interop"));
        Check(LevelOf(InteropRows(InteropEscaped(1, 1, InteropSnapshot.FlagValid, 0, 0)), "Interop") == Level.Error,
              "one switch alone is red: the GPU desktop path is not complete");

        // Only the start can say that a marker is the trace of a dead machine, and only in these flags.
        rows = InteropRows(InteropEscaped(0, 3, InteropSnapshot.FlagValid | InteropSnapshot.FlagUnclean
                                          | InteropSnapshot.FlagClosedByDriver | InteropSnapshot.FlagPersisted, 0, 4));
        Check(LevelOf(rows, "Interop") == Level.Error && Value(rows, "Interop").Contains("died with the path in use") &&
              Value(rows, "Interop").Contains("closed by the driver (unclean)"),
              "an unclean start is red and names the driver's reason: " + Value(rows, "Interop"));
        rows = InteropRows(InteropEscaped(3, 3, InteropSnapshot.FlagValid | InteropSnapshot.FlagStale, 0, 0));
        Check(LevelOf(rows, "Interop") == Level.Warn && Value(rows, "Interop").Contains("marker of this boot"),
              "a stale marker of this boot is amber, not red: " + Value(rows, "Interop"));
        rows = InteropRows(InteropEscaped(0, 3, InteropSnapshot.FlagValid | InteropSnapshot.FlagUnclean
                                          | InteropSnapshot.FlagPersistFailed, 0, 4));
        Check(Value(rows, "Interop").Contains("the durable close failed"), "a failed durable close is named");
        rows = InteropRows(InteropEscaped(3, 3, InteropSnapshot.FlagValid | InteropSnapshot.FlagDown, 1, 0));
        Check(LevelOf(rows, "Interop") == Level.Warn && Value(rows, "Interop").Contains("system power transition"),
              "a system power transition in progress is amber");

        // A display-only start, or one that stopped before the decision: the zeros mean nothing.
        // bc250_interop_decide leaves BC250_INTEROP_REASON_NOT_RUN = 7 behind and no VALID flag.
        var notDecided = InteropEscaped(0, 0, 0, 0, 0);
        notDecided.Escape.Reason = 7;
        rows = InteropRows(notDecided);
        Check(LevelOf(rows, "Interop") == Level.Warn && Value(rows, "Interop").Contains("not-run"),
              "no full start decided the switches: amber with the driver's reason, not a closed path: "
              + Value(rows, "Interop"));

        // The fallback for a control DLL without the export: the mirror answers, and its two normal states
        // (a live session, the last end of any boot) never change the level.
        rows = InteropRows(Interop(3, 3));
        Check(LevelOf(rows, "Interop") == Level.Good && Value(rows, "Interop").Contains("registry mirror, no snapshot"),
              "the mirror alone is green when the switches are as requested: " + Value(rows, "Interop"));
        var marked = Interop(3, 3);
        marked.Session = 41;
        marked.LastEnd = 1;
        Check(LevelOf(InteropRows(marked), "Interop") == Level.Good &&
              Value(InteropRows(marked), "Interop").Contains("session marked") &&
              Value(InteropRows(marked), "Interop").Contains("previous end device stop"),
              "a session marker and a device-stop end are text in the mirror path, never amber: "
              + Value(InteropRows(marked), "Interop"));
        Check(LevelOf(InteropRows(new InteropView { ParametersPresent = true }), "Interop") == Level.Info,
              "no record is neutral, not a claim");
        Check(Value(InteropRows(new InteropView { ParametersPresent = true, EscapeError = "control DLL too old for the interop snapshot" }),
                    "Interop") == "control DLL too old for the interop snapshot",
              "with neither snapshot nor mirror the reason is on the panel");
        Check(LabNames.InteropSwitches(0) == "none" && LabNames.InteropSwitches(2) == "cdd" &&
              LabNames.InteropSwitches(7) == "blit+cdd+0x4", "switch names, unknown bits kept visible");
        Check(LabNames.InteropEnd(3) == "system power" && LabNames.InteropEnd(77) == "end 77", "end names");
        Check(LabNames.InteropReasonName(0) == "none" && LabNames.InteropReasonName(4) == "unclean" &&
              LabNames.InteropReasonName(7) == "not-run" && LabNames.InteropReasonName(99) == "reason 99",
              "interop reason names follow the driver's own table");
    }

    // ---- the measurement guard ------------------------------------------------------------------------------

    static void GuardChecks()
    {
        LabExpectations x = Expect40();
        var rows = GuardRows(Guard(), x);
        Check(Value(rows, "Parameters") == "6 values as expected" && LevelOf(rows, "Parameters") == Level.Good,
              "the installed defaults plus CuMode, all matching: " + Value(rows, "Parameters"));
        Check(Find(rows, "Defaults") == null, "a present AppliedDefaults record adds no row");
        Check(Value(rows, "Markers") == "none set" && LevelOf(rows, "Markers") == Level.Good, "no marker is green");
        Check(Value(rows, "Release").StartsWith("0.7.205.100-tester.11, installed 12.4 h ago"),
              "the release row names version and age: " + Value(rows, "Release"));
        Check(Value(rows, "KMD image") == "0x000700CD = the release's 0.7.205.1, image witnessed" &&
              LevelOf(rows, "KMD image") == Level.Good,
              "a witnessed image of the installed release is green: " + Value(rows, "KMD image"));

        // The 2026-10-05 shape of the regression, in its general form: a value the installer never carried.
        var lost = Guard();
        lost.Parameters.Values.Remove("CuMode");
        rows = GuardRows(lost, x);
        Check(LevelOf(rows, "Parameters") == Level.Warn && Value(rows, "Parameters") == "CuMode absent, so 24 (expected 40)",
              "a missing expected value is amber, named, and says what is in force: " + Value(rows, "Parameters"));

        var raised = Guard();
        raised.Parameters.Values["DpmMaxMHz"] = 2000;
        rows = GuardRows(raised, x);
        Check(LevelOf(rows, "Parameters") == Level.Warn && Value(rows, "Parameters").Contains("DpmMaxMHz 2000 (expected 1500)"),
              "a value above the installed default is amber: " + Value(rows, "Parameters"));
        var admitted = new LabExpectations();
        admitted.Parameters["DpmMaxMHz"] = 2000;
        Check(LevelOf(GuardRows(raised, admitted), "Parameters") == Level.Good,
              "expectations.json admits a deliberate deviation");
        var off = new LabExpectations();
        off.Parameters["DpmMaxMHz"] = null;
        Check(LevelOf(GuardRows(raised, off), "Parameters") == Level.Good, "a null expectation switches the check off");

        foreach (string gate in MeasurementGuardRules.LatchedGates)
        {
            var closed = Guard();
            closed.Parameters.Values[gate] = 0;
            Check(LevelOf(GuardRows(closed, x), "Parameters") == Level.Error, gate + " at 0 is red, not amber");
            Check(MeasurementGuardRules.IsLatchedGate(gate.ToLowerInvariant()), gate + " is a latched gate");
        }
        Check(!MeasurementGuardRules.IsLatchedGate("KeepLog"), "KeepLog is not a latched gate");

        // Absent is not always a deviation. The two interop switches are on when absent (since 0.7.181), so a
        // key written by the driver package rather than the installer used to show amber for settings that are
        // in force at the expectation.
        foreach (string name in new[] { "EnableGpuPresentBlit", "EnableCddDwmInterop" })
        {
            var gone = Guard();
            gone.Parameters.Values.Remove(name);
            Check(LevelOf(GuardRows(gone, x), "Parameters") == Level.Good,
                  name + " absent means on, so it is green: " + Value(GuardRows(gone, x), "Parameters"));
            Check(MeasurementGuardRules.AbsentDefault(name) == 1, name + " absent default is 1");
        }
        // And where absent means something else it stays a deviation, with what is in force spelled out.
        var noDpm = Guard();
        noDpm.Parameters.Values.Remove("DpmMode");
        rows = GuardRows(noDpm, x);
        Check(LevelOf(rows, "Parameters") == Level.Error && Value(rows, "Parameters").Contains("DpmMode absent, so 0"),
              "an absent DpmMode is fixed-lab, which is red for a latched gate: " + Value(rows, "Parameters"));
        Check(MeasurementGuardRules.AbsentDefault("CuMode") == 24 && MeasurementGuardRules.AbsentDefault("DpmMaxMHz") == 2000 &&
              MeasurementGuardRules.AbsentDefault("EnableGpuSubmit") == 0,
              "the absent defaults come from the driver's own readers");
        Check(MeasurementGuardRules.AbsentDefault("SomethingNobodyKnows") == null,
              "an unknown name has no default, so absent stays a difference");
        var unknownName = Guard();
        unknownName.Installed = InstalledDefaults.Parse("{\"parameters\":{\"SomethingNobodyKnows\":1}}");
        unknownName.Parameters.Values.Remove("CuMode");
        rows = GuardRows(unknownName, x);
        Check(Value(rows, "Parameters").Contains("SomethingNobodyKnows absent (expected 1)"),
              "and it is reported as plain absent: " + Value(rows, "Parameters"));
        Check(MeasurementGuardRules.InForce(Guard().Parameters, "DpmMaxMHz") == 1500 &&
              MeasurementGuardRules.InForce(new RegistryDwords(), "EnableCddDwmInterop") == 1 &&
              MeasurementGuardRules.InForce(new RegistryDwords(), "NoSuchValue") == null,
              "InForce is the value in the key, else the driver's default");

        var many = Guard();
        many.Parameters.Values["DpmMaxMHz"] = 2000;
        many.Parameters.Values["EnableGpuPresentBlit"] = 2;
        many.Parameters.Values["EnableCddDwmInterop"] = 2;
        many.Parameters.Values.Remove("CuMode");
        rows = GuardRows(many, x);
        Check(Value(rows, "Parameters").Contains("+1 more") && LevelOf(rows, "Parameters") == Level.Warn,
              "four differences are shown as three plus a count: " + Value(rows, "Parameters"));

        var noRecord = Guard();
        noRecord.Installed = new InstalledDefaults();
        rows = GuardRows(noRecord, x);
        Check(Value(rows, "Parameters") == "1 value as expected" && LevelOf(rows, "Defaults") == Level.Warn &&
              Value(rows, "Defaults").Contains("CuMode only"),
              "without the installer's record only CuMode is checked, and the panel says so");

        var noKey = Guard();
        noKey.Parameters = new RegistryDwords();
        Check(LevelOf(GuardRows(noKey, x), "Parameters") == Level.Warn, "no Parameters key is amber");

        // Markers.
        var paused = Guard();
        paused.Markers = new List<LabMarker>
        {
            new LabMarker("graphics-summary.pause", true), new LabMarker("graphics-api.pause", true),
            new LabMarker { Name = "radv-perftest.txt", Present = true, Value = "rt_wave64", WriteUtc = Now.AddHours(-2) },
            new LabMarker { Name = "radv.cfg", Present = true, Value = "" },
        };
        rows = GuardRows(paused, x);
        Check(LevelOf(rows, "Markers") == Level.Warn, "any marker set is amber");
        Check(Value(rows, "Markers").Contains("graphics-summary.pause") && Value(rows, "Markers").Contains("graphics-api.pause"),
              "the pause markers are named: " + Value(rows, "Markers"));
        Check(Value(rows, "Markers").Contains("radv-perftest.txt = rt_wave64, 2.0 h old"),
              "an armed marker shows its content and its age, so the staleness rule can be applied");
        Check(!Value(rows, "Markers").Contains("radv.cfg"), "a present but empty file arms nothing");
        Check(MeasurementGuardRules.Short(new string('x', 40)).Length == 28, "a long marker value is shortened");
        Check(MeasurementGuardRules.Age(TimeSpan.FromSeconds(30)) == "30 s" &&
              MeasurementGuardRules.Age(TimeSpan.FromMinutes(5)) == "5 min" &&
              MeasurementGuardRules.Age(TimeSpan.FromHours(3)) == "3.0 h" &&
              MeasurementGuardRules.Age(TimeSpan.FromDays(2)) == "2.0 d", "ages are invariant in every unit");

        // A module swapped under a release that claims otherwise: every hand swap of a candidate triplet.
        var swapped = Guard();
        swapped.Release.LiveKmdVersion = 0x000700CC;
        rows = GuardRows(swapped, x);
        Check(LevelOf(rows, "KMD image") == Level.Error && Value(rows, "KMD image").Contains("a module was swapped"),
              "a live KMD other than the release's is red: " + Value(rows, "KMD image"));
        var unknown = Guard();
        unknown.Release.LiveKmdVersion = 0;
        Check(LevelOf(GuardRows(unknown, x), "KMD image") == Level.Warn, "an unknown live version is amber, never green");

        // One kmd_abi covers every build of a driver revision (tools/release/test-dryrun.ps1 admits them all),
        // so a candidate .sys of the same revision matches the ABI word. Only the witness names the image.
        var noWitness = Guard();
        noWitness.Release.WitnessKmdAbi = null;
        noWitness.Release.WitnessKmdBuild = null;
        rows = GuardRows(noWitness, x);
        Check(LevelOf(rows, "KMD image") == Level.Warn && Value(rows, "KMD image").Contains("the release's ABI") &&
              Value(rows, "KMD image").Contains("not witnessed (no running-release witness)"),
              "a matching ABI without a witness says so instead of claiming the image: " + Value(rows, "KMD image"));
        var oldBoot = Guard();
        oldBoot.Release.WitnessBootId = 189;
        Check(Value(GuardRows(oldBoot, x), "KMD image").Contains("from another boot (189, now 190)"),
              "a witness of an earlier boot is named: " + Value(GuardRows(oldBoot, x), "KMD image"));
        var noBoot = Guard();
        noBoot.Release.BootId = 0;
        Check(Value(GuardRows(noBoot, x), "KMD image").Contains("the current boot cannot be read"),
              "an unreadable BootId is named");
        var otherRelease = Guard();
        otherRelease.Release.WitnessVersion = "0.7.204.100-tester.11";
        Check(Value(GuardRows(otherRelease, x), "KMD image").Contains("witness is of release 0.7.204.100-tester.11"),
              "a witness of another release is named");
        var witnessAbi = Guard();
        witnessAbi.Release.WitnessKmdAbi = "0x000700CC";
        Check(Value(GuardRows(witnessAbi, x), "KMD image").Contains("the driver replies"),
              "a witness that names another ABI than the driver replies is named");
        var damaged = Guard();
        damaged.Release.WitnessProblem = "running-release.json is damaged or of an unknown schema";
        Check(Value(GuardRows(damaged, x), "KMD image").Contains("damaged or of an unknown schema"),
              "a damaged witness file is named");
        var noManifest = Guard();
        noManifest.Release.ManifestKmdAbi = null;
        noManifest.Release.ManifestError = "no manifest.json in C:\\Program Files\\amdgpu-wddm";
        Check(Value(GuardRows(noManifest, x), "KMD image").Contains("no manifest.json"), "a missing manifest is named");
        var noRelease = Guard();
        noRelease.Release.Version = null;
        Check(LevelOf(GuardRows(noRelease, x), "Release") == Level.Warn, "no release record is amber");

        // The installer's record is JSON written by the installer; a broken one must not take the panel down.
        Check(!InstalledDefaults.Parse("{broken").Present && InstalledDefaults.Parse("{broken").Error != null,
              "a broken AppliedDefaults record is reported, not thrown");
        Check(!InstalledDefaults.Parse("{\"desktop_router\":{}}").Present, "a record without parameters is not present");
        Check(InstalledDefaults.Parse(null).Error == null && !InstalledDefaults.Parse(null).Present, "no record at all");
    }

    // ---- the log key ----------------------------------------------------------------------------------------

    static void KeyChecks()
    {
        LabExpectations x = Expect40();
        OperatingPointView a = Point(), b = Point();
        Check(OperatingPointRules.StateKey(a, x) == OperatingPointRules.StateKey(b, x), "the same state is the same key");
        // A game drives the clock, the temperature and the thermal throttle all the time. None of them is a
        // state change, or the log would fill with normal thermal behaviour instead of real events.
        b.Dpm.Snapshot.CurrentMHz = 1300;
        b.Dpm.Snapshot.TargetMHz = 1300;
        b.Dpm.Snapshot.CapMHz = 1300;
        b.Dpm.Snapshot.Throttle = 8;
        b.Dpm.Snapshot.TemperatureMc = 86500;
        b.Dpm.Snapshot.BusyPermille = 980;
        b.Health.Snapshot.Completed = 999999;
        b.Dpm.Utc = Now.AddSeconds(-1);
        Check(OperatingPointRules.StateKey(a, x) == OperatingPointRules.StateKey(b, x),
              "clock, temperature, throttle and completions are not a state change");
        b.Cu.Escape = Escape(40, 24, 24, CuModeSnapshot.FlagValid, 3);
        Check(OperatingPointRules.StateKey(a, x) != OperatingPointRules.StateKey(b, x), "a CU change is a state change");
        OperatingPointView c = Point();
        c.Health.Snapshot.Flags = 7;
        Check(OperatingPointRules.StateKey(a, x) != OperatingPointRules.StateKey(c, x), "a health flag change is one too");
        // A device beginning or ending its use of the interop path is normal traffic, not a state change; the
        // start-latched flags are.
        OperatingPointView d = Point();
        d.Interop.Escape.Users = 4;
        d.Interop.Escape.Flags &= ~InteropSnapshot.FlagSession;
        Check(OperatingPointRules.StateKey(a, x) == OperatingPointRules.StateKey(d, x),
              "the session marker and the device count are not a state change");
        d.Interop.Escape.Flags |= InteropSnapshot.FlagUnclean;
        Check(OperatingPointRules.StateKey(a, x) != OperatingPointRules.StateKey(d, x), "an unclean start is");
        OperatingPointView e = Point();
        e.Cu.Escape = Escape(40, 40, 38, GoodCuFlags, 0);
        Check(OperatingPointRules.StateKey(a, x) != OperatingPointRules.StateKey(e, x),
              "a change in the counted units is a state change even at the same mode");
        Check(OperatingPointRules.StateKey(a, x) != OperatingPointRules.StateKey(a, new LabExpectations { CuMode = 24 }),
              "a changed expectation is a state change: the rules in force belong in the log");
        Check(OperatingPointRules.StateKey(a, x).Contains("cu=40/40"), "the key is readable: " + OperatingPointRules.StateKey(a, x));
        Check(OperatingPointRules.Worst(OperatingPointRules.Rows(a, x)) == Level.Good, "a healthy lab logs as good");
        OperatingPointView bad = Point();
        bad.Interop = InteropEscaped(0, 3, InteropSnapshot.FlagValid, 0, 0);
        Check(OperatingPointRules.Worst(OperatingPointRules.Rows(bad, x)) == Level.Error, "a red row sets the log level");

        MeasurementGuardView g = Guard(), h = Guard();
        Check(MeasurementGuardRules.StateKey(g, x) == MeasurementGuardRules.StateKey(h, x), "guard key is stable");
        h.NowUtc = Now.AddHours(5);
        Check(MeasurementGuardRules.StateKey(g, x) == MeasurementGuardRules.StateKey(h, x), "an age is not a state change");
        h.Parameters.Values["DpmMaxMHz"] = 2000;
        Check(MeasurementGuardRules.StateKey(g, x) != MeasurementGuardRules.StateKey(h, x), "a parameter change is");
        MeasurementGuardView i = Guard();
        i.Markers[1] = new LabMarker("graphics-summary.pause", true);
        Check(MeasurementGuardRules.StateKey(g, x) != MeasurementGuardRules.StateKey(i, x), "a marker change is");
    }

    // ---- the DPM feed: one snapshot, two panels -------------------------------------------------------------

    static void FeedChecks(string dir)
    {
        var clock = new DateTime(2026, 10, 5, 12, 0, 0, DateTimeKind.Utc);
        var feed = new DpmFeed(() => clock);
        Check(!feed.Read().Have && feed.Read().Error == null, "an empty feed claims nothing");
        var source = new FakeDpmSource { Dpm = new DpmSnapshot { Version = 0x000700CD, Flags = 1, MaxMHz = 1500 } };
        var provider = new TelemetryProvider(source, feed);
        var state = new State(dir);
        provider.Poll(state);
        DpmView view = feed.Read();
        Check(view.Have && view.Snapshot.MaxMHz == 1500 && view.Utc == clock, "the telemetry sample reaches the feed");
        // A failure must not erase the last good snapshot: the panel shows it with its age instead.
        source.Dpm = new InvalidOperationException("KMD DPM snapshot unavailable (0xC00000A3)");
        provider.Poll(state);
        view = feed.Read();
        Check(view.Have && view.Snapshot.MaxMHz == 1500 && view.Error.Contains("0xC00000A3"),
              "a failure keeps the last snapshot and records the reason");
        // A stopped governor is published too: that is exactly what the operating-point panel has to report.
        source.Dpm = new DpmSnapshot { Version = 0x000700CD, Flags = 0, MaxMHz = 1500, Mode = 1 };
        provider.Poll(state);
        Check(feed.Read().Snapshot.Flags == 0 && feed.Read().Error == null, "a not-running snapshot is published");
        Check(LevelOf(DpmRows(feed.Read()), "DPM") == Level.Error, "and the panel calls it out");
        Check(new TelemetryProvider(source).Period == TelemetryProvider.SamplePeriod, "a provider without a feed still works");
    }

    // ---- the providers themselves, on this computer ----------------------------------------------------------

    static void ProviderChecks(string dir)
    {
        var state = new State(dir);
        var source = new FakeLabSource
        {
            CuMode = Escape(40, 40, 40, GoodCuFlags, 0),
            Health = new StartHealthSnapshot { Magic = 0x30353242, Command = 21, AbiVersion = 1, Flags = 15, Generation = 3, Epoch = 1 },
            Interop = InteropEscape(3, 3, InteropSnapshot.FlagValid | InteropSnapshot.FlagSession, 1, 0),
        };
        var feed = new DpmFeed();
        feed.Publish(new DpmSnapshot { Version = 0x000700CD, Mode = 1, MaxMHz = 1500, CapMHz = 1500, Flags = 11 });
        var point = new OperatingPointProvider(source, feed, dir);
        Check(point.Name == "operating" && point.Period == TimeSpan.FromSeconds(10), "the operating-point poll is 10 s");
        point.Poll(state);
        Panel panel = state.Take(0).Panels.First(p => p.Name == "operating");
        Check(panel.Title == "Operating point" && panel.Order == 14, "the panel keeps its place in the column");
        Check(source.CuCalls == 1 && source.HealthCalls == 1 && source.InteropCalls == 1,
              "one escape of each per poll");
        Check(LevelOf(panel.Rows, "Interop") == Level.Good &&
              Value(panel.Rows, "Interop").Contains("session live"),
              "the provider's interop row reads the escape: " + Value(panel.Rows, "Interop"));
        Check(Value(panel.Rows, "CU").StartsWith("40 CU"), "the provider's CU row: " + Value(panel.Rows, "CU"));
        Check(Value(panel.Rows, "Sampled").EndsWith("(10 s poll)"), "the sample time is on the panel");
        int logged = state.Take(State.LogCapacity).Log.Count(l => l.Source == "operating");
        point.Poll(state);
        Check(source.CuCalls == 2 && state.Take(State.LogCapacity).Log.Count(l => l.Source == "operating") == logged,
              "an unchanged state is logged once, not every poll");

        // The pipeline panel's own pause marker stops this panel's escapes as well.
        string marker = Path.Combine(dir, GraphicsPipelineProvider.SummaryPauseFileName);
        File.WriteAllText(marker, "");
        point.Poll(state);
        Check(source.CuCalls == 2 && source.HealthCalls == 2 && source.InteropCalls == 2,
              "graphics-summary.pause stops all three escapes");
        panel = state.Take(0).Panels.First(p => p.Name == "operating");
        Check(Value(panel.Rows, "Sampled").StartsWith("paused; snapshot ") && LevelOf(panel.Rows, "Sampled") == Level.Warn,
              "a paused panel never looks like a fresh one: " + Value(panel.Rows, "Sampled"));
        Check(Value(panel.Rows, "CU").StartsWith("40 CU"), "the last snapshot is kept while paused");
        File.Delete(marker);

        // No control DLL on this computer: the provider says so and the rest of the panel still works.
        source.CuMode = new EntryPointNotFoundException("Bc250CuMode");
        source.Health = new EntryPointNotFoundException("Bc250StartHealth");
        source.Interop = new EntryPointNotFoundException("Bc250Interop");
        point.Poll(state);
        panel = state.Take(0).Panels.First(p => p.Name == "operating");
        Check(Value(panel.Rows, "CU").Contains("control DLL too old for CU mode") ||
              Value(panel.Rows, "CU snapshot") == "control DLL too old for CU mode",
              "a missing export is a row, not a dead provider: " + Value(panel.Rows, "CU"));
        Check(Value(panel.Rows, "Start health") == "control DLL too old for start health", "and the same for start health");
        Check(Find(panel.Rows, "DPM") != null && Find(panel.Rows, "Interop") != null, "the other rows are unaffected");
        Check(Value(panel.Rows, "Interop").Contains("interop snapshot") ||
              Value(panel.Rows, "Interop").Contains("registry mirror"),
              "the interop row falls back to the mirror and says so: " + Value(panel.Rows, "Interop"));

        // A broken expectations file is named on the panel, and the default expectation still applies.
        File.WriteAllText(Path.Combine(dir, ExpectationsFile.FileName), "{oops");
        point.Poll(state);
        panel = state.Take(0).Panels.First(p => p.Name == "operating");
        Check(Find(panel.Rows, "Expectations") != null && LevelOf(panel.Rows, "Expectations") == Level.Warn,
              "a broken expectations file is reported on the panel");
        File.Delete(Path.Combine(dir, ExpectationsFile.FileName));

        var guard = new MeasurementGuardProvider(dir, feed);
        Check(guard.Name == "guard" && guard.Period == TimeSpan.FromSeconds(10), "the guard poll is 10 s");
        guard.Poll(state);
        panel = state.Take(0).Panels.First(p => p.Name == "guard");
        Check(panel.Title == "Measurement guard" && panel.Order == 16, "the guard panel's place");
        Check(Find(panel.Rows, "Parameters") != null && Find(panel.Rows, "Markers") != null &&
              Find(panel.Rows, "Sampled") != null, "the guard panel has its rows on a computer without the driver");
        Check(Value(panel.Rows, "Markers") == "none set", "no marker in a fresh data directory");
        File.WriteAllText(Path.Combine(dir, GraphicsApiProvider.PauseFileName), "");
        guard.Poll(state);
        panel = state.Take(0).Panels.First(p => p.Name == "guard");
        Check(Value(panel.Rows, "Markers").Contains(GraphicsApiProvider.PauseFileName),
              "the guard sees a marker appear: " + Value(panel.Rows, "Markers"));
        File.Delete(Path.Combine(dir, GraphicsApiProvider.PauseFileName));

        // The file cache: one read per write, however often it is polled.
        string file = Path.Combine(dir, "cache-probe.txt");
        var cache = new FileCache(file);
        cache.Refresh();
        Check(!cache.Present && cache.Text == null && !cache.Changed, "an absent file reads as absent");
        File.WriteAllText(file, " value ");
        cache.Refresh();
        Check(cache.Present && cache.Text == "value" && cache.Changed, "a new file is read and trimmed");
        cache.Refresh();
        Check(!cache.Changed && cache.Text == "value", "an unchanged file is not read again");
        File.SetLastWriteTimeUtc(file, DateTime.UtcNow.AddSeconds(5));
        cache.Refresh();
        Check(cache.Changed, "a new write time re-reads the file");
        var limited = new FileCache(file);
        limited.Refresh(2);
        Check(limited.Present && limited.Text == "", "a file larger than the limit is not read into memory");
        File.Delete(file);
        cache.Refresh();
        Check(!cache.Present && cache.Text == null, "a deleted file clears the cache");
    }

    // ---- cost -----------------------------------------------------------------------------------------------

    static void CostCheck()
    {
        LabExpectations x = Expect40();
        OperatingPointView point = Point();
        MeasurementGuardView guard = Guard();
        const int rounds = 200000;
        var cpu = Process.GetCurrentProcess();
        TimeSpan before = cpu.TotalProcessorTime;
        for (int i = 0; i < rounds; i++)
        {
            OperatingPointRules.Rows(point, x);
            OperatingPointRules.StateKey(point, x);
            MeasurementGuardRules.Rows(guard, x);
            MeasurementGuardRules.StateKey(guard, x);
        }
        cpu.Refresh();
        double perPollUs = (cpu.TotalProcessorTime - before).TotalMilliseconds * 1000 / rounds;
        Console.WriteLine(string.Format(CultureInfo.InvariantCulture,
            "managed cost: {0:0.0} us per 10 s poll of both panels = {1:0.00000} % of one core",
            perPollUs, perPollUs / 10 / 1e4));
        Check(perPollUs < 500, "the rules of both panels cost under 500 us per poll");
    }
}
