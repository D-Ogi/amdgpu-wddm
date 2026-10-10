// Host tests of the control application's pure parts: KMD reply parsing (with offsets computed from the header text
// of driver/kmd/bc250kmd_escape.h, so a moved field fails here and not on a tester's PC), profile editing, DPM setting
// checks, redaction, the manifest, and the Recovery rules and plans (every refusal included). No driver, no registry.
// Usage: unit-tests.exe <repository root> [<start-confirm-core.ps1 of the release installer>]
// With the second argument the confirmation rule is also compared with the installer's own (Test-StartConfirmEligible).
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Web.Script.Serialization;
using AmdgpuWddmControl;

static partial class UnitTests
{
    static int _failed, _passed;

    static void Check(bool ok, string what)
    {
        if (ok) _passed++; else { _failed++; Console.WriteLine("FAIL " + what); }
    }

    static void Equal<T>(T expected, T actual, string what)
    {
        Check(EqualityComparer<T>.Default.Equals(expected, actual), what + ": expected <" + expected + "> got <" + actual + ">");
    }

    static void Throws<E>(Action a, string what) where E : Exception
    {
        try { a(); Check(false, what + ": no exception"); }
        catch (E) { Check(true, what); }
    }

    // Offsets of a struct of 4- and 8-byte scalars and arrays of them, from the C text (natural alignment, as MSVC
    // x64 lays it out). Another type ends the walk: the caller only asks for fields before it.
    static Dictionary<string, int> Layout(string header, string name, out int size)
    {
        var m = Regex.Match(header, @"typedef struct _" + name + @"\s*\{(?<body>.*?)\}\s*" + name + ";", RegexOptions.Singleline);
        if (!m.Success) throw new Exception("struct " + name + " not found in the header");
        var body = Regex.Replace(m.Groups["body"].Value, @"//[^\n]*", "");
        // An array length written as one of the header's own defines (BC250_DPM_CURVE_POINTS, BC250_CPU_CORE_SLOTS):
        // resolved from the same header, so the walk does not stop at the first such field.
        foreach (Match d in Regex.Matches(header, @"#define (?<id>BC250_\w+) (?<n>\d+)u?\b"))
            body = body.Replace("[" + d.Groups["id"].Value + "]", "[" + d.Groups["n"].Value + "]");
        var offsets = new Dictionary<string, int>();
        int at = 0;
        size = -1;
        foreach (var decl in body.Split(';').Select(d => Regex.Replace(d.Trim(), @"\s+", " ")).Where(d => d.Length > 0))
        {
            var t = Regex.Match(decl, @"^(?<type>unsigned long long|unsigned long|long long|long|unsigned char|char) (?<names>.+)$");
            if (!t.Success) return offsets;
            string type = t.Groups["type"].Value;
            int width = type.EndsWith("long long") ? 8 : type.EndsWith("char") ? 1 : 4;
            foreach (var raw in t.Groups["names"].Value.Split(','))
            {
                var n = Regex.Match(raw.Trim(), @"^(?<id>\w+)(\[(?<count>\w+)\])?$");
                if (!n.Success) return offsets;
                int count = 1;
                if (n.Groups["count"].Success)
                {
                    string c = n.Groups["count"].Value;
                    // An array length may be a #define of the same header, as BC250_ESCAPE_HWMON's slots are.
                    if (!int.TryParse(c, out count))
                    {
                        var d = Regex.Match(header, @"#define " + c + @"\s+(\d+)u?\b");
                        if (!d.Success) throw new Exception("array length " + c + " is not a #define of the header");
                        count = int.Parse(d.Groups[1].Value);
                    }
                }
                at = (at + width - 1) / width * width;
                offsets[n.Groups["id"].Value] = at;
                at += width * count;
            }
        }
        size = (at + 7) / 8 * 8;
        return offsets;
    }

    static void Put(byte[] b, int offset, uint v) { BitConverter.GetBytes(v).CopyTo(b, offset); }
    static void Put(byte[] b, int offset, ulong v) { BitConverter.GetBytes(v).CopyTo(b, offset); }

    static void Replies(string header)
    {
        int size;
        var dpm = Layout(header, "BC250_ESCAPE_DPM", out size);
        // The reply this app parses is RUN_DPM ABI 1, whose length the header states as BC250_DPM_ABI1_SIZE.
        // The structure itself is longer since 0.7.207, which appends the idle state as ABI 2; IdleMHz is the
        // first of those fields, so its offset is the ABI 1 length and the assertion holds for both revisions.
        Equal(KmdReply.DpmBytes, Regex.Match(header, @"#define BC250_DPM_ABI1_SIZE (\d+)u").Success
            ? int.Parse(Regex.Match(header, @"#define BC250_DPM_ABI1_SIZE (\d+)u").Groups[1].Value) : size,
            "DPM ABI 1 length from the header");
        Equal(KmdReply.DpmBytes, dpm.ContainsKey("IdleMHz") ? dpm["IdleMHz"] : size, "DPM ABI 1 fields fit it");
        var b = new byte[KmdReply.DpmBytes];
        Put(b, dpm["Magic"], KmdReply.Magic); Put(b, dpm["Command"], 23u); Put(b, dpm["AbiVersion"], 1u);
        Put(b, dpm["Version"], 0x000700C5u); Put(b, dpm["Flags"], 128u | 256u | 512u); Put(b, dpm["Mode"], 1u);
        Put(b, dpm["Reason"], 0u); Put(b, dpm["Throttle"], 1u); Put(b, dpm["MaxMHz"], 2000u); Put(b, dpm["CurrentMHz"], 1800u);
        Put(b, dpm["CurrentMv"], 950u); Put(b, dpm["ObservedMHz"], 1795u); Put(b, dpm["TemperatureMc"], unchecked((uint)87250));
        Put(b, dpm["BusyAvgPermille"], 734u); Put(b, dpm["ThermalEvents"], 3u); Put(b, dpm["Errors"], 0u);
        Put(b, dpm["UptimeMs"], 123456UL); Put(b, dpm["Generation"], 42UL);
        var d = KmdReply.ParseDpm(b);
        Equal("0.7.197", KmdReply.VersionText(d.Version), "DPM version");
        Equal(1u, d.Mode, "DPM mode"); Equal(1u, d.Throttle, "DPM throttle"); Equal(2000u, d.MaxMHz, "DPM max");
        Equal(950u, d.CurrentMv, "DPM mV"); Equal("1795 MHz", KmdReply.ClockText(d), "DPM observed clock wins");
        Equal("87.3 C", KmdReply.TemperatureText(d), "DPM temperature"); Equal(734u, d.BusyAvgPermille, "DPM busy");
        Equal(3u, d.ThermalEvents, "DPM thermal events"); Equal(123456UL, d.UptimeMs, "DPM uptime"); Equal(42UL, d.Generation, "DPM generation");
        Put(b, dpm["Flags"], 0u);
        d = KmdReply.ParseDpm(b);
        Equal("not available", KmdReply.TemperatureText(d), "DPM temperature without its flag");
        Equal("1800 MHz", KmdReply.ClockText(d), "DPM clock without readback");
        Put(b, dpm["AbiVersion"], 2u);
        Throws<FormatException>(() => KmdReply.ParseDpm(b), "DPM ABI 2 refused");
        Throws<FormatException>(() => KmdReply.ParseDpm(new byte[159]), "DPM short reply refused");
        Put(b, dpm["AbiVersion"], 1u); Put(b, dpm["Command"], 21u);
        Throws<FormatException>(() => KmdReply.ParseDpm(b), "DPM wrong command refused");
        Put(b, dpm["Command"], 23u); Put(b, dpm["Flags"], 128u | 8192u);
        Equal(0u, KmdReply.ParseDpm(b).Flags & DpmState.FlagPower, "DPM ABI 1: the power bit is dropped (it means something only in ABI 3)");

        // RUN_DPM ABI 3 (0.7.215): BC250_ESCAPE_DPM_EX, the ABI 2 structure followed by BC250_DPM_METRICS. Both lengths
        // and every offset come from the header text.
        int exSize = int.Parse(Regex.Match(header, @"#define BC250_DPM_ABI3_SIZE (\d+)u").Groups[1].Value);
        int prefix = int.Parse(Regex.Match(header, @"#define BC250_DPM_ABI2_SIZE (\d+)u").Groups[1].Value);
        Equal(KmdReply.DpmAbi3Bytes, exSize, "DPM ABI 3 length from the header");
        Equal(KmdReply.DpmMetricsOffset, prefix, "DPM ABI 3 metrics offset from the header");
        Equal(prefix, size, "DPM ABI 2 structure is the ABI 3 prefix");
        int metricsSize;
        var mt = Layout(header, "BC250_DPM_METRICS", out metricsSize);
        Equal(exSize - prefix, metricsSize, "DPM metrics size from the header");
        Equal(8192u, uint.Parse(Regex.Match(header, @"#define BC250_DPM_FLAG_POWER (\d+)u").Groups[1].Value), "DPM power flag from the header");
        Equal(2u, uint.Parse(Regex.Match(header, @"#define BC250_DPM_METRICS_OK (\d+)u").Groups[1].Value), "DPM metrics OK from the header");
        var x = new byte[KmdReply.DpmAbi3Bytes];
        Put(x, dpm["Magic"], KmdReply.Magic); Put(x, dpm["Command"], 23u); Put(x, dpm["AbiVersion"], 3u);
        Put(x, dpm["Version"], 0x000700D7u); Put(x, dpm["Flags"], 128u | 256u | 512u | 8192u); Put(x, dpm["ObservedMHz"], 1500u);
        Put(x, dpm["TemperatureMc"], 62000u); Put(x, dpm["BusyAvgPermille"], 370u); Put(x, dpm["IdleMHz"], 500u);
        Put(x, prefix + mt["MetricsState"], 2u); Put(x, prefix + mt["MetricsAgeMs"], 640u);
        Put(x, prefix + mt["SocketPowerMw"], 78400u); Put(x, prefix + mt["SocketPowerAvgMw"], 77100u);
        Put(x, prefix + mt["GfxPowerMw"], 48000u); Put(x, prefix + mt["SocPowerMw"], 21000u);
        Put(x, prefix + mt["GfxMv"], 919u); Put(x, prefix + mt["SocMv"], 900u);
        d = KmdReply.ParseDpm(x);
        Equal(3u, d.AbiVersion, "DPM ABI 3 version");
        Check(d.Has(DpmState.FlagPower), "DPM ABI 3 power flag");
        Equal(78400u, d.SocketPowerMw, "DPM ABI 3 socket power"); Equal(77100u, d.SocketPowerAvgMw, "DPM ABI 3 average power");
        Equal(48000u, d.GfxPowerMw, "DPM ABI 3 gfx power"); Equal(21000u, d.SocPowerMw, "DPM ABI 3 soc power");
        Equal(919u, d.GfxMv, "DPM ABI 3 gfx mV"); Equal(900u, d.SocMv, "DPM ABI 3 soc mV");
        Equal(DpmState.MetricsOk, d.MetricsState, "DPM ABI 3 metrics state"); Equal(640u, d.MetricsAgeMs, "DPM ABI 3 metrics age");
        Equal(1500u, d.ObservedMHz, "DPM ABI 3 keeps the ABI 1 fields"); Equal(62000, d.TemperatureMc, "DPM ABI 3 temperature");
        Put(x, dpm["AbiVersion"], 2u);
        Throws<FormatException>(() => KmdReply.ParseDpm(x), "DPM 248 bytes with ABI 2 refused");
        Put(x, dpm["AbiVersion"], 1u);
        Throws<FormatException>(() => KmdReply.ParseDpm(x), "DPM 248 bytes with ABI 1 refused");
        Put(b, dpm["AbiVersion"], 3u);
        Throws<FormatException>(() => KmdReply.ParseDpm(b), "DPM 160 bytes with ABI 3 refused");
        Throws<FormatException>(() => KmdReply.ParseDpm(new byte[192]), "DPM ABI 2 length is not a reply this app asks for");

        var io = Layout(header, "BC250_ESCAPE_INTEROP", out size);
        Equal(KmdReply.InteropBytes, size, "interop size from the header");
        b = new byte[KmdReply.InteropBytes];
        Put(b, io["Magic"], KmdReply.Magic); Put(b, io["Command"], 25u); Put(b, io["AbiVersion"], 1u);
        Put(b, io["Flags"], 1u | 2u); Put(b, io["Effective"], 3u); Put(b, io["Users"], 2u);
        Put(b, io["ClosedReason"], 4u); Put(b, io["LastEnd"], 2u); Put(b, io["SessionBootId"], 9u); Put(b, io["Generation"], 77UL);
        var i = KmdReply.ParseInterop(b);
        Equal("GPU (2 devices on the GPU path)", KmdReply.CompositionText(i), "interop in use");
        Equal(4u, i.ClosedReason, "interop closed reason"); Equal(2u, i.LastEnd, "interop last end");
        Equal(9u, i.SessionBootId, "interop session boot"); Equal(77UL, i.Generation, "interop generation");
        Put(b, io["Effective"], 0u); Put(b, io["Flags"], 1u | 16u);
        Check(KmdReply.CompositionText(KmdReply.ParseInterop(b)).StartsWith("CPU (the driver closed"), "interop closed by the driver");
        Put(b, io["Flags"], 0u);
        Check(KmdReply.CompositionText(KmdReply.ParseInterop(b)).StartsWith("Not decided"), "interop not decided");

        // The fan reply (BC250_ESCAPE_RUN_HWMON). KmdReply.ParseHwmon reads it by word index, so every offset this
        // app uses comes from the header here: a field inserted upstream shows a wrong fan speed, not an error.
        var fan = Layout(header, "BC250_ESCAPE_HWMON", out size);
        Equal(KmdReply.HwmonBytes, size, "fan size from the header");
        b = new byte[KmdReply.HwmonBytes];
        Put(b, fan["Magic"], KmdReply.Magic); Put(b, fan["Command"], 27u); Put(b, fan["AbiVersion"], 1u);
        Put(b, fan["Version"], 0x000700D0u);
        Put(b, fan["Flags"], HwmonState.FlagValid | HwmonState.FlagFresh | HwmonState.FlagMonitoring | HwmonState.FlagDutyProven);
        Put(b, fan["BasePort"], 0x0A20u); Put(b, fan["CustomerId"], 0x0E2Cu); Put(b, fan["EcVersion"], 0x0100u);
        Put(b, fan["EcBuild"], (21u << 16) | (7u << 8) | 28u);
        Put(b, fan["FanPresentMask"], 0x1Fu); Put(b, fan["DutyPresentMask"], 0x1Fu); Put(b, fan["ModeMask"], 0u);
        Put(b, fan["Rpm"] + 4, 1589u);                      // channel 1: the one fan unit A has
        Put(b, fan["DutyPermille"] + 4, 961u);              // 245 of 255
        Put(b, fan["TemperatureMc"], unchecked((uint)83000)); Put(b, fan["TemperatureSource"], 0x46u);
        Put(b, fan["TemperatureMc"] + 4, unchecked((uint)59500)); Put(b, fan["TemperatureSource"] + 4, 0x08u);
        Put(b, fan["AgeMs"], 420u); Put(b, fan["Samples"], 3600UL); Put(b, fan["Errors"], 2UL);
        Put(b, fan["Retries"], 5UL); Put(b, fan["Generation"], 77UL); Put(b, fan["Reason"], 0u);
        var f = KmdReply.ParseHwmon(b);
        Check(f.Reading, "fan: valid and fresh is a reading");
        Equal(1589u, f.FastestRpm, "fan: the fastest channel that turns");
        Equal(96u, f.DutyPercent, "fan: 961 permille is 96 %");
        Equal(83000, f.ApuMc.Value, "fan: the APU over the chip's own SB-TSI link");
        Equal(0x0A20u, f.BasePort, "fan: base port"); Equal(0x0E2Cu, f.CustomerId, "fan: customer id");
        Equal(0x0100u, f.EcVersion, "fan: EC version"); Equal(420u, f.AgeMs, "fan: age");
        Equal(3600UL, f.Samples, "fan: samples"); Equal(2UL, f.Errors, "fan: errors");
        Equal(5UL, f.Retries, "fan: retries"); Equal(77UL, f.Generation, "fan: generation");
        Check(f.Has(HwmonState.FlagDutyProven) && !f.Has(HwmonState.FlagStopped), "fan: flags");
        Put(b, fan["Flags"], HwmonState.FlagValid | HwmonState.FlagDutyProven);
        Check(!KmdReply.ParseHwmon(b).Reading, "fan: a stale sample is not a reading");
        Put(b, fan["Flags"], HwmonState.FlagGated); Put(b, fan["Reason"], 1u);
        Check(!KmdReply.ParseHwmon(b).Reading && KmdReply.ParseHwmon(b).Reason == 1, "fan: the closed gate says why");
        Put(b, fan["AbiVersion"], 2u);
        Throws<FormatException>(() => KmdReply.ParseHwmon(b), "fan ABI 2 refused");
        Put(b, fan["AbiVersion"], 1u); Put(b, fan["Command"], 23u);
        Throws<FormatException>(() => KmdReply.ParseHwmon(b), "fan wrong command refused");
        Throws<FormatException>(() => KmdReply.ParseHwmon(new byte[KmdReply.HwmonBytes - 1]), "fan short reply refused");

        var sh = Layout(header, "BC250_ESCAPE_START_HEALTH", out size);
        Equal(KmdReply.StartHealthBytes, size, "start health size from the header");
        b = new byte[KmdReply.StartHealthBytes];
        Put(b, sh["Magic"], KmdReply.Magic); Put(b, sh["Command"], 21u); Put(b, sh["AbiVersion"], 1u); Put(b, sh["Flags"], 15u);
        Put(b, sh["Generation"], 7UL); Put(b, sh["Epoch"], 3UL); Put(b, sh["Completed"], 99UL);
        Put(b, sh["LastCompletionAgeMs"], 1200UL); Put(b, sh["ReadyAgeMs"], 61000UL);
        var h = KmdReply.ParseStartHealth(b);
        Equal(7UL, h.Generation, "health generation"); Equal(99UL, h.Completed, "health completed"); Equal(61000UL, h.ReadyAgeMs, "health ready age");
        Equal(3UL, h.Epoch, "health epoch"); Equal(1200UL, h.LastCompletionAgeMs, "health completion age");

        var log = Layout(header, "BC250_ESCAPE_LOG", out size);   // stops at Lines[]: a struct type
        Equal(KmdReply.LogHeadBytes, log["SummaryFrom"] + 4, "log head size from the header");
        Check(Regex.IsMatch(header, @"sizeof\(BC250_ESCAPE_LOG\) == " + KmdReply.LogBytes + @"\)"), "log size check in the header is " + KmdReply.LogBytes);
        Check(Regex.IsMatch(header, @"#define BC250_LOG_TEXT " + KmdReply.LogTextBytes + @"\b"), "log text width");
        Check(Regex.IsMatch(header, @"#define BC250_LOG_MAX_LINES " + KmdReply.LogMaxLines + @"\b"), "log lines per page");
        b = new byte[KmdReply.LogBytes];
        Put(b, log["Magic"], KmdReply.Magic); Put(b, log["Command"], 12u); Put(b, log["Total"], 300u); Put(b, log["Returned"], 2u); Put(b, log["Next"], 66u);
        Put(b, KmdReply.LogHeadBytes, 64u); Put(b, KmdReply.LogHeadBytes + 4, 1234u);
        Encoding.ASCII.GetBytes("dpm: start mode 1").CopyTo(b, KmdReply.LogHeadBytes + 8);
        Put(b, KmdReply.LogHeadBytes + KmdReply.LogLineBytes, 65u);
        var full = new string('x', KmdReply.LogTextBytes);       // no terminator: the parser stops at the width
        Encoding.ASCII.GetBytes(full).CopyTo(b, KmdReply.LogHeadBytes + KmdReply.LogLineBytes + 8);
        var page = KmdReply.ParseLog(b);
        Equal(2, page.Lines.Count, "log lines"); Equal("dpm: start mode 1", page.Lines[0].Text, "log text");
        Equal(1234u, page.Lines[0].Milliseconds, "log time"); Equal(65u, page.Lines[1].Sequence, "log sequence");
        Equal(KmdReply.LogTextBytes, page.Lines[1].Text.Length, "unterminated log line bounded");
        Put(b, log["Returned"], 65u);
        Throws<FormatException>(() => KmdReply.ParseLog(b), "log page over 64 lines refused");

        b = new byte[KmdReply.VideoMemoryBytes];
        Put(b, 0, (uint)KmdReply.VideoMemoryBytes); Put(b, 4, 3u); Put(b, 24, 1UL << 30); Put(b, 64, 8UL << 30);
        var v = KmdReply.ParseVideoMemory(b);
        Equal(1UL << 30, v.LocalResident, "VRAM resident"); Equal(8UL << 30, v.Dedicated, "VRAM dedicated");
        Equal("8.00 GiB", KmdReply.Bytes(v.Dedicated), "VRAM text"); Equal("no limit", KmdReply.Bytes(ulong.MaxValue), "no limit text");

        Equal("The BC-250 driver is not loaded", KmdReply.StatusText(unchecked((int)0xC000000E)), "missing driver text");
        Check(KmdReply.ReasonText(4).Contains("fixed clock"), "reason UNCLEAN text");
        Check(KmdReply.ThrottleText(2).Contains("90 C"), "throttle hard text");
    }

