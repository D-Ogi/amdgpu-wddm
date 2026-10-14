using System;
using System.IO;
using System.Text;
using AmdgpuWddmControl;

static partial class UnitTests
{
    static void StartupDiagnosticTests(string root)
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
        // The fixture prints its pid and then blocks, so Run always reaches its deadline. What the deadline has
        // to be longer than is this PC starting a second managed process and draining one line of its output,
        // and that is not bounded by anything: in the full 24-worker quality suite of train b34 a 1500 ms
        // deadline twice killed the fixture before it printed, and the same two checks passed at once on the
        // idle machine. So the start is measured first, under its own bound of 60 s, by one start of the same
        // fixture that this test reads and kills itself, and the deadline under test is 1500 ms or three of
        // those starts, whichever is longer. A start that takes longer than 60 s fails here and says so; a
        // deadline path that is slow fails the bound below, which no retry can hide. What is under test is the
        // deadline's effect: partial output is kept, the elapsed time stays near the deadline, and the worker
        // process is really killed.
        string fixture = System.Reflection.Assembly.GetExecutingAssembly().Location;
        long startMs;
        string first = null;
        var startClock = System.Diagnostics.Stopwatch.StartNew();
        using (var probe = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(fixture, "--diagnostic-worker-fixture")
            { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, StandardOutputEncoding = Encoding.UTF8 }))
        {
            var line = probe.StandardOutput.ReadLineAsync();
            if (line.Wait(60000)) first = line.Result;
            startMs = startClock.ElapsedMilliseconds;
            try { probe.Kill(); } catch (InvalidOperationException) { } catch (System.ComponentModel.Win32Exception) { }
            probe.WaitForExit(10000);
        }
        Check(first != null && first.StartsWith("fixture-ready "), "worker fixture starts and prints within 60 s");
        int deadlineMs = (int)Math.Max(1500, 3 * startMs);
        if (deadlineMs > 1500) Console.WriteLine("  worker fixture start took " + startMs + " ms; deadline under test " + deadlineMs + " ms");
        var clock = System.Diagnostics.Stopwatch.StartNew();
        string output = DiagnosticWorker.Run(new System.Diagnostics.ProcessStartInfo(fixture, "--diagnostic-worker-fixture"), deadlineMs);
        long elapsedMs = clock.ElapsedMilliseconds;
        Check(elapsedMs < deadlineMs * 2, "blocked provider is bounded by worker deadline");
        Check(output.Contains("fixture-ready") && output.Contains("report is partial"), "partial worker output retained on timeout");
        if (!output.Contains("fixture-ready") || !output.Contains("report is partial"))
            Console.WriteLine("  worker fixture result: " + output.Replace("\r", "").Replace("\n", " | "));
        var pid = System.Text.RegularExpressions.Regex.Match(output, @"fixture-ready (\d+)");
        bool alive = false;
        if (pid.Success)
        {
            try { using (var p = System.Diagnostics.Process.GetProcessById(int.Parse(pid.Groups[1].Value))) alive = !p.HasExited; }
            catch (ArgumentException) { }
        }
        Check(pid.Success && !alive, "deadline terminates the actual worker, not only its waiter");
        WorkerRunTests(root, dir);
    }

    // BD-142: tester.29's reports held one line, "Worker exit -532462766." (0xE0434352, an unhandled .NET exception):
    // the worker set Console.OutputEncoding, whose setter calls SetConsoleOutputCP and throws IOException in a process
    // without a console, before it wrote anything. Now the worker writes to its output stream itself, every probe's
    // exception is a line of the report, and an exception outside the probes is the report's last lines.
    static void WorkerRunTests(string root, string dir)
    {
        var redactor = new Redactor("Alice", "LABPC", @"C:\Users\Alice");
        string held = Path.Combine(dir, "held-open.log");
        File.WriteAllText(held, "a log another program holds open\n");
        string report;
        int code;
        using (new FileStream(held, FileMode.Open, FileAccess.ReadWrite, FileShare.None))
        using (var pipe = new MemoryStream())
        {
            code = StartupDiagnosticData.RunWorker(pipe, redactor, () => 0, data =>
            {
                data.Add("Automatic start/stop diagnostics\n");
                data.Source("first source", () => data.Add("first source ran for Alice\n"));
                data.Source("held file", () => data.Add(StartupDiagnosticData.Tail(held, 1024)));
                data.Source("third source", () => data.Add("third source ran\n"));
                data.Add("\nCollection finished.\n");
            });
            report = new UTF8Encoding(false, true).GetString(pipe.ToArray());
        }
        Equal(0, code, "BD-142: a probe that throws is a line of the report, not the worker's exit");
        Check(report.Contains("=== first source ===\nfirst source ran") && report.Contains("=== third source ===\nthird source ran") && report.Contains("Collection finished."),
            "BD-142: the probes before and after the failing one are in the report, and the collection finished");
        // The message and the word before each stack frame ("at", "w", ...) are in the language of Windows; the type,
        // the HRESULT and the method names are not.
        Check(report.Contains("=== held file ===\nUnavailable: IOException: "), "BD-142: the failing probe's line names the exception's type and message");
        Check(report.Contains("type System.IO.IOException, HRESULT 0x80070020"), "BD-142: and its full type and HRESULT (0x80070020, a sharing violation)");
        Check(report.Contains(" AmdgpuWddmControl.StartupDiagnosticData.Tail(") && report.Contains(" System.IO.FileStream..ctor("), "BD-142: and its stack, down to the probe that threw");
        Check(!report.Contains("Alice") && !report.Contains("\uFEFF"), "BD-142: the worker's text is redacted UTF-8 without a byte order mark");

        using (var pipe = new MemoryStream())
        {
            code = StartupDiagnosticData.RunWorker(pipe, redactor, () => 0, data =>
            {
                data.Source("first source", () => data.Add("first source ran\n"));
                throw new InvalidOperationException("outside every source");
            });
            report = Encoding.UTF8.GetString(pipe.ToArray());
        }
        Equal(StartupDiagnosticData.WorkerFaulted, code, "BD-142: an exception outside the probes is the worker's exit 3, not an unhandled exception");
        Check(report.Contains("first source ran") && report.Contains("=== The collection stopped ===") && report.Contains("InvalidOperationException: outside every source"),
            "BD-142: and its type and message are the report's last lines");
        Check(report.Contains("type System.InvalidOperationException, HRESULT 0x80131509") && report.Contains("WorkerRunTests") && report.Contains(" AmdgpuWddmControl.StartupDiagnosticData.RunWorker("),
            "BD-142: with its HRESULT and its stack");

        long late = StartupDiagnosticData.TimeoutMs;
        using (var pipe = new MemoryStream())
        {
            code = StartupDiagnosticData.RunWorker(pipe, redactor, () => late, data => { data.Add("skipped after the deadline\n"); throw new TimeoutException("late"); });
            report = Encoding.UTF8.GetString(pipe.ToArray());
        }
        Check(code == StartupDiagnosticData.WorkerFaulted && !report.Contains("skipped") && report.Contains("TimeoutException: late"), "BD-142: the stop lines are written after the deadline too");

        code = StartupDiagnosticData.RunWorker(new BrokenPipe(), redactor, () => 0, data =>
        {
            data.Source("first source", () => data.Add("lost\n"));
            Check(data.OutputLost && !data.Active, "BD-142: a write to a closed pipe marks the output lost and stops the collection");
            data.Source("second source", () => { throw new Exception("must not run"); });
        });
        Equal(StartupDiagnosticData.WorkerOutputLost, code, "BD-142: a closed pipe is exit 4, not an exception");
        Equal(StartupDiagnosticData.WorkerOutputLost, StartupDiagnosticData.RunWorker(new MemoryStream(new byte[0], false), redactor, () => 0, data => data.Add("x")),
            "BD-142: an output that cannot be written at all is exit 4");

        Equal("Worker exit 0", DiagnosticWorker.ExitText(0), "BD-142: a finished worker reads as before");
        Check(DiagnosticWorker.ExitText(-532462766).StartsWith("Worker exit -532462766 (0xE0434352: ") && DiagnosticWorker.ExitText(-532462766).Contains("Application event log"),
            "BD-142: the tester.29 exit says what it is and where its record is");
        Check(DiagnosticWorker.ExitText(3).Contains("lines above") && DiagnosticWorker.ExitText(4).Contains("could not write") && DiagnosticWorker.ExitText(7) == "Worker exit 7 (0x00000007)",
            "BD-142: the worker's own exit codes are explained");
        var missing = DiagnosticWorker.Run(new System.Diagnostics.ProcessStartInfo(Path.Combine(dir, "no-such-worker.exe")), 2000);
        Check(missing.StartsWith("The diagnostics worker could not start: Win32Exception: "), "BD-142: a worker that cannot start is a line of the report, not an exception of the window");

        // No source of the application sets the console's code pages: the exe is a Windows-subsystem program, and the
        // worker and the other entries run without a console.
        var src = Path.Combine(root, @"tools\win\amdgpu_wddm_control\src");
        foreach (var file in Directory.GetFiles(src, "*.cs"))
        {
            var text = File.ReadAllText(file);
            Check(!System.Text.RegularExpressions.Regex.IsMatch(text, @"Console\.(OutputEncoding|InputEncoding)\s*="), "BD-142: " + Path.GetFileName(file) + " does not set the console's encoding");
        }
    }

    sealed class BrokenPipe : Stream
    {
        public override bool CanRead { get { return false; } }
        public override bool CanSeek { get { return false; } }
        public override bool CanWrite { get { return true; } }
        public override long Length { get { throw new NotSupportedException(); } }
        public override long Position { get { throw new NotSupportedException(); } set { throw new NotSupportedException(); } }
        public override void Flush() { }
        public override int Read(byte[] buffer, int offset, int count) { throw new NotSupportedException(); }
        public override long Seek(long offset, SeekOrigin origin) { throw new NotSupportedException(); }
        public override void SetLength(long value) { throw new NotSupportedException(); }
        public override void Write(byte[] buffer, int offset, int count) { throw new IOException("The pipe is being closed.", unchecked((int)0x800700E8)); }
    }
}
