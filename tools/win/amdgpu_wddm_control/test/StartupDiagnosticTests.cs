using System;
using System.IO;
using System.Text;
using AmdgpuWddmControl;

static partial class UnitTests
{
    static void StartupDiagnosticTests()
    {
        Check(!StartupDiagnosticData.Needed(false, false, 0, false), "no report on a PC with no installed driver");
        // How the report prints one value of a record. The numbers a reader compares by eye - the stage the
        // start reached, the stage of the stop, the health flags, the number of this boot and the count of
        // kept log lines - are decimal, like the installer's reader, the driver's own log, the lab arms and
        // the boot number this same report prints a few lines under the records. Only the NTSTATUS and the
        // escape ABI word read as hexadecimal words.
        Equal("79", StartupDiagnosticData.Field("Stage", 79), "Stage is decimal");
        Equal("39", StartupDiagnosticData.Field("StartStage", 39), "StartStage is decimal");
        Equal("3", StartupDiagnosticData.Field("HealthFlags", 3), "HealthFlags is decimal");
        Equal("1234", StartupDiagnosticData.Field("BootId", 1234), "BootId is decimal, so it compares with this boot's number");
        // BD-130: the number this report prints as "boot ID" is compared by eye with the BootId of the records
        // above, which the driver writes from KUSER_SHARED_DATA. It must come from that same page, at the offset
        // the WDK header fixes, and not from the PrefetchParameters copy of the counter, which lags inside a boot
        // (the lab, 22631, 13 minutes after the boot: page 388, registry 387, and the fresh record of that boot
        // was reported as an earlier boot's).
        string bootSource;
        var sharedBoot = DriverCardProbe.SharedBootId(out bootSource);
        uint pageBoot = (uint)System.Runtime.InteropServices.Marshal.ReadInt32(new IntPtr(0x7FFE02C4));
        Check(pageBoot != 0, "this host maps KUSER_SHARED_DATA and its BootId is a number (" + pageBoot + ")");
        Check(sharedBoot == pageBoot, "the reader of this boot's number reads that page, not the registry");
        Equal("KUSER_SHARED_DATA", bootSource, "and names the source it used, because the registry fallback is the weaker answer");
        var registryBoot = DriverCardProbe.BootId();
        Check(registryBoot != null, "the registry copy is still readable, for the records this application writes itself");
        Equal("64", StartupDiagnosticData.Field("LogLines", 64), "LogLines is decimal");
        Equal("2", StartupDiagnosticData.Field("UnconfirmedStarts", 2), "UnconfirmedStarts is decimal");
        Equal("0x000700D8", StartupDiagnosticData.Field("Version", 0x000700D8), "the escape ABI word stays a hexadecimal word");
        Equal("0xC0000022", StartupDiagnosticData.Field("Status", unchecked((int)0xC0000022)), "an NTSTATUS stays a hexadecimal word");
        Equal("0x00000000", StartupDiagnosticData.Field("Status", 0), "a zero NTSTATUS reads as a word too, and the stop record's own text says it is no verdict");
        Equal("18446744073709551610", StartupDiagnosticData.Field("Generation", "18446744073709551610"), "a 64-bit decimal string is passed through");
        Equal("a\nb", StartupDiagnosticData.Field("Log", new[] { "a", "b" }), "the kept log lines join with one line feed each");
        Check(StartupDiagnosticData.Needed(true, true, 43, true), "Code 43 overrides old confirmation");
        Check(StartupDiagnosticData.Needed(true, true, 0, false), "unconfirmed start is collected");
        Check(!StartupDiagnosticData.Needed(true, true, 0, true), "healthy confirmed start is quiet");
        var text = "A😀ąć日本語";
        for (int n = 0; n < 35; n++)
        {
            var cut = StartupDiagnosticData.Fit(text, n);
            Check(Encoding.UTF8.GetByteCount(cut) <= n, "encoded byte bound " + n);
            Check(cut.Length == 0 || !char.IsHighSurrogate(cut[cut.Length - 1]), "no split surrogate " + n);
        }
        var setup = ">>>  [foreign]\nPCI\\VEN_1234&DEV_5678\nprivate foreign section\n<<<  [Exit status]\n" +
            ">>>  [Device Install - pci\\ven_1002&dev_13fe&subsys]\n>>> Section start\nfailed test install\n<<< Section end\n<<< [Exit status]\n" +
            ">>> [unrelated]\nother disk details\n";
        var selected = StartupDiagnosticData.SetupSections(setup);
        Check(selected.Contains("failed test install") && selected.Contains("Section end"), "complete matching SetupAPI section");
        Check(!selected.Contains("private foreign") && !selected.Contains("other disk"), "unrelated sections omitted");
        Check(StartupDiagnosticData.SetupSections("truncated header\n" + StartupDiagnosticData.DeviceId).Contains("No matching"), "do not misattribute headless tail");
        long ms = 0;
        var writer = new StringWriter();
        var d = new StartupDiagnosticData(writer, new Redactor("Alice", "LABPC", @"C:\Users\Alice"), () => ms);
        d.Source("fixture", () => d.Add("Alice LABPC email=a@b.test MAC=12:34:56:78:90:AB\n"));
        Check(!writer.ToString().Contains("Alice") && !writer.ToString().Contains("LABPC") && !writer.ToString().Contains("12:34:"), "redaction before output");
        d.Add("C:\\Users\\OtherPerson\\driver.log <Computer>other-machine</Computer> S-1-5-21-123456-789-1001\n");
        Check(!writer.ToString().Contains("OtherPerson") && !writer.ToString().Contains("other-machine") && !writer.ToString().Contains("S-1-5-21"), "events and other profile paths redacted");
        d.Source("failed", () => { throw new IOException("file missing"); });
        Check(writer.ToString().Contains("Unavailable: IOException"), "source failure stays explicit");
        ms = StartupDiagnosticData.TimeoutMs;
        int before = writer.ToString().Length;
        d.Source("must not run", () => { throw new Exception("ran after timeout"); });
        Equal(before, writer.ToString().Length, "no sources started after deadline");
        ms = 0;
        for (int i = 0; i < 40; i++) d.Add(new string('語', 200000));
        Check(d.Bytes < StartupDiagnosticData.Limit && Encoding.UTF8.GetByteCount(writer.ToString()) == d.Bytes, "aggregate 4 MiB bound after encoding");
        string dir = Path.Combine(Path.GetTempPath(), "startup-diag-tests");
        Directory.CreateDirectory(dir);
        string file = Path.Combine(dir, "setup-fixture.log");
        File.WriteAllText(file, setup, Encoding.Unicode);
        Equal(setup, StartupDiagnosticData.Tail(file, 1024 * 1024), "UTF16 BOM reader");
        Check(Encoding.UTF8.GetByteCount(StartupDiagnosticData.Tail(file, 31)) < 100, "bounded odd UTF16 tail");
        File.WriteAllText(file, setup, new UTF8Encoding(false));
        Equal(setup, StartupDiagnosticData.Tail(file, 1024 * 1024), "UTF8 reader");
        var clock = System.Diagnostics.Stopwatch.StartNew();
        var output = DiagnosticWorker.Run(new System.Diagnostics.ProcessStartInfo(
            System.Reflection.Assembly.GetExecutingAssembly().Location, "--diagnostic-worker-fixture"), 1500);
        Check(clock.ElapsedMilliseconds < 3000, "blocked provider is bounded by worker deadline");
        Check(output.Contains("fixture-ready") && output.Contains("report is partial"), "partial worker output retained on timeout");
        var pid = System.Text.RegularExpressions.Regex.Match(output, @"fixture-ready (\d+)");
        bool alive = false;
        if (pid.Success)
        {
            try { using (var p = System.Diagnostics.Process.GetProcessById(int.Parse(pid.Groups[1].Value))) alive = !p.HasExited; }
            catch (ArgumentException) { }
        }
        Check(pid.Success && !alive, "deadline terminates the actual worker, not only its waiter");
    }
}