    static void ShellTokens(string root)
    {
        // Every catalog switch is a name the shell asks for; every name the shell asks for is in the catalog.
        // ddi_experiment_off("x") asks for "x-off" (driver/umd/d3d12/ddi-trace.h): the default is on and the
        // switch subtracts from it, so the name in the value, and in the catalog, is the off form.
        var asked = new HashSet<string>();
        foreach (var file in Directory.GetFiles(Path.Combine(root, @"driver\umd\d3d12"), "*.cpp").Where(f => !f.EndsWith("-test.cpp")))
            foreach (Match m in Regex.Matches(File.ReadAllText(file), "ddi_experiment(_off)?\\(\"([a-z0-9-]+)\"\\)"))
                asked.Add(m.Groups[2].Value + (m.Groups[1].Success ? "-off" : ""));
        foreach (var s in Profiles.Catalog)
        {
            // A switch of a shell branch that is not on main yet may be missing; once the shell reads it, the entry
            // in Profiles.AwaitingShell has to go.
            if (Profiles.AwaitingShell.Contains(s.Token)) Check(!asked.Contains(s.Token), "catalog switch " + s.Token + " is read by the shell now: remove it from Profiles.AwaitingShell");
            else Check(asked.Contains(s.Token), "catalog switch " + s.Token + " is read by the shell");
        }
        foreach (var t in Profiles.AwaitingShell) Check(Profiles.Find(t) != null, "awaiting switch " + t + " is in the catalog");
        foreach (var t in asked) Check(Profiles.Find(t) != null, "shell switch " + t + " is in the catalog");
        var trace = File.ReadAllText(Path.Combine(root, @"driver\umd\d3d12\ddi-trace.h"));
        Check(trace.Contains(@"SOFTWARE\\amdgpu-wddm\\D3D12\\Applications\\"), "profile key path matches the shell");
        Check(trace.Contains("\"Experiment\""), "profile value name matches the shell");
    }

    static void ProfileEditing()
    {
        var p = Profiles.Parse("present-noprimary,present-cached,raytracing-tier-off,recording-bind-off,retire-handoff-off,deferred-replay-off");
        Equal(6, p.Known.Count, "witcher3 profile known"); Equal(0, p.Unknown.Count, "witcher3 profile unknown");
        Equal("raytracing-tier-off", p.Known[0], "catalog order");
        // The positive names of the defaults are accepted by the shell and do nothing, so they are unknown here.
        p = Profiles.Parse("raytracing-tier,recording-bind,retire-handoff,deferred-replay");
        Equal(0, p.Known.Count, "the positive names of the defaults are not catalog switches");
        Equal(4, p.Unknown.Count, "the positive names are kept as unknown names");
        p = Profiles.Parse("future-switch,raytracing-tier-off,raytracing-tier-off,,none");
        Equal(1, p.Known.Count, "duplicates, empty and none dropped"); Equal("future-switch", p.Unknown.Single(), "unknown kept");
        Equal("raytracing-tier-off,deferred-replay-off,future-switch", Profiles.Compose(new[] { "deferred-replay-off", "raytracing-tier-off" }, p.Unknown), "compose order");
        Equal("", Profiles.Compose(new string[0], new string[0]), "empty profile");
        p = Profiles.Parse("Present-Cached;x");
        Equal(1, p.Unknown.Count, "value outside the syntax is one unknown entry"); Equal(0, p.Known.Count, "nothing parsed from it");
        Throws<ArgumentException>(() => Profiles.Compose(new string[0], p.Unknown), "invalid unknown refused on save");
        Throws<ArgumentException>(() => Profiles.Compose(new[] { "not-a-switch" }, null), "non-catalog switch refused");
        Throws<ArgumentException>(() => Profiles.Compose(new string[0], new[] { new string('a', 256) }), "over 255 refused");
        Check(Profiles.IsValidImage("witcher3.exe"), "image witcher3.exe");
        Check(Profiles.IsValidImage("The Ascent.exe"), "image with a space");
        Check(Profiles.IsValidImage("ROTTR.EXE"), "image upper case");
        Check(!Profiles.IsValidImage(@"C:\Games\witcher3.exe"), "path refused");
        Check(!Profiles.IsValidImage("..\\x.exe"), "relative path refused");
        Check(!Profiles.IsValidImage("witcher3"), "no .exe refused");
        Check(!Profiles.IsValidImage(" witcher3.exe"), "leading space refused");
        Check(!Profiles.IsValidImage(""), "empty refused");
    }

    static void Dpm()
    {
        Check(DpmSettings.IsValidCeiling(1500) && DpmSettings.IsValidCeiling(2000) && DpmSettings.IsValidCeiling(1000), "ceilings in range");
        Check(!DpmSettings.IsValidCeiling(2100) && !DpmSettings.IsValidCeiling(1550) && !DpmSettings.IsValidCeiling(900), "ceilings out of range");
        Equal(2000u, DpmSettings.CeilingChoices.Max(), "hard ceiling");
        Check(DpmSettings.CeilingChoices.Contains(DpmSettings.DefaultMaxMHz), "default among the choices");
        Check(DpmSettings.IsValidMode(0) && DpmSettings.IsValidMode(1) && !DpmSettings.IsValidMode(2), "modes");
        Equal("Fixed (default), ceiling 1500 MHz (default)", DpmSettings.Describe(null, null), "describe defaults");
        Equal("Automatic, ceiling 2000 MHz", DpmSettings.Describe(1, 2000), "describe dpm 2000");
    }

    static void DpmDesignDoc(string root)
    {
        var doc = File.ReadAllText(Path.Combine(root, @"docs\design\dpm.md"));
        Check(doc.Contains("`DpmMode`") && doc.Contains("`DpmMaxMHz`"), "DPM setting names in docs/design/dpm.md");
        Check(doc.Contains(@"Services\bc250kmd\Parameters"), "DPM registry path in docs/design/dpm.md");
        Check(Regex.IsMatch(doc, @"absent = 1500"), "default ceiling 1500 in docs/design/dpm.md");
    }

    static void Redaction()
    {
        var r = new Redactor("tester", "BENCH-PC", @"C:\Users\tester");
        var input = string.Join("\n", new[]
        {
            @"Log: C:\Users\tester\AppData\Local\Temp\x.txt",
            "Machine name: BENCH-PC",
            "Machine Id: {01234567-89AB-CDEF-0123-456789ABCDEF}",
            "Owner tester ran it",
            "MAC 00-00-5E-00-53-01 and 00:00:5e:00:53:02",
            "Physical Address: 00005E005303",
            "System Serial Number: ABC123XYZ",
            "contact someone@example.org now",
            "Driver Version: 0.7.197.1 sha256 982DB3CF0011",
            @"Hardware ID: PCI\VEN_1002&DEV_13FE&SUBSYS_00000000",
            "testerx and xtester stay",
        });
        var o = r.Apply(input);
        Check(!o.Contains(@"Users\tester"), "profile path removed");
        Check(o.Contains(@"%USERPROFILE%\AppData"), "profile path replaced");
        Check(!o.Contains("BENCH-PC"), "computer name removed");
        Check(!o.Contains("0123-456789ABCDEF"), "machine id removed");
        Check(o.Contains("Owner <user> ran it"), "user name replaced");
        Check(!o.Contains("53-01") && !o.Contains("53:02") && !o.Contains("00005E005303"), "MAC addresses removed");
        Check(!o.Contains("ABC123XYZ") && o.Contains("System Serial Number: <redacted>"), "serial removed, label kept");
        Check(!o.Contains("someone@example.org"), "e-mail removed");
        Check(o.Contains("0.7.197.1") && o.Contains("982DB3CF0011"), "versions and hashes kept");
        Check(o.Contains(@"PCI\VEN_1002&DEV_13FE"), "hardware id kept");
        Check(o.Contains("testerx and xtester stay"), "user name only as a whole word");
        Equal("", r.Apply(null), "null text");
    }

