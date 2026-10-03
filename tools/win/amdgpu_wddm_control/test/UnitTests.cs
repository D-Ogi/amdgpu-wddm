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

static class UnitTests
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
                var n = Regex.Match(raw.Trim(), @"^(?<id>\w+)(\[(?<count>\d+)\])?$");
                if (!n.Success) return offsets;
                at = (at + width - 1) / width * width;
                offsets[n.Groups["id"].Value] = at;
                at += width * (n.Groups["count"].Success ? int.Parse(n.Groups["count"].Value) : 1);
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
        Equal(KmdReply.DpmBytes, size, "DPM size from the header");
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
        var asked = new HashSet<string>();
        foreach (var file in Directory.GetFiles(Path.Combine(root, @"driver\umd\d3d12"), "*.cpp").Where(f => !f.EndsWith("-test.cpp")))
            foreach (Match m in Regex.Matches(File.ReadAllText(file), "ddi_experiment\\(\"([a-z0-9-]+)\"\\)"))
                asked.Add(m.Groups[1].Value);
        foreach (var s in Profiles.Catalog) Check(asked.Contains(s.Token), "catalog switch " + s.Token + " is read by the shell");
        foreach (var t in asked) Check(Profiles.Find(t) != null, "shell switch " + t + " is in the catalog");
        var trace = File.ReadAllText(Path.Combine(root, @"driver\umd\d3d12\ddi-trace.h"));
        Check(trace.Contains(@"SOFTWARE\\amdgpu-wddm\\D3D12\\Applications\\"), "profile key path matches the shell");
        Check(trace.Contains("\"Experiment\""), "profile value name matches the shell");
    }

    static void ProfileEditing()
    {
        var p = Profiles.Parse("present-noprimary,present-cached,raytracing-tier,recording-bind,retire-handoff,deferred-replay");
        Equal(6, p.Known.Count, "witcher3 profile known"); Equal(0, p.Unknown.Count, "witcher3 profile unknown");
        Equal("raytracing-tier", p.Known[0], "catalog order");
        p = Profiles.Parse("future-switch,raytracing-tier,raytracing-tier,,none");
        Equal(1, p.Known.Count, "duplicates, empty and none dropped"); Equal("future-switch", p.Unknown.Single(), "unknown kept");
        Equal("raytracing-tier,deferred-replay,future-switch", Profiles.Compose(new[] { "deferred-replay", "raytracing-tier" }, p.Unknown), "compose order");
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
        Check(p.OfferRestart && p.Undoable && !p.RestartDwm && p.Effect.Contains("next restart"), "reopen: next restart, undoable, no DWM restart");
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
        Check(!p.Refused && Writes(p, "DwmForceCpu=0") && p.RestartDwm && p.WatchDwm && p.DwmForceCpuAfter == 0 && p.Undoable, "GPU route: DwmForceCpu 0, DWM restart, watched");
        var onGpu = Open(); onGpu.DwmForceCpu = 0; onGpu.DwmRoute = "gpu";
        Check(Recovery.Plan("desktop-gpu", onGpu).Refusal.Contains("already"), "GPU route refused when DWM is on it already");
        onGpu.DwmRoute = "unknown";
        Check(!Recovery.Plan("desktop-gpu", onGpu).Refused, "GPU route allowed when the running route is unknown (restart re-checks)");
        p = Recovery.Plan("desktop-cpu", Closed());
        Check(Recovery.Plan("desktop-cpu", Closed()).Refusal.Contains("already"), "CPU route refused when DWM is on it already");
        var cpuUnknown = Closed(); cpuUnknown.DwmRoute = "unknown";
        p = Recovery.Plan("desktop-cpu", cpuUnknown);
        Check(!p.Refused && Writes(p, "DwmForceCpu=1") && p.RestartDwm && !p.WatchDwm && p.DwmForceCpuAfter == 1, "CPU route: DwmForceCpu 1, DWM restart, not watched");
        Check(!Recovery.Plan("desktop-cpu", onGpu).Refused, "CPU route allowed from the GPU route");

        // 3. Confirm.
        p = Recovery.Plan("confirm-start", Closed());
        Check(!p.Refused && p.ConfirmStart && p.Writes.Count == 0 && !p.Undoable, "confirm: the escape only, not undoable");
        var c = Closed(); c.Health.Flags = 15;
        Check(Recovery.Plan("confirm-start", c).Refusal.Contains("confirmed already"), "confirm refused when confirmed");
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
            new BackupRecord { File = file, Action = action, Utc = "2026-10-03T" + file.Substring(16, 6), Undoable = undoable, Undoes = undoes, Values = values.ToList(), RestartsDwm = action.StartsWith("desktop") };
        var b1 = rec("backup-20260103T100000000Z.json", "reopen-gpu-path", true, null, new[]
        {
            new BackupValue { Path = Recovery.ParametersPath, Name = "EnableGpuPresentBlit", Existed = true, Kind = "DWord", Number = 0 },
            new BackupValue { Path = Recovery.ParametersPath, Name = "InteropClosedReason", Existed = true, Kind = "DWord", Number = 4 },
            new BackupValue { Path = Recovery.ParametersPath, Name = "EnableCddDwmInterop", Existed = false },
        });
        var b2 = rec("backup-20260103T110000000Z.json", "confirm-start", false, null, new BackupValue[0]);
        p = Recovery.Plan("undo", Closed(), null, null, new[] { b1, b2 });
        Check(!p.Refused && p.UndoOf == b1.File && Writes(p, "EnableGpuPresentBlit=0", "InteropClosedReason=4", "EnableCddDwmInterop-"), "undo restores the newest undoable backup, deletes what was absent");
        Check(!p.Undoable && p.OfferRestart && !p.RestartDwm, "undo: not undoable itself, restart offered");
        var u1 = rec("backup-20260103T120000000Z.json", "undo", false, b1.File, new BackupValue[0]);
        Check(Recovery.Plan("undo", Closed(), null, null, new[] { b1, b2, u1 }).Refused, "an undone backup is not undone twice");
        var b3 = rec("backup-20260103T130000000Z.json", "desktop-cpu", true, null, new[] { new BackupValue { Path = Recovery.RouterPath, Name = "DwmForceCpu", Existed = true, Kind = "DWord", Number = 0 } });
        Equal(b3.File, Recovery.UndoTarget(new[] { b1, b2, u1, b3 }).File, "newest undoable wins");
        Check(Recovery.Plan("undo", Closed(), null, null, new[] { b3 }).Refusal.Contains("GPU desktop path is closed"), "undo onto the GPU route refused while the switches are closed");
        p = Recovery.Plan("undo", Open(), null, null, new[] { b3 });
        Check(!p.Refused && p.RestartDwm && p.WatchDwm && p.DwmForceCpuAfter == 0, "undo onto the GPU route restarts DWM with the watchdog");
        var evil = rec("backup-20260103T140000000Z.json", "reopen-gpu-path", true, null, new[] { new BackupValue { Path = Recovery.ParametersPath, Name = "UnconfirmedStarts", Existed = true, Kind = "DWord", Number = 2 } });
        Check(Recovery.Plan("undo", Closed(), null, null, new[] { evil }).Refusal.Contains("does not change"), "undo refuses a backup outside the allow-list");
        evil = rec("backup-20260103T150000000Z.json", "reopen-gpu-path", true, null, new[] { new BackupValue { Path = @"SYSTEM\CurrentControlSet\Control\CI\Policy", Name = "DwmForceCpu", Existed = false } });
        Check(Recovery.Plan("undo", Closed(), null, null, new[] { evil }).Refused, "undo refuses a backup of another key");

        // Every allowed plan writes only allowed values.
        foreach (var snap in new[] { Closed(), Open(), cpuUnknown, onGpu })
            foreach (var a in Recovery.Actions)
            {
                var plan = Recovery.Plan(a, snap, 1, 1600, new[] { b1, b3 });
                Check(plan.Writes.All(w => Recovery.Allowed(w.Path, w.Name)), a + " writes only allowed values");
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
        Check(line(lines, "Desktop composition").Action == "desktop-cpu" && line(lines, "GPU desktop path").Severity == "ok", "GPU route offers the way back");
        var gpuClosed = Closed(); gpuClosed.DwmForceCpu = 0;
        Check(line(Recovery.Describe(gpuClosed), "Desktop composition").Text.Contains("stays on the CPU route"), "GPU route selected with closed switches explained");
        var refused = Closed(); refused.Interop = null; refused.Health = null; refused.Dpm = null; refused.DriverError = "not loaded";
        refused.Parameters["UnconfirmedStarts"] = 2; refused.Parameters["LastStage"] = 90;
        Check(line(Recovery.Describe(refused), "Driver start").Text.Contains("refused to start") && line(Recovery.Describe(refused), "Driver start").Text.Contains("installer"), "guard refusal explained, installer named");
        var early = Closed(); early.Health.ReadyAgeMs = 1000;
        Check(line(Recovery.Describe(early), "Driver start").Action == null, "an early start recommends waiting");
        var trial = Open(); trial.Parameters["DpmMode"] = 1; trial.Parameters["DpmPending"] = 0x10005DC; trial.Dpm = new DpmState { Mode = 1, MaxMHz = 1500 };
        Check(line(Recovery.Describe(trial), "Clock control").Action == "confirm-start", "clocks on trial recommend the confirmation");
        Equal(1, Recovery.Describe(new RecoverySnapshot()).Count, "not installed: one line");

        // The watchdog.
        Equal(null, Recovery.WatchVerdict(null, new int[0], 0, 5, 60), "watch: waiting for DWM");
        Check(Recovery.WatchVerdict(null, new int[0], 0, 20, 60).StartsWith("no DWM"), "watch: no DWM in 20 s fails");
        Equal(null, Recovery.WatchVerdict(100, new[] { 100 }, 0, 30, 60), "watch: holding");
        Equal("ok", Recovery.WatchVerdict(100, new[] { 100 }, 0, 60, 60), "watch: held 60 s");
        Check(Recovery.WatchVerdict(100, new[] { 100 }, 1, 30, 60).Contains("Application Error 1000"), "watch: a dwm.exe crash fails");
        Check(Recovery.WatchVerdict(100, new[] { 104 }, 0, 30, 60).Contains("replaced"), "watch: a replaced DWM fails");
        Check(Recovery.WatchVerdict(100, new[] { 100, 104 }, 0, 30, 60).Contains("second"), "watch: a second DWM fails");
        Equal("ok", Recovery.WatchVerdict(100, new[] { 100 }, 0, 5, 5), "settle: 5 s on the CPU route");

        Equal("gpu", Recovery.RouteFromModules(new[] { @"C:\Program Files\amdgpu-wddm\desktop\bc250d3d_router.dll", @"C:\Program Files\amdgpu-wddm\desktop\bc250d3d_zink.dll" }), "route from zink");
        Equal("gpu", Recovery.RouteFromModules(new[] { @"C:\Program Files\amdgpu-wddm\desktop\amdgpu_wddm_radv.dll" }), "route from the desktop RADV");
        Equal("unknown", Recovery.RouteFromModules(new[] { @"C:\Program Files\amdgpu-wddm\d3d12\amdgpu_wddm_radv.dll" }), "the D3D12 RADV is not the desktop");
        Equal("cpu", Recovery.RouteFromModules(new[] { @"C:\Program Files\amdgpu-wddm\desktop\bc250d3d_router.dll", @"C:\Program Files\amdgpu-wddm\desktop\bc250d3d.dll" }), "route from the CPU UMD");
        Equal("unknown", Recovery.RouteFromModules(null), "no modules");

        Equal("backup-20261003T101112013Z.json", Recovery.BackupFileName(new DateTime(2026, 10, 3, 10, 11, 12, 13, DateTimeKind.Utc)), "backup file name");
        Check(Recovery.TaskResultText(0).Contains("confirmed") && Recovery.TaskResultText(0x41303).Contains("not run"), "task result texts");

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
        var w = Profiles.PlanWrite("a.exe", "raytracing-tier", none);
        Equal(ProfileWriteKind.Delete, w.Kind, "profile: checked, saved, then unchecked: the key is removed");
        Equal(ProfileWriteKind.Delete, Profiles.PlanWrite("a.exe", "", none).Kind, "profile: an empty stored value is removed, not kept");
        w = Profiles.PlanWrite("a.exe", "raytracing-tier,x-future", new[] { "raytracing-tier" });
        Check(w.Kind == ProfileWriteKind.Set && w.Value == "raytracing-tier", "profile: unchecking one name removes exactly that name");
        w = Profiles.PlanWrite("a.exe", null, new[] { "deferred-replay", "x-future", "raytracing-tier" });
        Check(w.Kind == ProfileWriteKind.Set && w.Value == "raytracing-tier,deferred-replay,x-future", "profile: the exact checked set is written, catalog order first");
        Equal(ProfileWriteKind.None, Profiles.PlanWrite("a.exe", "raytracing-tier,deferred-replay", new[] { "deferred-replay", "raytracing-tier" }).Kind, "profile: same set: nothing written");
        const string witcher = "present-noprimary,present-cached,raytracing-tier,recording-bind,retire-handoff,deferred-replay";
        var parsed = Profiles.Parse(witcher);
        Equal(6, parsed.Known.Count, "installer profile: every name shows as checked");
        Equal(ProfileWriteKind.None, Profiles.PlanWrite("witcher3.exe", witcher, parsed.Known.Concat(parsed.Unknown)).Kind, "installer profile in its own order: not a change");
        w = Profiles.PlanWrite("witcher3.exe", witcher, parsed.Known.Where(n => n != "deferred-replay"));
        Check(w.Kind == ProfileWriteKind.Set && !w.Value.Contains("deferred-replay") && w.Value.Split(',').Length == 5, "installer profile: one unchecked name is removed");
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

    static int Main(string[] args)
    {
        if (args.Length != 1 && args.Length != 2) { Console.WriteLine("usage: unit-tests <repository root> [<start-confirm-core.ps1>]"); return 2; }
        var header = File.ReadAllText(Path.Combine(args[0], @"driver\kmd\bc250kmd_escape.h"));
        Replies(header);
        ShellTokens(args[0]);
        ProfileEditing();
        Dpm();
        DpmDesignDoc(args[0]);
        Redaction();
        Manifest();
        SettingsRule();
        RecoveryRules(args[0], header, args.Length == 2 ? args[1] : null);
        if (args.Length == 2) Console.WriteLine("confirmation rule compared with " + args[1]);
        Console.WriteLine(_passed + " checks passed, " + _failed + " failed");
        return _failed == 0 ? 0 : 1;
    }
}
