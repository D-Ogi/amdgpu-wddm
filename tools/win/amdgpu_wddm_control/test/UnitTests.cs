// Host tests of the control application's pure parts: KMD reply parsing (with offsets computed from the header text
// of driver/kmd/bc250kmd_escape.h, so a moved field fails here and not on a tester's PC), profile editing, DPM setting
// checks and redaction. No driver, no registry. Usage: unit-tests.exe <repository root>
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
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
        var i = KmdReply.ParseInterop(b);
        Equal("GPU (2 devices on the GPU path)", KmdReply.CompositionText(i), "interop in use");
        Put(b, io["Effective"], 0u); Put(b, io["Flags"], 1u | 16u);
        Check(KmdReply.CompositionText(KmdReply.ParseInterop(b)).StartsWith("CPU (the driver closed"), "interop closed by the driver");
        Put(b, io["Flags"], 0u);
        Check(KmdReply.CompositionText(KmdReply.ParseInterop(b)).StartsWith("Not decided"), "interop not decided");

        var sh = Layout(header, "BC250_ESCAPE_START_HEALTH", out size);
        Equal(KmdReply.StartHealthBytes, size, "start health size from the header");
        b = new byte[KmdReply.StartHealthBytes];
        Put(b, sh["Magic"], KmdReply.Magic); Put(b, sh["Command"], 21u); Put(b, sh["AbiVersion"], 1u); Put(b, sh["Flags"], 15u);
        Put(b, sh["Generation"], 7UL); Put(b, sh["Completed"], 99UL); Put(b, sh["ReadyAgeMs"], 61000UL);
        var h = KmdReply.ParseStartHealth(b);
        Equal(7UL, h.Generation, "health generation"); Equal(99UL, h.Completed, "health completed"); Equal(61000UL, h.ReadyAgeMs, "health ready age");

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
            "MAC 00-1A-2B-3C-4D-5E and 00:1a:2b:3c:4d:5f",
            "Physical Address: 001A2B3C4D60",
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
        Check(!o.Contains("4D-5E") && !o.Contains("4d:5f") && !o.Contains("001A2B3C4D60"), "MAC addresses removed");
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
        Equal("CPU route (GPU route disabled, BD-058)", KmdReply.CompositionLine(1, interop, null), "router forces CPU");
        Equal("GPU (1 device on the GPU path)", KmdReply.CompositionLine(0, interop, null), "router on GPU, KMD decides");
        Equal("GPU (1 device on the GPU path)", KmdReply.CompositionLine(null, interop, null), "no router key");
        Equal("not loaded", KmdReply.CompositionLine(null, null, "not loaded"), "no interop reply");
    }

    static int Main(string[] args)
    {
        if (args.Length != 1) { Console.WriteLine("usage: unit-tests <repository root>"); return 2; }
        var header = File.ReadAllText(Path.Combine(args[0], @"driver\kmd\bc250kmd_escape.h"));
        Replies(header);
        ShellTokens(args[0]);
        ProfileEditing();
        Dpm();
        DpmDesignDoc(args[0]);
        Redaction();
        Manifest();
        Console.WriteLine(_passed + " checks passed, " + _failed + " failed");
        return _failed == 0 ? 0 : 1;
    }
}