    static void Manifest()
    {
        const string json = @"{""schema"": 1, ""release"": ""amdgpu-wddm tester"", ""version"": ""0.7.197.1-tester.0"", ""kmd_version"": ""0.7.197.1"",
            ""components"": [
              {""role"": ""d3d12-shell"", ""install_path"": ""<InstallDir>\\d3d12\\amdgpu_wddm_d3d12.dll"", ""version"": ""1"", ""sha256"": ""aa11""},
              {""role"": ""desktop-umd"", ""install_path"": ""%SystemRoot%\\System32\\bc250umd.dll"", ""version"": ""2"", ""sha256"": ""BB22""},
              {""role"": ""kmd"", ""install_path"": ""DriverStore (bc250kmd.inf)\\bc250kmd.sys"", ""version"": ""0.7.197.1"", ""sha256"": ""CC33""},
              {""role"": ""firmware"", ""install_path"": ""C:\\BC250\\firmware\\x.bin"", ""version"": """", ""sha256"": ""DD44""},
              {""role"": ""tool"", ""install_path"": ""somewhere\\relative.exe"", ""version"": """", ""sha256"": ""EE55""},
              {""role"": ""certificate"", ""install_path"": ""LocalMachine Root and TrustedPublisher"", ""version"": """", ""sha256"": ""C0FF""}
            ], ""files"": [], ""release_certificate"": ""ab12""}";
        var m = ManifestCheck.Parse(json);
        Equal("0.7.197.1-tester.0", m.Version, "manifest version"); Equal(6, m.Components.Count, "manifest components");
        Equal("ab12", m.ReleaseCertificate, "release certificate");
        Equal("AA11", m.Components[0].Sha256, "hash upper-cased");
        Func<string, string> expand = p => p.Replace("%SystemRoot%", @"C:\Windows");
        const string dir = @"C:\Program Files\amdgpu-wddm", kmd = @"C:\Windows\System32\DriverStore\FileRepository\bc250kmd.inf_amd64_x\bc250kmd.sys";
        Equal(@"C:\Program Files\amdgpu-wddm\d3d12\amdgpu_wddm_d3d12.dll", ManifestCheck.Resolve(m.Components[0].InstallPath, dir + "\\", kmd, expand), "InstallDir form");
        Equal(@"C:\Windows\System32\bc250umd.dll", ManifestCheck.Resolve(m.Components[1].InstallPath, dir, kmd, expand), "SystemRoot form");
        Equal(kmd, ManifestCheck.Resolve(m.Components[2].InstallPath, dir, kmd, expand), "DriverStore form");
        Equal(null, ManifestCheck.Resolve(m.Components[2].InstallPath, dir, "", expand), "DriverStore without a KMD image");
        Equal(null, ManifestCheck.Resolve(m.Components[4].InstallPath, dir, kmd, expand), "relative path unresolved");
        var files = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
        {
            { @"C:\Program Files\amdgpu-wddm\d3d12\amdgpu_wddm_d3d12.dll", "aa11" }, { @"C:\Windows\System32\bc250umd.dll", "FFFF" }, { kmd, "CC33" },
        };
        Check(ManifestCheck.CertificateStores("LocalMachine Root and TrustedPublisher").SequenceEqual(new[] { "Root", "TrustedPublisher" }), "certificate stores parsed");
        Equal(null, ManifestCheck.CertificateStores(@"C:\x.cer"), "a path is no store form");
        Equal(null, ManifestCheck.CertificateStores(@"LocalMachine ..\Root"), "a store name with separators refused");
        var other = new KeyValuePair<string, string>("FFFF", "EEEE");
        var bySha = new KeyValuePair<string, string>("1234", "c0ff");
        var byThumb = new KeyValuePair<string, string>("AB12", "9999");
        Func<Dictionary<string, KeyValuePair<string, string>[]>, Func<string, IEnumerable<KeyValuePair<string, string>>>> stores =
            d => s => d.ContainsKey(s) ? d[s] : new KeyValuePair<string, string>[0];
        var both = stores(new Dictionary<string, KeyValuePair<string, string>[]> { { "Root", new[] { other, bySha } }, { "TrustedPublisher", new[] { byThumb } } });
        var report = ManifestCheck.Report(m, dir, kmd, expand, files.ContainsKey, p => files[p], both);
        Check(Regex.IsMatch(report, @"(?m)^OK\s+d3d12-shell"), "matching file OK");
        Check(Regex.IsMatch(report, @"(?m)^MISMATCH\s+desktop-umd"), "changed file MISMATCH");
        Check(Regex.IsMatch(report, @"(?m)^OK\s+kmd"), "driver store file OK");
        Check(Regex.IsMatch(report, @"(?m)^MISSING\s+firmware"), "absent file MISSING");
        Check(Regex.IsMatch(report, @"(?m)^UNRESOLVED\s+tool"), "relative path UNRESOLVED");
        Check(Regex.IsMatch(report, @"(?m)^OK\s+certificate .*in every store"), "certificate in both stores OK (by SHA-256 or thumbprint)");
        Check(report.Contains("3 match, 3 differ, missing or unresolved"), "count line");
        var rootOnly = stores(new Dictionary<string, KeyValuePair<string, string>[]> { { "Root", new[] { bySha } } });
        report = ManifestCheck.Report(m, dir, kmd, expand, files.ContainsKey, p => files[p], rootOnly);
        Check(Regex.IsMatch(report, @"(?m)^MISSING\s+certificate .*not in TrustedPublisher\r?$"),"certificate absent from one store MISSING");
        report = ManifestCheck.Report(m, dir, kmd, expand, files.ContainsKey, p => files[p], s => { throw new UnauthorizedAccessException(); });
        Check(report.Contains("not in Root (unreadable), TrustedPublisher (unreadable)"), "unreadable stores named");
        Throws<FormatException>(() => ManifestCheck.Parse(@"{""schema"": 2}"), "schema 2 refused");
        Throws<FormatException>(() => ManifestCheck.Parse("[1]"), "not an object refused");

        var interop = new InteropState { Flags = InteropState.FlagValid, Effective = 3, Users = 1 };
        Equal("CPU route (GPU route disabled: DwmForceCpu 1)", KmdReply.CompositionLine(1, interop, null), "router forces CPU");
        Equal("GPU (1 device on the GPU path)", KmdReply.CompositionLine(0, interop, null), "router on GPU, KMD decides");
        Equal("GPU (1 device on the GPU path)", KmdReply.CompositionLine(null, interop, null), "no router key");
        Equal("not loaded", KmdReply.CompositionLine(null, null, "not loaded"), "no interop reply");
    }

    // ---- Recovery ----------------------------------------------------------------------------------------------

    static StartHealthState Eligible() { return new StartHealthState { Flags = 7, Generation = 5, Epoch = 2, Completed = 40, ReadyAgeMs = 70000, LastCompletionAgeMs = 800 }; }

    // The state BD-059 leaves behind: switches closed by the driver after a restart it took as unclean, desktop on
    // the CPU route (release default), clocks fell back to fixed after the same "unclean" stop, start not confirmed.
    static RecoverySnapshot Closed()
    {
        return new RecoverySnapshot
        {
            DriverInstalled = true, RouterInstalled = true, DwmForceCpu = 1, RequireKmdSwitches = 1, GpuDesktopModules = true,
            Parameters = new Dictionary<string, long>
            {
                { "EnableGpuPresentBlit", 0 }, { "EnableCddDwmInterop", 0 }, { "InteropClosedReason", 4 }, { "InteropLastState", 0x300 },
                { "InteropLastReason", 4 }, { "UnconfirmedStarts", 1 }, { "LastStage", 61 }, { "DpmMode", 0 }, { "DpmMaxMHz", 1500 },
                { "DpmLastMode", 0 }, { "DpmLastReason", 4 },
            },
            Interop = new InteropState { Version = 0x000700C5, Flags = InteropState.FlagValid | InteropState.FlagClosedByDriver | InteropState.FlagUnclean, Requested = 3, Effective = 0, Reason = 4, ClosedReason = 4 },
            Health = Eligible(),
            Dpm = new DpmState { Mode = 0, Reason = 4 },
            DwmRoute = "cpu", TaskFound = true, TaskResult = 1, TaskLastRun = "2026-10-03 10:00",
            DefaultParameters = new Dictionary<string, long> { { "EnableGpuPresentBlit", 1 }, { "EnableCddDwmInterop", 1 }, { "DpmMode", 1 }, { "DpmMaxMHz", 1500 }, { "EnableMmio", 1 } },
            DefaultRouter = new Dictionary<string, long> { { "DwmForceCpu", 1 } },
        };
    }

    static RecoverySnapshot Open()
    {
        var s = Closed();
        s.Parameters["EnableGpuPresentBlit"] = 1; s.Parameters["EnableCddDwmInterop"] = 1; s.Parameters.Remove("InteropClosedReason");
        s.Parameters["InteropLastState"] = 0x303; s.Parameters["InteropLastReason"] = 0;
        s.Interop = new InteropState { Flags = InteropState.FlagValid, Requested = 3, Effective = 3, Reason = 0 };
        return s;
    }

    static bool Writes(ActionPlan p, params string[] expected)
    {
        var got = p.Writes.Select(w => w.Name + (w.Delete ? "-" : "=" + w.Number)).ToArray();
        return got.SequenceEqual(expected);
    }

    static void RecoveryRules(string root, string header, string startConfirmCore)
    {
        // Constants against the KMD sources.
        Check(Regex.IsMatch(header, @"#define BC250_START_HEALTH_REQUIRED " + Recovery.ConfirmRequiredFlags + @"u\b"), "confirm flags = BC250_START_HEALTH_REQUIRED");
        Check(Regex.IsMatch(header, @"#define BC250_START_HEALTH_MIN_MS " + Recovery.ConfirmMinReadyMs + @"ull\b"), "confirm ready = BC250_START_HEALTH_MIN_MS");
        var kmdh = File.ReadAllText(Path.Combine(root, @"driver\kmd\bc250kmd.h"));
        Check(Regex.IsMatch(kmdh, @"#define BC250_MAX_UNCONFIRMED_STARTS " + Recovery.MaxUnconfirmedStarts + @"\b"), "unconfirmed start limit = BC250_MAX_UNCONFIRMED_STARTS");
        var stages = Regex.Match(kmdh, @"typedef enum _BC250_STAGE \{(?<b>.*?)\} BC250_STAGE;", RegexOptions.Singleline).Groups["b"].Value;
        foreach (Match m in Regex.Matches(stages, @"(\w+) = (\d+),"))
            Check(!Recovery.StageText(uint.Parse(m.Groups[2].Value)).StartsWith("stage "), "stage " + m.Groups[1].Value + " has a text");
        var policy = File.ReadAllText(Path.Combine(root, @"driver\kmd\interop_policy.h"));
        foreach (Match m in Regex.Matches(policy, @"BC250_INTEROP_REASON_(\w+) = (\d+),"))
            Check(!Recovery.InteropReasonText(uint.Parse(m.Groups[2].Value)).StartsWith("reason "), "interop reason " + m.Groups[1].Value + " has a text");
        foreach (var flag in new[] { "VALID 1u", "SESSION 2u", "UNCLEAN 4u", "STALE 8u", "CLOSED_BY_DRIVER 16u" })
            Check(Regex.IsMatch(header, @"#define BC250_INTEROP_FLAG_" + flag + @"\b"), "interop flag " + flag);
        Check(InteropState.FlagUnclean == 4 && InteropState.FlagStale == 8 && InteropState.FlagClosedByDriver == 16, "interop flag constants");
        var interopC = File.ReadAllText(Path.Combine(root, @"driver\kmd\interop.c"));
        foreach (var n in new[] { "EnableGpuPresentBlit", "EnableCddDwmInterop", "InteropClosedReason", "InteropLastState" })
            Check(interopC.Contains("L\"" + n + "\""), "interop.c names " + n);
        var dpmC = File.ReadAllText(Path.Combine(root, @"driver\kmd\dpm.c"));
        foreach (var n in new[] { "DpmMode", "DpmMaxMHz", "DpmPending", "DpmConfirmed", "DpmLastMode", "DpmLastReason" })
            Check(dpmC.Contains("L\"" + n + "\""), "dpm.c names " + n);

        // The installer's own rule, when the build passes its file.
        if (startConfirmCore != null)
        {
            var ps = File.ReadAllText(startConfirmCore);
            Check(Regex.IsMatch(ps, @"\$script:StartConfirmMinReadyMs = " + Recovery.ConfirmMinReadyMs + @"\b"), "start-confirm-core ready bound");
            Check(Regex.IsMatch(ps, @"\$script:StartConfirmFreshMs = " + Recovery.ConfirmFreshMs + @"\b"), "start-confirm-core fresh bound");
            var rule = Regex.Match(ps, @"function Test-StartConfirmEligible \{(?<b>.*?)\n\}", RegexOptions.Singleline).Groups["b"].Value;
            foreach (var part in new[] { @"($Reading.flags -band 7) -eq 7", "$Reading.completed -gt 0", "$Reading.ready_ms -ge $script:StartConfirmMinReadyMs", "$Reading.age_ms -le $script:StartConfirmFreshMs" })
                Check(rule.Contains(part), "Test-StartConfirmEligible has " + part);
            Equal(4, Regex.Matches(rule, @"-and").Count + 1, "Test-StartConfirmEligible has exactly four terms");
            Check(ps.Contains("($Reading.flags -band 9) -eq 9"), "start-confirm-core confirmed = flags 9");
        }

        // The confirmation rule.
        Check(Recovery.ConfirmEligible(Eligible()), "eligible reading");
        var h = Eligible(); h.ReadyAgeMs = 60000; h.LastCompletionAgeMs = 5000;
        Check(Recovery.ConfirmEligible(h), "eligible at both bounds");
        h = Eligible(); h.Flags = 6; Check(!Recovery.ConfirmEligible(h) && Recovery.ConfirmBlocker(h, false).Contains("not a full"), "not full: refused");
        h = Eligible(); h.Flags = 5; Check(Recovery.ConfirmBlocker(h, false).Contains("not finished"), "not ready: refused");
        h = Eligible(); h.Flags = 3; Check(Recovery.ConfirmBlocker(h, false).Contains("screen"), "not visible: refused");
        h = Eligible(); h.Completed = 0; Check(!Recovery.ConfirmEligible(h) && Recovery.ConfirmBlocker(h, false).Contains("any work"), "no completions: refused");
        h = Eligible(); h.ReadyAgeMs = 59999; Check(!Recovery.ConfirmEligible(h) && Recovery.ConfirmBlocker(h, false).Contains("1 s left"), "ready 59.999 s: refused");
        h = Eligible(); h.LastCompletionAgeMs = 5001;
        Check(!Recovery.ConfirmEligible(h) && Recovery.ConfirmBlocker(h, true) != null && Recovery.ConfirmBlocker(h, false) == null, "stale completion: the helper waits, the window does not refuse");
        h = Eligible(); h.Flags = 15; Check(Recovery.Confirmed(h), "flags 15 confirmed");
        h.Flags = 8; Check(!Recovery.Confirmed(h), "CONFIRMED without FULL is not confirmed");

        // The allow-list.
        foreach (var n in Recovery.ParameterNames) Check(Recovery.Allowed(Recovery.ParametersPath, n), "allowed " + n);
        Check(Recovery.Allowed(Recovery.RouterPath, "DwmForceCpu"), "allowed DwmForceCpu");
        foreach (var n in new[] { "UnconfirmedStarts", "EnableMmio", "EnableFullWddm", "KeepLog", "CuMode", "DpmPending", "InteropSession", "DwmForceCpu", "ImagePath" })
            Check(!Recovery.Allowed(Recovery.ParametersPath, n), "not allowed: Parameters " + n);
        foreach (var path in new[] { @"SYSTEM\CurrentControlSet\Services\bc250kmd", @"SYSTEM\CurrentControlSet\Control\CI\Policy", @"SOFTWARE\amdgpu-wddm\AppRouter" })
            Check(!Recovery.Allowed(path, "EnableGpuPresentBlit") && !Recovery.Allowed(path, "DwmForceCpu"), "not allowed: " + path);

        // Not installed: every action refused.
        foreach (var a in Recovery.Actions)
            Check(Recovery.Plan(a, new RecoverySnapshot(), 1, 1500).Refused, a + " refused without the driver");
        Check(Recovery.Plan("format-c", Closed()).Refusal.StartsWith("unknown action"), "unknown action refused");

        // 1. Reopen.
        var p = Recovery.Plan("reopen-gpu-path", Closed());
        Check(!p.Refused && Writes(p, "EnableGpuPresentBlit=1", "EnableCddDwmInterop=1", "InteropClosedReason-"), "reopen writes both switches and deletes the close mark");
        Check(p.OfferRestart && p.Undoable && p.Effect.Contains("next restart"), "reopen: next restart, undoable");
        Check(p.Notes.Any(n => n.Contains("Known issue BD-059:")), "reopen names BD-059 on KMD 0.7.197");
        var c6 = Closed(); c6.Interop.Version = 0x000700C6;
        var p6 = Recovery.Plan("reopen-gpu-path", c6);
        Check(!p6.Notes.Any(n => n.Contains("BD-059")) && p6.Notes.Any(n => n.Contains("bug report")), "reopen on KMD 0.7.198 (BD-059 fixed): no BD-059 note, a bug report if it closes again");
        Check(Writes(p6, "EnableGpuPresentBlit=1", "EnableCddDwmInterop=1", "InteropClosedReason-"), "reopen on KMD 0.7.198 writes the same");
        var unknown = Closed(); unknown.Interop = null; unknown.Health = null; unknown.Dpm = null;
        Check(Recovery.Plan("reopen-gpu-path", unknown).Notes.Any(n => n.Contains("drivers before 0.7.198")), "reopen without a driver reply: BD-059 for older drivers only");
        Check(Recovery.Plan("reopen-gpu-path", Open()).Refusal.Contains("open already"), "reopen refused when open");
        var pending = Closed(); pending.Parameters["EnableGpuPresentBlit"] = 1; pending.Parameters["EnableCddDwmInterop"] = 1; pending.Parameters.Remove("InteropClosedReason");
        Check(Recovery.Plan("reopen-gpu-path", pending).Refusal.Contains("restart Windows"), "reopen refused when reopened, restart pending");
        var off = Closed(); off.Parameters.Remove("InteropClosedReason");
        Check(Writes(Recovery.Plan("reopen-gpu-path", off), "EnableGpuPresentBlit=1", "EnableCddDwmInterop=1"), "reopen of switches the operator closed: no delete");

        // 2. Desktop route.
        p = Recovery.Plan("desktop-gpu", Closed());
        Check(p.Refused && p.Refusal.Contains("Reopen the GPU desktop path"), "GPU route refused while the switches are closed, pointing to the reopen");
        Check(Recovery.Plan("desktop-gpu", pending).Refused, "GPU route refused while the reopen waits for a restart");
        var half = Open(); half.Interop.Effective = 1;
        Check(Recovery.Plan("desktop-gpu", half).Refused, "GPU route refused with one switch effective");
        var stale = Open(); stale.Parameters["InteropLastState"] = 0x301;
        Check(Recovery.Plan("desktop-gpu", stale).Refused, "GPU route refused when the router's InteropLastState is not 3");
        var undecided = Open(); undecided.Interop.Flags = 0;
        Check(Recovery.Plan("desktop-gpu", undecided).Refused, "GPU route refused when the start did not decide");
        var down = Open(); down.Interop = null; down.Health = null; down.Dpm = null; down.DriverError = "not loaded";
        Check(Recovery.Plan("desktop-gpu", down).Refusal.Contains("not running"), "GPU route refused without the driver");
        var noFiles = Open(); noFiles.GpuDesktopModules = false;
        Check(Recovery.Plan("desktop-gpu", noFiles).Refusal.Contains("files"), "GPU route refused without the GPU desktop files");
        var noRouter = Open(); noRouter.RouterInstalled = false;
        Check(Recovery.Plan("desktop-gpu", noRouter).Refusal.Contains("router"), "GPU route refused without the router");
        p = Recovery.Plan("desktop-gpu", Open());
        Check(!p.Refused && Writes(p, "DwmForceCpu=0") && p.OfferRestart && p.Effect == "at the next restart of Windows" && p.Undoable, "GPU route: DwmForceCpu 0, at the next restart, restart offered");
        Check(p.Notes.Contains(Recovery.Bd060Note) && !p.Text().Contains("restart DWM"), "GPU route: no DWM restart, BD-060 named");
        var onGpu = Open(); onGpu.DwmForceCpu = 0; onGpu.DwmRoute = "gpu";
        Check(Recovery.Plan("desktop-gpu", onGpu).Refusal.Contains("already"), "GPU route refused when DWM is on it already");
        onGpu.DwmRoute = "cpu";
        Check(Recovery.Plan("desktop-gpu", onGpu).Refusal.Contains("selected already: restart Windows"), "GPU route selected but not running yet: refused, restart Windows");
        onGpu.DwmRoute = "gpu";
        p = Recovery.Plan("desktop-cpu", Closed());
        Check(Recovery.Plan("desktop-cpu", Closed()).Refusal.Contains("already"), "CPU route refused when DWM is on it already");
        var cpuUnknown = Closed(); cpuUnknown.DwmRoute = "unknown";
        Check(Recovery.Plan("desktop-cpu", cpuUnknown).Refusal.Contains("selected already: restart Windows"), "CPU route stored, running route unknown: refused, restart Windows");
        cpuUnknown.DwmForceCpu = 0;
        p = Recovery.Plan("desktop-cpu", cpuUnknown);
        Check(!p.Refused && Writes(p, "DwmForceCpu=1") && p.OfferRestart && p.Effect == "at the next restart of Windows" && p.Notes.Contains(Recovery.Bd060Note), "CPU route: DwmForceCpu 1, at the next restart, BD-060 named");
        Check(!Recovery.Plan("desktop-cpu", onGpu).Refused, "CPU route allowed from the GPU route");

        // 3. Confirm.
        p = Recovery.Plan("confirm-start", Closed());
        Check(!p.Refused && p.ConfirmStart && p.Writes.Count == 0 && !p.Undoable, "confirm: the escape only, not undoable");
        var c = Closed(); c.Health.Flags = 15;
        Check(Recovery.Plan("confirm-start", c).Refusal.Contains("confirmed already"), "confirm refused when confirmed");
        // The logon task confirmed the start, then the epoch moved on (flags back to 7): the guard counter decides.
        c = Closed(); c.Health.Flags = 7; c.Parameters["UnconfirmedStarts"] = 0;
        Check(Recovery.StartConfirmed(c) && Recovery.Plan("confirm-start", c).Refusal.Contains("confirmed already"), "confirm refused when the guard counter is 0");
        c.Parameters["UnconfirmedStarts"] = 1;
        Check(!Recovery.StartConfirmed(c) && !Recovery.Plan("confirm-start", c).Refused, "confirm offered while the guard counts this start");
        c.Parameters["UnconfirmedStarts"] = 0; c.Parameters["DpmPending"] = 1;
        Check(!Recovery.StartConfirmed(c), "a clock trial pending still needs the confirmation");
        c = Closed(); c.Health.ReadyAgeMs = 30000;
        Check(Recovery.Plan("confirm-start", c).Refusal.Contains("60 seconds"), "confirm refused before 60 s");
        c = Closed(); c.Health.Flags = 6;
        Check(Recovery.Plan("confirm-start", c).Refused, "confirm refused on a display-only start");
        Check(Recovery.Plan("confirm-start", down).Refusal.Contains("not running"), "confirm refused without the driver");

        // 4. Clocks.
        p = Recovery.Plan("enable-dpm", Closed(), null, 1800);
        Check(!p.Refused && Writes(p, "DpmMode=1", "DpmMaxMHz=1800") && p.OfferRestart && p.Undoable, "enable-dpm writes mode 1 and the ceiling");
        Check(p.Notes.Any(n => n.Contains("hotter")), "a ceiling above 1500 warns");
        Check(Writes(Recovery.Plan("enable-dpm", Closed()), "DpmMode=1"), "enable-dpm without a chosen ceiling keeps the stored one");
        var noCeiling = Closed(); noCeiling.Parameters.Remove("DpmMaxMHz");
        Check(Writes(Recovery.Plan("enable-dpm", noCeiling), "DpmMode=1"), "enable-dpm writes no implicit ceiling");
        foreach (uint bad in new uint[] { 900, 1550, 2100, 0 })
            Check(Recovery.Plan("enable-dpm", Closed(), null, bad).Refused, "ceiling " + bad + " refused");
        var dpmOn = Closed(); dpmOn.Parameters["DpmMode"] = 1;
        Check(Recovery.Plan("enable-dpm", dpmOn, null, 1500).Refusal.Contains("stored already"), "enable-dpm refused when stored");
        Check(Recovery.Plan("set-clocks", Closed(), 2, 1500).Refused, "clock mode 2 refused");
        Check(Recovery.Plan("set-clocks", Closed(), 0, 1500).Refusal.Contains("Only DpmMode 1"), "an explicit DpmMode 0 is not written");
        Check(Writes(Recovery.Plan("set-clocks", dpmOn, null, null), "DpmMode-", "DpmMaxMHz-"), "set-clocks with both unchecked removes both");
        Check(Writes(Recovery.Plan("set-clocks", dpmOn, null, 1500), "DpmMode-"), "set-clocks: unchecking automatic removes DpmMode");
        Check(Recovery.Plan("set-clocks", dpmOn, 1, 1500).Refusal.Contains("stored already"), "set-clocks refused when nothing changes");
        Check(Recovery.Plan("set-clocks", Closed(), null, 1500).Refused, "the driver's own DpmMode 0 is not a change to write");
        Check(Writes(Recovery.Plan("set-clocks", noCeiling, 1, null), "DpmMode=1"), "set-clocks: automatic without a ceiling writes no ceiling");

        // 5. Reset.
        p = Recovery.Plan("reset-defaults", Closed());
        Check(!p.Refused && Writes(p, "EnableGpuPresentBlit=1", "EnableCddDwmInterop=1", "DpmMode=1", "DpmMaxMHz=1500", "DwmForceCpu=1", "InteropClosedReason-"), "reset writes the manifest defaults");
        Check(p.Notes.Count(n => n.Contains("left to the installer")) == 1, "reset names the defaults it leaves to the installer, once");
        Check(p.OfferRestart && p.Undoable, "reset: next restart, undoable");
        var noDefaults = Closed(); noDefaults.DefaultParameters = null; noDefaults.DefaultsError = "manifest.json has no \"defaults\"";
        Check(Recovery.Plan("reset-defaults", noDefaults).Refusal.Contains("no list of default settings"), "reset refused without manifest defaults");
        var badDefault = Closed(); badDefault.DefaultParameters["DpmMaxMHz"] = 2500;
        Check(Recovery.Plan("reset-defaults", badDefault).Refused, "reset refused with a ceiling default out of range");
        badDefault = Closed(); badDefault.DefaultParameters["EnableGpuPresentBlit"] = 2;
        Check(Recovery.Plan("reset-defaults", badDefault).Refused, "reset refused with a switch default out of range");
        badDefault = Closed(); badDefault.DefaultParameters.Remove("DpmMode");
        Check(Recovery.Plan("reset-defaults", badDefault).Refused, "reset refused when a default is missing");
        badDefault = Closed(); badDefault.DefaultRouter = new Dictionary<string, long>();
        Check(Recovery.Plan("reset-defaults", badDefault).Refused, "reset refused without the DwmForceCpu default");
        var atDefaults = Open(); atDefaults.Parameters["DpmMode"] = 1;
        Check(Recovery.Plan("reset-defaults", atDefaults).Refusal.Contains("already"), "reset refused at the defaults");

        // Undo.
        Check(Recovery.Plan("undo", Closed()).Refusal.Contains("no action"), "undo refused without backups");
        Func<string, string, bool, string, BackupValue[], BackupRecord> rec = (file, action, undoable, undoes, values) =>
            new BackupRecord { File = file, Action = action, Utc = "2026-10-03T" + file.Substring(16, 6), Undoable = undoable, Undoes = undoes, Values = values.ToList() };
        var b1 = rec("backup-20260103T100000000Z.json", "reopen-gpu-path", true, null, new[]
        {
            new BackupValue { Path = Recovery.ParametersPath, Name = "EnableGpuPresentBlit", Existed = true, Kind = "DWord", Number = 0 },
            new BackupValue { Path = Recovery.ParametersPath, Name = "InteropClosedReason", Existed = true, Kind = "DWord", Number = 4 },
            new BackupValue { Path = Recovery.ParametersPath, Name = "EnableCddDwmInterop", Existed = false },
        });
        var b2 = rec("backup-20260103T110000000Z.json", "confirm-start", false, null, new BackupValue[0]);
        p = Recovery.Plan("undo", Closed(), null, null, new[] { b1, b2 });
        Check(!p.Refused && p.UndoOf == b1.File && Writes(p, "EnableGpuPresentBlit=0", "InteropClosedReason=4", "EnableCddDwmInterop-"), "undo restores the newest undoable backup, deletes what was absent");
        Check(!p.Undoable && p.OfferRestart && p.Effect == "at the next restart of Windows", "undo: not undoable itself, restart offered");
        var u1 = rec("backup-20260103T120000000Z.json", "undo", false, b1.File, new BackupValue[0]);
        Check(Recovery.Plan("undo", Closed(), null, null, new[] { b1, b2, u1 }).Refused, "an undone backup is not undone twice");
        var b3 = rec("backup-20260103T130000000Z.json", "desktop-cpu", true, null, new[] { new BackupValue { Path = Recovery.RouterPath, Name = "DwmForceCpu", Existed = true, Kind = "DWord", Number = 0 } });
        Equal(b3.File, Recovery.UndoTarget(new[] { b1, b2, u1, b3 }).File, "newest undoable wins");
        Check(Recovery.Plan("undo", Closed(), null, null, new[] { b3 }).Refusal.Contains("GPU desktop path is closed"), "undo onto the GPU route refused while the switches are closed");
        p = Recovery.Plan("undo", Open(), null, null, new[] { b3 });
        Check(!p.Refused && Writes(p, "DwmForceCpu=0") && p.OfferRestart && p.Effect == "at the next restart of Windows" && p.Notes.Contains(Recovery.Bd060Note), "undo onto the GPU route: at the next restart, no DWM restart");
        var old = rec("backup-20260103T130500000Z.json", "desktop-cpu", true, null, b3.Values.ToArray()); old.RestartsDwm = true;
        Check(Recovery.Plan("undo", Open(), null, null, new[] { old }).OfferRestart, "undo of a 0.3 backup that restarted DWM: also at the next restart");
        var evil = rec("backup-20260103T140000000Z.json", "reopen-gpu-path", true, null, new[] { new BackupValue { Path = Recovery.ParametersPath, Name = "UnconfirmedStarts", Existed = true, Kind = "DWord", Number = 2 } });
        Check(Recovery.Plan("undo", Closed(), null, null, new[] { evil }).Refusal.Contains("does not change"), "undo refuses a backup outside the allow-list");
        evil = rec("backup-20260103T150000000Z.json", "reopen-gpu-path", true, null, new[] { new BackupValue { Path = @"SYSTEM\CurrentControlSet\Control\CI\Policy", Name = "DwmForceCpu", Existed = false } });
        Check(Recovery.Plan("undo", Closed(), null, null, new[] { evil }).Refused, "undo refuses a backup of another key");

        // Every allowed plan writes only allowed values.
        foreach (var snap in new[] { Closed(), Open(), cpuUnknown, onGpu })
            foreach (var a in Recovery.Actions)
            {
                var plan = Recovery.Plan(a, snap, 1, 1600, new[] { b1, b3 }, true);
                Check(plan.Writes.All(w => Recovery.Allowed(w.Path, w.Name)), a + " writes only allowed values");
                Check(!plan.RestartCompositor, a + " never stops DWM");
            }

        // The states.
        var lines = Recovery.Describe(Closed());
        Func<List<StateLine>, string, StateLine> line = (ls, topic) => ls.First(l => l.Topic == topic);
        Check(line(lines, "GPU desktop path").Action == "reopen-gpu-path" && line(lines, "GPU desktop path").Severity == "warn", "closed path: warn, recommends the reopen");
        Check(line(lines, "GPU desktop path").Text.Contains("A normal restart can cause this (BD-059)"), "closed path on KMD 0.7.197 names BD-059");
        var fixedState = Closed(); fixedState.Interop.Version = 0x000700C6;
        var fl = line(Recovery.Describe(fixedState), "GPU desktop path");
        Check(fl.Text.StartsWith("Closed by the driver: the last session ended without a clean shutdown (power loss, crash or reset; reason 4).") && !fl.Text.Contains("BD-059") && fl.Action == "reopen-gpu-path",
            "closed path on KMD 0.7.198: an unclean end, no BD-059, reopen recommended");
        fixedState.Interop.Version = 0x000700C7;
        Check(!line(Recovery.Describe(fixedState), "GPU desktop path").Text.Contains("BD-059"), "closed path on a later KMD: no BD-059");
        var healthOnly = Closed(); healthOnly.Interop = null; healthOnly.Health.Version = 0x000700C6;
        Equal(true, Recovery.Bd059Fixed(healthOnly), "the KMD version also comes from the start health reply");
        var noReply = Closed(); noReply.Interop = null; noReply.Health = null; noReply.Dpm = null;
        Equal(null, Recovery.Bd059Fixed(noReply), "no driver reply: version unknown");
        Check(line(Recovery.Describe(noReply), "GPU desktop path").Text.Contains("before 0.7.198"), "closed path, version unknown: both causes named");
        Check(line(lines, "Desktop composition").Text.StartsWith("CPU route (GPU route disabled, BD-058)") && line(lines, "Desktop composition").Action == null, "CPU route: release default, no action");
        Check(line(lines, "Clock control").Action == "enable-dpm", "clock fallback recommends enable-dpm");
        // BD-069: one start after the fallback the live reason is "fixed requested" (1); only the durable record remembers.
        var later = Closed(); later.Dpm = new DpmState { Mode = 0, Reason = 1 }; later.Parameters["DpmLastReason"] = 1; later.Parameters["DpmClosedReason"] = 8;
        var ll = line(Recovery.Describe(later), "Clock control");
        Check(ll.Action == "enable-dpm" && ll.Severity == "warn" && ll.Text.Contains(KmdReply.ReasonText(8)), "a fallback one start later: the durable record still recommends enable-dpm");
        later.Parameters.Remove("DpmClosedReason");
        Check(line(Recovery.Describe(later), "Clock control").Action == null, "no durable record, live reason 1: no recommendation");
        later.Parameters["DpmClosedReason"] = 8; later.Parameters["DpmMode"] = 1;
        Check(line(Recovery.Describe(later), "Clock control").Action != "enable-dpm", "automatic clock already requested: the record is not a fallback any more");
        var gpuDefault = Open(); gpuDefault.DefaultRouter["DwmForceCpu"] = 0;
        var gl = line(Recovery.Describe(gpuDefault), "Desktop composition");
        Check(gl.Text.StartsWith("CPU route (DwmForceCpu 1). The release default is the GPU route.") && gl.Action == "desktop-gpu", "release default GPU (manifest): the CPU route recommends the GPU route");
        gpuDefault = Closed(); gpuDefault.DefaultRouter["DwmForceCpu"] = 0;
        Check(line(Recovery.Describe(gpuDefault), "Desktop composition").Action == null, "release default GPU but switches closed: no GPU recommendation");
        Check(line(lines, "Driver start").Action == "confirm-start" && line(lines, "Driver start").Text.Contains("1 of 2"), "healthy unconfirmed start recommends the confirmation");
        Check(line(lines, "Start confirmation task").Severity == "warn" && line(lines, "Start confirmation task").Text.Contains("gave up"), "task result 1 explained");
        lines = Recovery.Describe(pending);
        Check(line(lines, "GPU desktop path").Action == "restart", "reopened path recommends a restart");
        lines = Recovery.Describe(onGpu);
        Check(line(lines, "Desktop composition").Action == null && line(lines, "Desktop composition").Text.Contains("(desktop-cpu)") && line(lines, "GPU desktop path").Severity == "ok",
            "a healthy GPU route recommends nothing and names the way back");
        var gpuClosed = Closed(); gpuClosed.DwmForceCpu = 0;
        Check(line(Recovery.Describe(gpuClosed), "Desktop composition").Text.Contains("stays on the CPU route"), "GPU route selected with closed switches explained");
        var refused = Closed(); refused.Interop = null; refused.Health = null; refused.Dpm = null; refused.DriverError = "not loaded";
        refused.Parameters["UnconfirmedStarts"] = 2; refused.Parameters["LastStage"] = 90;
        Check(line(Recovery.Describe(refused), "Driver start").Text.Contains("refused to start") && line(Recovery.Describe(refused), "Driver start").Text.Contains("installer"), "guard refusal explained, installer named");
        var early = Closed(); early.Health.ReadyAgeMs = 1000;
        Check(line(Recovery.Describe(early), "Driver start").Action == null, "an early start recommends waiting");
        var trial = Open(); trial.Parameters["DpmMode"] = 1; trial.Parameters["DpmPending"] = 0x10005DC; trial.Dpm = new DpmState { Mode = 1, MaxMHz = 1500 };
        Check(line(Recovery.Describe(trial), "Clock control").Action == "confirm-start", "clocks on trial recommend the confirmation");
        var bare = Recovery.Describe(new RecoverySnapshot());
        Check(bare.Count == 2 && bare[0].Topic == "Desktop compositor" && bare[1].Topic == "Driver", "not installed: the compositor line and the driver line");

        // The selected route (DwmForceCpu) and the active route (the running DWM's modules) are separate; a write
        // after this DWM started is pending until the restart (review 907).
        var cpuPending = Open(); cpuPending.DwmForceCpu = 1; cpuPending.DwmRoute = "gpu";
        var cp = line(Recovery.Describe(cpuPending), "Desktop composition");
        Check(cp.Text.StartsWith("Selected: the CPU route; the running DWM loaded the GPU route. A route chosen in this session applies at the next restart of Windows.") &&
            cp.Action == "restart" && cp.Severity == "info", "CPU selected, GPU active, timing unknown: restart recommended, not a completed switch");
        cpuPending.DwmNow = Reading(900, "2026-10-03T18:00:03.000Z"); cpuPending.RouteWrittenUtc = "2026-10-03T18:10:00.000Z"; cpuPending.RouteWrittenValue = 0;
        Check(line(Recovery.Describe(cpuPending), "Desktop composition").Text.StartsWith("Selected: the CPU route; the running DWM loaded the GPU route."),
            "a route record of another value (an older write, or a failed one) gives no timing");
        cpuPending.RouteWrittenValue = 1;
        cp = line(Recovery.Describe(cpuPending), "Desktop composition");
        Check(cp.Text.StartsWith("Selected for the next start: the CPU route, pending until Windows restarts. Active now: the GPU route") && cp.Action == "restart",
            "CPU route written after this DWM started: pending until the restart");
        cpuPending.DwmNow = Reading(901, "2026-10-03T18:20:00.000Z");
        cp = line(Recovery.Describe(cpuPending), "Desktop composition");
        Check(cp.Severity == "warn" && cp.Text.StartsWith("The CPU route was selected at 2026-10-03 18:10:00 UTC, but the DWM that started after it loaded the GPU route.") &&
            cp.Text.EndsWith("Create a bug report."), "a DWM started after the write on the other route: verification failed, bug report");
        var gpuPending = Open(); gpuPending.DwmForceCpu = 0; gpuPending.DwmRoute = "cpu";
        var gp = line(Recovery.Describe(gpuPending), "Desktop composition");
        Check(gp.Text.StartsWith("Selected: the GPU route; the running DWM loaded the CPU route.") && gp.Action == "restart", "GPU selected, CPU active: restart recommended");
        var verified = Open(); verified.DwmForceCpu = 0; verified.DwmRoute = "gpu";
        verified.DwmNow = Reading(901, "2026-10-03T18:20:00.000Z"); verified.RouteWrittenUtc = "2026-10-03T18:10:00.000Z"; verified.RouteWrittenValue = 0;
        Check(line(Recovery.Describe(verified), "Desktop composition").Text.StartsWith("GPU route: selected and active (the DWM that started after the change loaded it).") &&
            line(Recovery.Describe(verified), "Desktop composition").Action == null && line(Recovery.Describe(verified), "Desktop composition").Severity == "info",
            "GPU route verified on the DWM started after the change: healthy, nothing recommended");
        var failing = Open(); failing.DwmForceCpu = 0; failing.DwmRoute = "gpu"; failing.DwmNow = Reading(901, "2026-10-03T18:30:00.000Z");
        failing.DwmHistory = Recovery.Observe(new[] { Recovery.Observe(null, Reading(900, "2026-10-03T18:00:03.000Z"), "window", "2026-10-03T18:01:00.000Z") },
            failing.DwmNow, "window", "2026-10-03T18:30:02.000Z");
        var fg = line(Recovery.Describe(failing), "Desktop composition");
        Check(Recovery.DwmVerdict(failing) == "observed" && fg.Severity == "warn" && fg.Action == "desktop-cpu" && fg.Text.Contains("may be failing"),
            "GPU route with a DWM replaced in this session: the CPU route recommended");
        verified.DwmNow = Reading(900, "2026-10-03T18:00:03.000Z");
        Check(line(Recovery.Describe(verified), "Desktop composition").Text.StartsWith("GPU route: selected and active. "), "GPU active before the write: not called verified by the change");
        var cpuVerified = Closed(); cpuVerified.DwmNow = Reading(901, "2026-10-03T18:20:00.000Z"); cpuVerified.RouteWrittenUtc = "2026-10-03T18:10:00.000Z"; cpuVerified.RouteWrittenValue = 1;
        Check(line(Recovery.Describe(cpuVerified), "Desktop composition").Text.EndsWith("Active now: the CPU route. The DWM that started after the change loaded it."), "CPU route verified on the DWM started after the change");
        var blind = Open(); blind.DwmForceCpu = 0; blind.DwmRoute = "unknown";
        blind.DwmNow = Reading(900, "2026-10-03T18:00:03.000Z"); blind.RouteWrittenUtc = "2026-10-03T18:10:00.000Z"; blind.RouteWrittenValue = 0;
        var bl = line(Recovery.Describe(blind), "Desktop composition");
        Check(bl.Text == "Selected for the next start: the GPU route, pending until Windows restarts. The active route cannot be read." && bl.Action == "restart",
            "pending write, active route unreadable: pending, never verified");
        blind.RouteWrittenUtc = null; blind.RouteWrittenValue = null;
        Check(!line(Recovery.Describe(blind), "Desktop composition").Text.Contains("pending"), "no route record (only a backup, or a rolled-back attempt): not pending");

        // The operator escape: never a window action, refused without --accept-bd060, writes nothing.
        Check(!Recovery.Actions.Contains(Recovery.OperatorEscape), "the operator escape is not a window action");
        var esc = Recovery.Plan(Recovery.OperatorEscape, Closed());
        Check(esc.Refused && esc.Refusal.Contains("--accept-bd060") && esc.Refusal.Contains("BD-060") && !esc.RestartCompositor, "the escape is refused without --accept-bd060");
        esc = Recovery.Plan(Recovery.OperatorEscape, new RecoverySnapshot(), null, null, null, true);
        Check(!esc.Refused && esc.RestartCompositor && !esc.Undoable && esc.Writes.Count == 0 && esc.Notes.Any(n => n.Contains("BD-060") && n.Contains("Restart Windows as soon as you can")),
            "the accepted escape stops DWM, writes nothing, names BD-060 and the restart, and needs no driver");
        Check(esc.Text().Contains("stop DWM in the active session") && esc.Text().Contains("undo: no"), "the escape's plan text");

        Compositor();

        Equal("gpu", Recovery.RouteFromModules(new[] { @"C:\Program Files\amdgpu-wddm\desktop\bc250d3d_router.dll", @"C:\Program Files\amdgpu-wddm\desktop\bc250d3d_zink.dll" }), "route from zink");
        Equal("gpu", Recovery.RouteFromModules(new[] { @"C:\Program Files\amdgpu-wddm\desktop\amdgpu_wddm_radv.dll" }), "route from the desktop RADV");
        Equal("unknown", Recovery.RouteFromModules(new[] { @"C:\Program Files\amdgpu-wddm\d3d12\amdgpu_wddm_radv.dll" }), "the D3D12 RADV is not the desktop");
        Equal("cpu", Recovery.RouteFromModules(new[] { @"C:\Program Files\amdgpu-wddm\desktop\bc250d3d_router.dll", @"C:\Program Files\amdgpu-wddm\desktop\bc250d3d.dll" }), "route from the CPU UMD");
        Equal("unknown", Recovery.RouteFromModules(null), "no modules");

        Equal("backup-20261003T101112013Z.json", Recovery.BackupFileName(new DateTime(2026, 10, 3, 10, 11, 12, 13, DateTimeKind.Utc)), "backup file name");
        Check(Recovery.TaskResultText(0).Contains("confirmed") && Recovery.TaskResultText(0x41303).Contains("not run"), "task result texts");

        // The start-confirm task's last run: the newest run of any source (lab, 678e770c: COM "never, 0x41303" from an
        // elevated session-0 shell while Get-ScheduledTaskInfo reported 21:34:34, result 0).
        var comNever = new TaskRun { LastRun = new DateTime(1999, 11, 30), Result = 0x41303, Source = "com" };
        var wmiRan = new TaskRun { LastRun = new DateTime(2026, 10, 3, 21, 34, 34), Result = 0, Source = "wmi" };
        var newest = Recovery.NewestTaskRun(new[] { comNever, wmiRan });
        Check(newest.Source == "wmi" && newest.Result == 0 && Recovery.TaskLastRunText(newest) == "2026-10-03 21:34", "task: a source that saw the run wins over one that says never");
        var ranState = Closed(); ranState.TaskResult = newest.Result; ranState.TaskLastRun = Recovery.TaskLastRunText(newest);
        var tl = line(Recovery.Describe(ranState), "Start confirmation task");
        Check(tl.Text.StartsWith("Last run 2026-10-03 21:34: ") && !tl.Text.Contains("never") && tl.Severity == "ok", "task: the lab case reads as run, not never");
        Equal("com", Recovery.NewestTaskRun(new[] { new TaskRun { LastRun = new DateTime(2026, 10, 3, 22, 0, 0), Result = 1, Source = "com" }, wmiRan }).Source, "task: the newer run wins");
        Check(Recovery.NewestTaskRun(new[] { comNever, null, new TaskRun { LastRun = null, Result = null, Source = "wmi" } }).Source == "com" && Recovery.TaskLastRunText(comNever) == "never",
            "task: no source with a run: never");
        Equal(null, Recovery.NewestTaskRun(new TaskRun[] { null, null }), "task: no source: nothing");
        Equal(null, Recovery.NewestTaskRun(null), "task: no readings");

        // JSON: the snapshot (--snapshot) and the backup round-trip.
        var json = new JavaScriptSerializer();
        var back = json.Deserialize<RecoverySnapshot>(json.Serialize(Closed()));
        Check(back.Health.ReadyAgeMs == 70000 && back.Interop.ClosedReason == 4 && back.P("InteropLastState") == 0x300 && back.DwmForceCpu == 1 &&
            back.DefaultParameters["DpmMaxMHz"] == 1500, "snapshot JSON round-trip");
        Check(Writes(Recovery.Plan("reset-defaults", back), "EnableGpuPresentBlit=1", "EnableCddDwmInterop=1", "DpmMode=1", "DpmMaxMHz=1500", "DwmForceCpu=1", "InteropClosedReason-"), "plans from a round-tripped snapshot match");
        var bb = json.Deserialize<BackupRecord>(json.Serialize(b1));
        Check(bb.Values.Count == 3 && bb.Values[1].Number == 4 && !bb.Values[2].Existed && bb.Undoable, "backup JSON round-trip");

        // manifest.json "defaults".
        var mf = ManifestCheck.Parse(@"{""schema"": 1, ""defaults"": {""parameters"": {""DpmMode"": 1, ""DpmMaxMHz"": 1500}, ""desktop_router"": {""DwmForceCpu"": 1}}}");
        Check(mf.DefaultParameters["DpmMaxMHz"] == 1500 && mf.DefaultRouter["DwmForceCpu"] == 1, "manifest defaults parsed");
        Check(ManifestCheck.Parse(@"{""schema"": 1}").DefaultParameters == null, "manifest without defaults");
        Throws<FormatException>(() => ManifestCheck.Parse(@"{""defaults"": {""parameters"": {""DpmMode"": ""1""}}}"), "manifest default as text refused");
        Throws<FormatException>(() => ManifestCheck.Parse(@"{""defaults"": {""parameters"": {""DpmMode"": -1}}}"), "negative manifest default refused");
        Throws<FormatException>(() => ManifestCheck.Parse(@"{""defaults"": [1]}"), "manifest defaults as a list refused");
    }

    // The settings rule (owner, 2026-10-03): unchecked = not written, unchecking removes, the exact set is written.
    static string Show(IEnumerable<RegWrite> writes) { return string.Join(" ", writes.Select(w => w.Name + (w.Delete ? "-" : "=" + w.Number))); }

    static void SettingsRule()
    {
        var none = new string[0];
        Equal(ProfileWriteKind.None, Profiles.PlanWrite("a.exe", null, none).Kind, "profile: nothing checked, no key: nothing written");
        Equal(ProfileWriteKind.None, Profiles.PlanWrite("a.exe", null, null).Kind, "profile: no names at all: nothing written");
        var w = Profiles.PlanWrite("a.exe", "raytracing-tier-off", none);
        Equal(ProfileWriteKind.Delete, w.Kind, "profile: checked, saved, then unchecked: the key is removed");
        Equal(ProfileWriteKind.Delete, Profiles.PlanWrite("a.exe", "", none).Kind, "profile: an empty stored value is removed, not kept");
        w = Profiles.PlanWrite("a.exe", "raytracing-tier-off,x-future", new[] { "raytracing-tier-off" });
        Check(w.Kind == ProfileWriteKind.Set && w.Value == "raytracing-tier-off", "profile: unchecking one name removes exactly that name");
        w = Profiles.PlanWrite("a.exe", null, new[] { "deferred-replay-off", "x-future", "raytracing-tier-off" });
        Check(w.Kind == ProfileWriteKind.Set && w.Value == "raytracing-tier-off,deferred-replay-off,x-future", "profile: the exact checked set is written, catalog order first");
        Equal(ProfileWriteKind.None, Profiles.PlanWrite("a.exe", "raytracing-tier-off,deferred-replay-off", new[] { "deferred-replay-off", "raytracing-tier-off" }).Kind, "profile: same set: nothing written");
        const string witcher = "present-noprimary,present-cached,raytracing-tier-off,recording-bind-off,retire-handoff-off,deferred-replay-off";
        var parsed = Profiles.Parse(witcher);
        Equal(6, parsed.Known.Count, "installer profile: every name shows as checked");
        Equal(ProfileWriteKind.None, Profiles.PlanWrite("witcher3.exe", witcher, parsed.Known.Concat(parsed.Unknown)).Kind, "installer profile in its own order: not a change");
        w = Profiles.PlanWrite("witcher3.exe", witcher, parsed.Known.Where(n => n != "deferred-replay-off"));
        Check(w.Kind == ProfileWriteKind.Set && !w.Value.Contains("deferred-replay-off") && w.Value.Split(',').Length == 5, "installer profile: one unchecked name is removed");
        Throws<ArgumentException>(() => Profiles.PlanWrite("a.exe", "BAD VALUE", new[] { "BAD VALUE" }), "profile: a checked value outside the syntax is not written");
        Equal(ProfileWriteKind.Delete, Profiles.PlanWrite("a.exe", "BAD VALUE", none).Kind, "profile: an unchecked bad value is removed");

        Equal("", Show(DpmSettings.PlanWrites(null, null, false, null)), "clocks: nothing checked, nothing stored: nothing written");
        Equal("DpmMode=1", Show(DpmSettings.PlanWrites(null, null, true, null)), "clocks: automatic checked writes DpmMode 1 only");
        Equal("", Show(DpmSettings.PlanWrites(null, null, false, null)), "clocks: checked, then unchecked before Apply: nothing written");
        Equal("DpmMode- DpmMaxMHz-", Show(DpmSettings.PlanWrites(1, 1500, false, null)), "clocks: unchecking after a save removes both values");
        Equal("", Show(DpmSettings.PlanWrites(0, null, false, null)), "clocks: the driver's fallback 0 stays when unchecked");
        Equal("DpmMaxMHz=1800", Show(DpmSettings.PlanWrites(1, null, true, 1800)), "clocks: the exact ceiling is written");
        Equal("", Show(DpmSettings.PlanWrites(null, 1700, false, 1700)), "clocks: a stored ceiling shown checked is not rewritten");
    }

    static DwmReading Reading(int pid, string created)
    {
        return new DwmReading
        {
            BootId = 53, Session = 1, SessionStartUtc = "2026-10-03T18:00:00.000Z", Pid = pid, CreatedUtc = created,
            BootUtc = "2026-10-03T17:59:20.000Z", LogonUtc = "2026-10-03T18:00:30.000Z",
        };
    }

    // BD-060 (review 907): only a replacement an observer saw is a restart; a first observation is unknown history,
    // never "restarted" and never "healthy"; a new session or boot starts a new baseline.
    static void Compositor()
    {
        var s = Closed();
        Equal("unknown", Recovery.DwmVerdict(s), "dwm: no reading, unknown");
        Equal("dwm-restart: unknown (boot -, session -, session start -, DWM process - started -, instances seen -, watched since -)", Recovery.CompositorStatusLine(s), "dwm: unknown status line");
        Check(line0(s).Severity == "info" && line0(s).Action == null && line0(s).Text == "The desktop compositor (DWM) of the active session cannot be read.", "dwm: unknown, info");
        Equal("-", Recovery.CompositorOverview(s), "dwm: unknown overview");

        var first = Reading(900, "2026-10-03T18:00:03.000Z");
        var h = Recovery.Observe(null, first, "status", "2026-10-03T18:00:40.000Z");
        s.DwmNow = first; s.DwmHistory = h;
        Equal("unknown-history", Recovery.DwmVerdict(s), "dwm: a first observation is unknown history, even 3 s after the session began");
        Equal("No replacement of the desktop compositor (DWM) seen since 2026-10-03 18:00:40 UTC (first seen by --status, 40 s after the session began). A replacement before that would not be seen.",
            Recovery.CompositorText(s), "dwm: unknown history text");
        Equal("dwm-restart: unknown-history (boot 53, session 1, session start 2026-10-03T18:00:00.000Z, DWM process 900 started 2026-10-03T18:00:03.000Z, instances seen 1, watched since 2026-10-03T18:00:40.000Z by status)",
            Recovery.CompositorStatusLine(s), "dwm: unknown history status line");
        Check(line0(s).Severity == "info" && line0(s).Action == null, "dwm: unknown history is info, not ok");
        Equal("No replacement seen since 2026-10-03 18:00:40 UTC (earlier: not known)", Recovery.CompositorOverview(s), "dwm: unknown history overview");

        // The false positive of the start-time rule: a DWM created days after the session, first seen late.
        var lateDwm = Reading(4242, "2026-10-08T22:24:14.000Z");
        var late = Closed(); late.DwmNow = lateDwm; late.DwmHistory = Recovery.Observe(null, lateDwm, "window", "2026-10-09T08:00:00.000Z");
        Equal("unknown-history", Recovery.DwmVerdict(late), "dwm: a late DWM seen once is not called a restart");
        Check(Recovery.CompositorText(late).Contains("6 days after the session began"), "dwm: the late observer is named with its delay");

        var again = Recovery.Observe(new[] { h }, first, "window", "2026-10-03T19:00:00.000Z");
        Check(again.Instances.Count == 1 && again.Instances[0].FirstSeenUtc == "2026-10-03T18:00:40.000Z" && again.Instances[0].Observer == "status", "dwm: the same instance keeps its first observation");

        // Replacement observed; the same process id with another creation time is another instance.
        var second = Reading(900, "2026-10-03T18:30:00.000Z");
        var r = Recovery.Observe(new[] { again }, second, "window", "2026-10-03T18:30:02.000Z");
        s.DwmNow = second; s.DwmHistory = r;
        Equal("observed", Recovery.DwmVerdict(s), "dwm: a reused process id with a new creation time is a replacement");
        Equal("The desktop compositor (DWM) was replaced in this session: 1 replacement seen; the DWM now running (process 900) started 2026-10-03 18:30:00 UTC. " +
            "After a DWM restart some Windows 11 apps, for example the Explorer command bar and Task Manager, can ignore mouse clicks until Windows restarts (BD-060): restart Windows. " +
            "If nobody restarted DWM on purpose, create a bug report: a DWM crash can be a driver defect.", Recovery.CompositorText(s), "dwm: observed text, restart remedy, bug report kept");
        Check(line0(s).Severity == "warn" && line0(s).Action == "restart", "dwm: observed, warn, restart recommended");
        Equal("dwm-restart: observed (boot 53, session 1, session start 2026-10-03T18:00:00.000Z, DWM process 900 started 2026-10-03T18:30:00.000Z, instances seen 2, watched since 2026-10-03T18:00:40.000Z by status)",
            Recovery.CompositorStatusLine(s), "dwm: observed status line");
        Equal("Replaced in this session: restart Windows (BD-060, see Recovery)", Recovery.CompositorOverview(s), "dwm: observed overview");
        var third = Reading(950, "2026-10-03T18:45:00.000Z");
        s.DwmNow = third; s.DwmHistory = Recovery.Observe(new[] { r }, third, "status", "2026-10-03T18:46:00.000Z");
        Check(Recovery.CompositorText(s).Contains("2 replacements seen"), "dwm: replacements counted");

        // The user's and the administrator's copies merge; the earliest observation wins.
        var userCopy = Recovery.Observe(null, first, "window", "2026-10-03T18:01:00.000Z");
        var adminCopy = Recovery.Observe(null, first, "helper", "2026-10-03T18:00:20.000Z");
        var merged = Recovery.Observe(new[] { userCopy, null, adminCopy }, first, "status", "2026-10-03T18:05:00.000Z");
        Check(merged.Instances.Count == 1 && merged.Instances[0].Observer == "helper" && merged.Instances[0].FirstSeenUtc == "2026-10-03T18:00:20.000Z", "dwm: two copies merge");

        // Damaged entries of the app's own copies never make a replacement (reviewer 911's reproduction).
        var stored = new DwmObservations { BootId = 53, Session = 1, SessionStartUtc = "2026-10-03T18:00:00.000Z" };
        stored.Instances.Add(new DwmInstance { Pid = 900, CreatedUtc = "2026-10-03T18:00:03.000Z", FirstSeenUtc = "2026-10-03T18:00:40.000Z", Observer = "window" });
        var v = Closed(); v.DwmNow = first; v.DwmHistory = Recovery.Observe(new[] { stored }, first, "status", "2026-10-03T19:00:00.000Z");
        Check(v.DwmHistory.Instances.Count == 1 && Recovery.DwmVerdict(v) == "unknown-history", "own copy: the same valid identity, one instance");
        foreach (var bad in new[]
        {
            new DwmInstance { Pid = 900, CreatedUtc = "bad", FirstSeenUtc = "2026-10-03T18:00:40.000Z", Observer = "window" },
            new DwmInstance { Pid = 0, CreatedUtc = "2026-10-03T18:00:03.000Z", FirstSeenUtc = "2026-10-03T18:00:40.000Z", Observer = "window" },
            new DwmInstance { Pid = -7, CreatedUtc = "2026-10-03T18:10:00.000Z", FirstSeenUtc = "2026-10-03T18:10:40.000Z", Observer = "window" },
            new DwmInstance { Pid = 901, CreatedUtc = null, FirstSeenUtc = "2026-10-03T18:10:40.000Z", Observer = "window" },
            new DwmInstance { Pid = 901, CreatedUtc = "2026-10-03T18:10:00.000Z", FirstSeenUtc = "never", Observer = "window" },
            null,
        })
        {
            var damaged = new DwmObservations { BootId = 53, Session = 1, SessionStartUtc = "2026-10-03T18:00:00.000Z" };
            damaged.Instances.Add(bad);
            var d = Closed(); d.DwmNow = first; d.DwmHistory = Recovery.Observe(new[] { damaged }, first, "status", "2026-10-03T19:00:00.000Z");
            string what = bad == null ? "null" : "pid " + bad.Pid + " created " + (bad.CreatedUtc ?? "null") + " first seen " + bad.FirstSeenUtc;
            Check(d.DwmHistory.Instances.Count == 1 && Recovery.DwmVerdict(d) == "unknown-history", "own copy with a damaged entry (" + what + "): ignored, never observed");
            var raw = Closed(); raw.DwmNow = first; raw.DwmHistory = new DwmObservations { BootId = 53, Session = 1, SessionStartUtc = "2026-10-03T18:00:00.000Z" };
            raw.DwmHistory.Instances.Add(stored.Instances[0]); raw.DwmHistory.Instances.Add(bad);
            Equal("unknown", Recovery.DwmVerdict(raw), "a history holding a damaged entry (" + what + ") is unknown, never observed");
        }
        foreach (var live in new[]
        {
            new DwmReading { BootId = 53, Session = 1, SessionStartUtc = "2026-10-03T18:00:00.000Z", Pid = 0, CreatedUtc = "2026-10-03T18:30:00.000Z" },
            new DwmReading { BootId = 53, Session = 1, SessionStartUtc = "2026-10-03T18:00:00.000Z", Pid = 950, CreatedUtc = "bad" },
            new DwmReading { BootId = 53, Session = 0, SessionStartUtc = "2026-10-03T18:00:00.000Z", Pid = 950, CreatedUtc = "2026-10-03T18:30:00.000Z" },
            new DwmReading { BootId = 53, Session = 1, SessionStartUtc = "garbage", Pid = 950, CreatedUtc = "2026-10-03T18:30:00.000Z" },
        })
        {
            Equal(null, Recovery.Observe(new[] { stored }, live, "status", "2026-10-03T19:00:00.000Z"), "a damaged live reading (pid " + live.Pid + ", created " + live.CreatedUtc + ", session " + live.Session + ") records nothing");
            var lv = Closed(); lv.DwmNow = live; lv.DwmHistory = stored;
            Equal("unknown", Recovery.DwmVerdict(lv), "a damaged live reading is unknown, never observed");
        }
        var fileJson = new JavaScriptSerializer();
        var obsFile = new DwmObservationsFile(); obsFile.Records.Add(stored);
        var obsBack = fileJson.Deserialize<DwmObservationsFile>(fileJson.Serialize(obsFile));
        Check(obsBack.Schema == 2 && obsBack.Records.Count == 1 && obsBack.Records[0].Instances[0].Pid == 900, "observation file JSON round-trip");
        var routeBack = fileJson.Deserialize<RouteRecord>(fileJson.Serialize(new RouteRecord { Schema = 1, Utc = "2026-10-03T18:10:00.000Z", Value = null, Action = "undo" }));
        Check(routeBack.Value == null && routeBack.Action == "undo", "route record JSON round-trip, removed value");

        // Persistence (reviewer 914): a damaged retained epoch is dropped, the current observation is saved, and the
        // next observation is saved too.
        var oldEpoch = new DwmObservations { BootId = 52, Session = 1, SessionStartUtc = "2026-10-02T08:00:00.000Z" };
        oldEpoch.Instances.Add(null);
        var oldGarbage = new DwmObservations { BootId = 51, Session = 1, SessionStartUtc = "2026-10-01T08:00:00.000Z" };
        oldGarbage.Instances.Add(new DwmInstance { Pid = 0, CreatedUtc = "bad", FirstSeenUtc = null });
        var oldGood = new DwmObservations { BootId = 50, Session = 1, SessionStartUtc = "2026-09-30T08:00:00.000Z" };
        oldGood.Instances.Add(new DwmInstance { Pid = 700, CreatedUtc = "2026-09-30T08:00:03.000Z", FirstSeenUtc = "2026-09-30T08:01:00.000Z", Observer = "window" });
        var badIdentity = new DwmObservations { BootId = 49, Session = 0, SessionStartUtc = "x" };
        badIdentity.Instances.Add(new DwmInstance { Pid = 600, CreatedUtc = "2026-09-29T08:00:03.000Z", FirstSeenUtc = "2026-09-29T08:01:00.000Z", Observer = "window" });
        var stored0 = new List<DwmObservations> { oldEpoch, oldGarbage, oldGood, badIdentity };
        var h1 = Recovery.Observe(stored0, first, "status", "2026-10-03T18:00:40.000Z");
        var plan1 = Recovery.PlanCommit(stored0, h1, first, "status", "2026-10-03T18:00:40.000Z");
        Check(plan1 != null && plan1.Write && plan1.File.Records.Count == 2 && plan1.File.Records[0].BootId == 50 && plan1.File.Records[1].BootId == 53 &&
            plan1.File.Records.All(e => e.Instances.All(Recovery.Valid)), "commit: damaged old epochs dropped, the valid old epoch kept, the current one saved");
        var stored1 = new JavaScriptSerializer().Deserialize<DwmObservationsFile>(new JavaScriptSerializer().Serialize(plan1.File)).Records;
        var plan1b = Recovery.PlanCommit(stored1, Recovery.Observe(stored1, first, "window", "2026-10-03T18:05:00.000Z"), first, "window", "2026-10-03T18:05:00.000Z");
        Check(plan1b != null && !plan1b.Write, "commit: the same observation again writes nothing");
        var plan2 = Recovery.PlanCommit(stored1, Recovery.Observe(stored1, second, "window", "2026-10-03T18:30:02.000Z"), second, "window", "2026-10-03T18:30:02.000Z");
        Check(plan2 != null && plan2.Write && plan2.Merged.Instances.Count == 2 && plan2.File.Records.Last().Instances.Count == 2, "commit: the next observation (a replacement) is saved too");
        var cleanRewrite = Recovery.PlanCommit(new List<DwmObservations> { plan1.File.Records[1], oldEpoch }, h1, first, "status", "2026-10-03T18:06:00.000Z");
        Check(cleanRewrite.Write && cleanRewrite.File.Records.Count == 1, "commit: a file that holds a damaged entry is rewritten without it");
        var noList = Recovery.PlanCommit(new List<DwmObservations> { null, new DwmObservations { BootId = 52, Session = 1, SessionStartUtc = "2026-10-02T08:00:00.000Z", Instances = null } }, h1, first, "status", "2026-10-03T18:06:00.000Z");
        Check(noList.Write && noList.File.Records.Count == 1, "commit: a null epoch or one without instances is dropped");
        var many = Enumerable.Range(0, 12).Select(n =>
        {
            var e = new DwmObservations { BootId = 10 + n, Session = 1, SessionStartUtc = "2026-09-0" + (n % 9 + 1) + "T08:00:00.000Z" };
            e.Instances.Add(new DwmInstance { Pid = 100 + n, CreatedUtc = "2026-09-01T08:00:03.000Z", FirstSeenUtc = "2026-09-01T08:" + (10 + n) + ":00.000Z", Observer = "window" });
            return e;
        }).ToList();
        var capped = Recovery.PlanCommit(many, h1, first, "status", "2026-10-03T18:00:40.000Z");
        Check(capped.File.Records.Count == 8 && capped.File.Records[0].BootId == 15 && capped.File.Records[6].BootId == 21, "commit: the 7 newest other epochs are kept, oldest first");

        // The module list (reviewer 914): complete or null, never partial.
        Func<string[], EnumModules> lists = files => (IntPtr[] b, out int needed) =>
        {
            needed = files.Length * IntPtr.Size;
            for (int i = 0; i < Math.Min(files.Length, b.Length); i++) b[i] = new IntPtr(i + 1);
            return true;
        };
        var cpuZink = new[] { @"C:\Windows\System32\dwmcore.dll", @"C:\Program Files\amdgpu-wddm\desktop\bc250d3d_router.dll", @"C:\Program Files\amdgpu-wddm\desktop\bc250d3d.dll", @"C:\Program Files\amdgpu-wddm\desktop\bc250d3d_zink.dll" };
        Func<string[], Func<IntPtr, string>> names = files => m => files[(int)m.ToInt64() - 1];
        var complete = Recovery.CompleteModuleList(lists(cpuZink), names(cpuZink), IntPtr.Size);
        Check(complete != null && complete.Count == 4 && Recovery.RouteFromModules(complete) == "gpu", "modules: the complete list (positive control) gives the GPU route");
        Equal("cpu", Recovery.RouteFromModules(cpuZink.Take(3)), "modules: the list without the Zink entry would read CPU, which is why a partial list must not count");
        Equal(null, Recovery.CompleteModuleList(lists(cpuZink), m => (int)m.ToInt64() == 4 ? null : cpuZink[(int)m.ToInt64() - 1], IntPtr.Size), "modules: a failed name read gives no list (route unknown)");
        Equal(null, Recovery.CompleteModuleList((IntPtr[] b, out int needed) => { needed = 0; return false; }, names(cpuZink), IntPtr.Size), "modules: a failed enumeration gives no list");
        Equal(null, Recovery.CompleteModuleList((IntPtr[] b, out int needed) => { needed = 3; return true; }, names(cpuZink), IntPtr.Size), "modules: a size that is not whole entries gives no list");
        var big = Enumerable.Range(0, 1500).Select(i => @"C:\Windows\System32\m" + i + ".dll").Concat(cpuZink).ToArray();
        int calls = 0;
        var grown = Recovery.CompleteModuleList((IntPtr[] b, out int needed) => { calls++; return lists(big)(b, out needed); }, names(big), IntPtr.Size);
        Check(grown != null && grown.Count == 1504 && calls == 2 && Recovery.RouteFromModules(grown) == "gpu", "modules: more than 1024 modules: one retry with the reported size, the complete list");
        int growing = 1024;
        Equal(null, Recovery.CompleteModuleList((IntPtr[] b, out int needed) => { growing += 2000; needed = growing * IntPtr.Size; return true; }, m => "x.dll", IntPtr.Size),
            "modules: a list that keeps growing gives no list after 4 tries");
        Equal(null, Recovery.CompleteModuleList((IntPtr[] b, out int needed) => { needed = 20000 * IntPtr.Size; return true; }, m => "x.dll", IntPtr.Size), "modules: an absurd size gives no list");

        // The installer's record (start-confirm at logon, dwm-baseline.json as dwm-session.ps1 writes it) is one more
        // observer of the same epoch; this app only reads it.
        const string baseline = @"{""schema"":1,""records"":[" +
            @"{""boot_utc"":""2026-10-01T08:00:00.0000000Z"",""session"":1,""logon_utc"":""2026-10-01T08:01:00.0000000Z"",""dwm_pid"":700,""dwm_created_utc"":""2026-10-01T08:00:50.0000000Z"",""recorded_utc"":""2026-10-01T08:01:10.0000000Z"",""recorded_by"":""start-confirm""}," +
            @"{""boot_utc"":""2026-10-03T17:59:21.4000000Z"",""session"":1,""logon_utc"":""2026-10-03T18:00:31.2000000Z"",""dwm_pid"":900,""dwm_created_utc"":""2026-10-03T18:00:03.4567891Z"",""recorded_utc"":""2026-10-03T18:00:35.0000000Z"",""recorded_by"":""start-confirm""}]}";
        var records = Recovery.ParseBaseline(baseline);
        Equal(2, records.Count, "installer record: both records parsed");
        var fromInstaller = Recovery.FromBaseline(records, first);
        Check(fromInstaller != null && fromInstaller.Instances.Count == 1 && fromInstaller.Instances[0].Pid == 900, "installer record: the record of this epoch (boot and logon within 2 s) is used, the older one is not");
        var same = Closed(); same.DwmNow = first; same.DwmHistory = Recovery.Observe(new[] { fromInstaller }, first, "status", "2026-10-03T19:00:00.000Z");
        Equal("unknown-history", Recovery.DwmVerdict(same), "installer record + the same DWM (creation 0.46 s apart, WMI precision): unknown history");
        Check(same.DwmHistory.Instances.Count == 1 && same.DwmHistory.Instances[0].FirstSeenUtc == "2026-10-03T18:00:35.000Z" && same.DwmHistory.Instances[0].Observer == "start-confirm",
            "installer record: first seen = the record's time, by start-confirm");
        Check(Recovery.CompositorText(same).StartsWith("No replacement of the desktop compositor (DWM) seen since 2026-10-03 18:00:35 UTC (first seen by the installer's start-confirm task at logon, 35 s after the session began)."),
            "installer record: unknown history text names the record");
        var later = Reading(1234, "2026-10-03T18:40:00.000Z");
        var replaced = Closed(); replaced.DwmNow = later; replaced.DwmHistory = Recovery.Observe(new[] { Recovery.FromBaseline(records, later) }, later, "window", "2026-10-03T18:40:02.000Z");
        Equal("observed", Recovery.DwmVerdict(replaced), "installer record + a later, different DWM: observed");
        Check(Recovery.CompositorStatusLine(replaced).EndsWith("instances seen 2, watched since 2026-10-03T18:00:35.000Z by start-confirm)"), "installer record: observed status line names the record");
        var otherLogon = Reading(900, "2026-10-03T18:00:03.000Z"); otherLogon.LogonUtc = "2026-10-03T18:00:40.000Z";
        Equal(null, Recovery.FromBaseline(records, otherLogon), "installer record of another logon: ignored, not a replacement");
        var otherBoot = Reading(900, "2026-10-03T18:00:03.000Z"); otherBoot.BootUtc = "2026-10-03T17:59:30.000Z";
        Equal(null, Recovery.FromBaseline(records, otherBoot), "installer record of another boot: ignored");
        var otherSession = Reading(900, "2026-10-03T18:00:03.000Z"); otherSession.Session = 2;
        Equal(null, Recovery.FromBaseline(records, otherSession), "installer record of another session: ignored");
        var noLogon = Reading(900, "2026-10-03T18:00:03.000Z"); noLogon.LogonUtc = null;
        Equal(null, Recovery.FromBaseline(records, noLogon), "no logon time read: the installer record cannot be matched");
        Equal(0, Recovery.ParseBaseline("{not json").Count, "installer record: a damaged file gives no record");
        Equal(0, Recovery.ParseBaseline(@"{""schema"":2,""records"":[]}").Count, "installer record: another schema is ignored");
        Equal(1, Recovery.ParseBaseline(@"{""schema"":1,""records"":[{""session"":1},""x"",{""boot_utc"":""2026-10-03T17:59:21Z"",""session"":1,""logon_utc"":""bad"",""dwm_pid"":5,""dwm_created_utc"":""2026-10-03T18:00:03Z"",""recorded_utc"":""2026-10-03T18:00:35Z""}," +
            @"{""boot_utc"":""2026-10-03T17:59:21Z"",""session"":1,""logon_utc"":""2026-10-03T18:00:31Z"",""dwm_pid"":5,""dwm_created_utc"":""2026-10-03T18:00:03Z"",""recorded_utc"":""2026-10-03T18:00:35Z""}]}").Count,
            "installer record: damaged records are skipped, the good one kept");
        Equal(1, Recovery.ParseBaseline(@"{""schema"":1,""records"":{""boot_utc"":""2026-10-03T17:59:21Z"",""session"":1,""logon_utc"":""2026-10-03T18:00:31Z"",""dwm_pid"":5,""dwm_created_utc"":""2026-10-03T18:00:03Z"",""recorded_utc"":""2026-10-03T18:00:35Z""}}").Count,
            "installer record: a single record not in a list is read");

        // Malformed fields (reviewer 910): the record reads as no record, never as an observed replacement.
        const string good = @"""boot_utc"":""2026-10-03T17:59:21Z"",""session"":1,""logon_utc"":""2026-10-03T18:00:31Z"",""dwm_created_utc"":""2026-10-03T18:00:03Z"",""recorded_utc"":""2026-10-03T18:00:35Z""";
        var broken = new[]
        {
            new[] { "dwm_pid missing", "{" + good + "}" },
            new[] { "dwm_pid null", "{" + good + @",""dwm_pid"":null}" },
            new[] { "dwm_pid 0", "{" + good + @",""dwm_pid"":0}" },
            new[] { "dwm_pid negative", "{" + good + @",""dwm_pid"":-4}" },
            new[] { "dwm_pid as text", "{" + good + @",""dwm_pid"":""1234""}" },
            new[] { "dwm_pid fractional", "{" + good + @",""dwm_pid"":1234.5}" },
            new[] { "dwm_pid too large", "{" + good + @",""dwm_pid"":4294967296}" },
            new[] { "dwm_pid an object", "{" + good + @",""dwm_pid"":{""a"":1}}" },
            new[] { "session 0", "{" + good.Replace(@"""session"":1", @"""session"":0") + @",""dwm_pid"":1234}" },
            new[] { "boot_utc invalid", "{" + good.Replace("2026-10-03T17:59:21Z", "yesterday") + @",""dwm_pid"":1234}" },
            new[] { "logon_utc missing", "{" + good.Replace(@"""logon_utc"":""2026-10-03T18:00:31Z"",", "") + @",""dwm_pid"":1234}" },
            new[] { "dwm_created_utc invalid", "{" + good.Replace("2026-10-03T18:00:03Z", "2026-13-45T99:00:00Z") + @",""dwm_pid"":1234}" },
            new[] { "dwm_created_utc a number", "{" + good.Replace(@"""2026-10-03T18:00:03Z""", "5") + @",""dwm_pid"":1234}" },
            new[] { "recorded_utc empty", "{" + good.Replace(@"""recorded_utc"":""2026-10-03T18:00:35Z""", @"""recorded_utc"":""""") + @",""dwm_pid"":1234}" },
        };
        foreach (var b in broken)
        {
            var parsed = Recovery.ParseBaseline(@"{""schema"":1,""records"":[" + b[1] + "]}");
            Equal(0, parsed.Count, "installer record with " + b[0] + ": skipped");
            Equal(null, Recovery.FromBaseline(parsed, first), "installer record with " + b[0] + ": no record for this epoch");
            var h0 = Closed(); h0.DwmNow = first; h0.DwmHistory = Recovery.Observe(new[] { Recovery.FromBaseline(parsed, first) }, first, "status", "2026-10-03T19:00:00.000Z");
            Check(Recovery.DwmVerdict(h0) == "unknown-history" && h0.DwmHistory.Instances.Count == 1 && h0.DwmHistory.Instances[0].Observer == "status",
                "installer record with " + b[0] + ": unknown history from this reading only, never observed");
        }
        Equal(1, Recovery.ParseBaseline(@"{""schema"":1,""records"":[{" + good + @",""dwm_pid"":1234}]}").Count, "installer record: the well-formed control of the malformed cases is read");

        // A new session, a new logon in a reused session id, a new boot: each starts a new baseline.
        var other = Reading(5000, "2026-10-03T20:00:05.000Z"); other.Session = 2; other.SessionStartUtc = "2026-10-03T20:00:00.000Z";
        var ns = Recovery.Observe(new[] { r }, other, "status", "2026-10-03T20:01:00.000Z");
        Check(ns.Instances.Count == 1 && ns.Session == 2, "dwm: a new session starts a new baseline");
        var relogon = Reading(5000, "2026-10-03T21:00:05.000Z"); relogon.SessionStartUtc = "2026-10-03T21:00:00.000Z";
        Equal(1, Recovery.Observe(new[] { r }, relogon, "status", "2026-10-03T21:01:00.000Z").Instances.Count, "dwm: a new logon in the same session id starts a new baseline");
        var reboot = Reading(900, "2026-10-03T18:30:00.000Z"); reboot.BootId = 54;
        Equal(1, Recovery.Observe(new[] { r }, reboot, "status", "2026-10-03T22:00:00.000Z").Instances.Count, "dwm: a new boot starts a new baseline");
        Equal(null, Recovery.Observe(new[] { r }, new DwmReading { BootId = 53, Session = 1 }, "status", "2026-10-03T22:00:00.000Z"), "dwm: no DWM read, nothing recorded");
        var stale = Closed(); stale.DwmNow = reboot; stale.DwmHistory = r;
        Equal("unknown", Recovery.DwmVerdict(stale), "dwm: a record of another boot is not this session's history");

        var json = new JavaScriptSerializer();
        var back = json.Deserialize<DwmObservations>(json.Serialize(r));
        Check(back.Schema == 1 && back.BootId == 53 && back.Instances.Count == 2 && back.Instances[1].CreatedUtc == "2026-10-03T18:30:00.000Z", "dwm: record JSON round-trip");
        var snap = json.Deserialize<RecoverySnapshot>(json.Serialize(s));
        Equal("observed", Recovery.DwmVerdict(snap), "dwm: snapshot JSON keeps the reading and the history");
    }

    static StateLine line0(RecoverySnapshot s) { return Recovery.Describe(s).First(l => l.Topic == "Desktop compositor"); }

    // BD-060, statically: no source stops, kills or signals DWM except the operator escape, and none restarts Windows
    // other than through WindowsRestart (ExitWindowsEx without force). The Kill() calls allowed: the bug report's own
    // timed-out child tool, and StopCompositor of the escape, reached only from a plan with RestartCompositor, which
    // only restart-compositor with --accept-bd060 gives and the window never asks for.
    static void NoDwmRestart(string root)
    {
        var dir = Path.Combine(root, @"tools\win\amdgpu_wddm_control\src");
        var files = Directory.GetFiles(dir, "*.cs");
        Check(files.Length >= 10, "static check: the sources are found");
        int kills = 0;
        foreach (var f in files)
        {
            var name = Path.GetFileName(f);
            var text = File.ReadAllText(f);
            foreach (var bad in new[] { "taskkill", "shutdown.exe", "uxsms", "CloseMainWindow", "EWX_FORCE", "EwxForce", "InitiateSystemShutdown", "NtTerminateProcess", "DebugActiveProcess" })
                Check(!text.Contains(bad), "static check: " + name + " has no " + bad);
            var terminate = System.Text.RegularExpressions.Regex.Matches(text, @"TerminateProcess\(");
            if (name != "RecoveryActions.cs") Check(!text.Contains("TerminateProcess"), "static check: " + name + " has no TerminateProcess");
            foreach (var promise in new[] { "sign out", "Sign out", "sign-out", "log off", "Log off" })
                Check(!text.Contains(promise), "static check: " + name + " promises no sign-out remedy (" + promise + ")");
            var matches = System.Text.RegularExpressions.Regex.Matches(text, @"\.Kill\s*\(");
            int k = matches.Count;
            if (name == "BugReport.cs") Equal(1, k, "static check: BugReport.cs kills only its own timed-out child tool");
            else Equal(0, k, "static check: " + name + " calls no Kill()");
            if (name == "RecoveryActions.cs")
            {
                // The escape: one TerminateProcess call besides its declaration, inside StopCompositor, on the handle that
                // DwmHandle.Open checked; the terminate access is asked for nowhere else (reviewer 911).
                int from = text.IndexOf("static int StopCompositor(", StringComparison.Ordinal), to = text.IndexOf("static List<RecoveryProbe.Started> Starts(", StringComparison.Ordinal);
                Equal(2, terminate.Count, "static check: RecoveryActions.cs declares TerminateProcess and calls it once");
                Check(terminate.Count == 2 && text.Contains("public static extern bool TerminateProcess(SafeProcessHandle process, uint code);") &&
                    from > 0 && from < terminate[1].Index && terminate[1].Index < to && text.Substring(terminate[1].Index - 10, 10).EndsWith("DwmHandle."),
                    "static check: the TerminateProcess call is inside StopCompositor");
                var access = System.Text.RegularExpressions.Regex.Matches(text, @"DwmHandle\.Terminate\b");
                Check(access.Count == 1 && from < access[0].Index && access[0].Index < terminate[1].Index &&
                    text.Substring(from, to - from).Contains("using (var h = DwmHandle.Open(old, DwmHandle.QueryLimited | DwmHandle.Terminate | DwmHandle.Synchronize, out why))") &&
                    text.Substring(from, to - from).Contains("DwmHandle.TerminateProcess(h, 1)"), "static check: the escape terminates the handle it opened and checked");
                Check(text.Contains("if (plan.RestartCompositor) return StopCompositor(s);") && System.Text.RegularExpressions.Regex.Matches(text, @"StopCompositor\(").Count == 2,
                    "static check: StopCompositor is reached only from a RestartCompositor plan");
                int c0 = text.IndexOf("static DwmObservations Commit(", StringComparison.Ordinal), c1 = text.IndexOf("public static void ReadCompositor(", StringComparison.Ordinal);
                var commit = c0 > 0 && c1 > c0 ? text.Substring(c0, c1 - c0) : "";
                Check(commit.Contains("new Mutex(false, LockName(path))") && commit.Contains("WaitOne(2000)") && commit.Contains("Recovery.PlanCommit(Load(path, admin), history, now, observer, nowUtc)") &&
                    commit.Contains("File.Replace(temp, path, null, true)") && !commit.Contains(".partial"), "static check: observations are re-read, merged and replaced under one lock");
            }
            kills += k;
            // Every line naming the dwm process only reads it.
            foreach (var l in text.Split('\n').Where(x => x.Contains("\"dwm\"")))
                Check(l.Contains("GetProcessesByName(\"dwm\")"), "static check: " + name + " only reads the dwm process: " + l.Trim());
        }
        Equal(1, kills, "static check: one Kill() in all sources (the bug report's child tool)");
        var rules = File.ReadAllText(Path.Combine(dir, "Recovery.cs"));
        Check(rules.Contains("if (!operatorAccepted)") && System.Text.RegularExpressions.Regex.Matches(rules, @"RestartCompositor = true").Count == 1,
            "static check: only the accepted escape plans a DWM stop");
        foreach (var f in files)
            foreach (var l in File.ReadAllText(f).Split('\n').Where(x => x.Contains("InstallerBaseline") || x.Contains("dwm-baseline")))
                Check(!System.Text.RegularExpressions.Regex.IsMatch(l, @"Write|Move|Delete|Copy|Replace|Create"), "static check: the installer's dwm-baseline.json is only read: " + l.Trim());
        var form = File.ReadAllText(Path.Combine(dir, "MainForm.cs"));
        Check(!form.Contains("restart-compositor") && !form.Contains("OperatorEscape") && !form.Contains("--accept-bd060"), "static check: the window never offers the escape");
        Check(File.ReadAllText(Path.Combine(dir, "Native.cs")).Contains("ExitWindowsEx(EwxReboot, "), "static check: the restart is ExitWindowsEx EWX_REBOOT without force");
    }

    static int Main(string[] args)
    {
        if (args.Length != 1 && args.Length != 2) { Console.WriteLine("usage: unit-tests <repository root> [<start-confirm-core.ps1>]"); return 2; }
        var header = File.ReadAllText(Path.Combine(args[0], @"driver\kmd\bc250kmd_escape.h"));
        Strings.Directory = Path.Combine(args[0], @"tools\win\amdgpu_wddm_control\strings");
        StringTables();
        Replies(header);
        CuLayout(header);
        CuReasons(args[0]);
        CuFixtures();
        CuNativeAdapter();
        ShellTokens(args[0]);
        ProfileEditing();
        Dpm();
        DpmDesignDoc(args[0]);
        Redaction();
        Manifest();
        SettingsRule();
        NoDwmRestart(args[0]);
        RecoveryRules(args[0], header, args.Length == 2 ? args[1] : null);
        PlanAdditions(args[0]);
        GraphicsSettingsTests();
        TdrTests(args[0]);
        UmaTests(args[0]);
        TunerTests(args[0], header);
        TunerViewTests();
        FanTests(args[0], header);
        DriverCardTests();
        UpdateTests(args[0]);
        GuiTests(args[0]);
        OracleTests();
        if (args.Length == 2) Console.WriteLine("confirmation rule compared with " + args[1]);
        Console.WriteLine(_passed + " checks passed, " + _failed + " failed");
        return _failed == 0 ? 0 : 1;
    }
}
