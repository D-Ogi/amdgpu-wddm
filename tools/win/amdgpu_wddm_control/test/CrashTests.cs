// BD-174: the record a process-fatal exception leaves behind, and the background work items that no longer take
// the window with them. The window had vanished once with no event anywhere (see Crash.cs for the exit paths that
// were read), so what matters here is that the record is written whatever the exception does, and that a failing
// work item ends in the record instead of in silence.
using System;
using System.Threading;
using AmdgpuWddmControl;

static partial class UnitTests
{
    // An exception that fights back: its Message throws, as a badly written exception of a foreign library can.
    sealed class NastyException : Exception
    {
        public override string Message { get { throw new InvalidOperationException("no message for you"); } }
    }

    static void CrashTests(string root)
    {
        // Below the build's own temporary directory: no test run writes into a real profile.
        Crash.DirectoryForTests = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "crash-" + Guid.NewGuid().ToString("N"));
        var utc = new DateTime(2026, 10, 14, 6, 30, 43, DateTimeKind.Utc);
        var record = Crash.Record("map cell click", new InvalidOperationException("the control is disposed"), utc, "0.7.0.0");
        Check(record.Contains("2026-10-14T06:30:43Z") && record.Contains("map cell click") && record.Contains("0.7.0.0") &&
            record.Contains("System.InvalidOperationException: the control is disposed"),
            "the record names the moment, the place, the build and the exception: " + record);

        var inner = new ArgumentNullException("prefs");
        record = Crash.Record("wallpaper at start", new ApplicationException("outer", inner), utc, null);
        Check(record.Contains("System.ApplicationException: outer") && record.Contains("System.ArgumentNullException"),
            "an inner exception is in the record as well: " + record);

        record = Crash.Record(null, null, utc, null);
        Check(record.Contains("unknown") && record.Contains("no exception object"), "a record is written even with nothing to report: " + record);

        record = Crash.Record("nasty", new NastyException(), utc, null);
        Check(record.Contains("UnitTests+NastyException") && record.Contains("the message could not be read"),
            "an exception whose message throws still leaves a record: " + record);

        var long80 = new string('x', 80000);
        record = Crash.Record("long", new InvalidOperationException(long80), utc, null);
        Check(record.Length <= Crash.MaxRecordChars + 16 && record.EndsWith("[cut]" + Environment.NewLine),
            "a record of a runaway exception is cut, not kept whole (" + record.Length + " chars)");

        // The file: one record written here, and then the record of a background work item that threw. The window
        // lives on through that failure, which is the whole point of Crash.Background.
        Crash.Write("test write", new InvalidOperationException("boom"));
        Check(ReadShared(Crash.LogPath()).Contains("test write"), "the record reaches " + Crash.LogPath());
        using (var done = new ManualResetEventSlim(false))
        {
            Crash.Background("test work item", () => { throw new InvalidOperationException("boom"); });
            Crash.Background("after the failure", () => done.Set());
            Check(done.Wait(TimeSpan.FromSeconds(10)), "a work item that threw does not stop the next one");
        }
        bool recorded = false;
        for (int i = 0; i < 100 && !recorded; i++)
        {
            recorded = ReadShared(Crash.LogPath()).Contains("test work item");
            if (!recorded) Thread.Sleep(50);
        }
        Check(recorded, "the failure of a work item is in the file");

        // Two records written at the same instant. This is the loss the four items below found first: the append of
        // this framework writes at the offset it opened with, so two writers overwrote each other without a word.
        const int Together = 12;
        using (var start = new ManualResetEventSlim(false))
        {
            var writers = new Thread[Together];
            for (int i = 0; i < Together; i++)
            {
                int n = i;
                writers[n] = new Thread(() => { start.Wait(); Crash.Write("at once " + n, new InvalidOperationException("boom " + n)); });
                writers[n].IsBackground = true;
                writers[n].Start();
            }
            start.Set();
            foreach (var w in writers) w.Join(TimeSpan.FromSeconds(10));
        }
        var atOnce = ReadShared(Crash.LogPath());
        int kept = 0;
        for (int i = 0; i < Together; i++) if (atOnce.Contains("at once " + i)) kept++;
        Equal(Together, kept, "every record of twelve threads that wrote at the same instant is in the file");

        // A body a task waits for: the record is written and the exception goes on to the task, so the waiter's own
        // continuation says what did not happen. The person is told by that step, not twice, so no report is made.
        var reports = new System.Collections.Generic.List<string>();
        Crash.Reporter = (where, e) => { lock (reports) reports.Add(where); };
        bool threwOn = false;
        try { Crash.Watched("watched body", () => { throw new InvalidOperationException("boom"); }); }
        catch (InvalidOperationException) { threwOn = true; }
        Check(threwOn, "Crash.Watched throws on, so the task faults and its continuation tells the person");
        Equal(7, Crash.Watched("watched value", () => 7), "Crash.Watched hands back the value of a body that did not fail");
        Check(ReadShared(Crash.LogPath()).Contains("watched body"), "the failure of a watched body is in the file");
        lock (reports) Check(!reports.Contains("watched body"), "a watched body is not reported twice: its waiter reports it");

        // The window's own background items (BD-174). Each is started the way the window starts it, with a body
        // that throws. The process must live through every one of them, the person must be told, and the record
        // must name the item. The names are the constants the window's call sites use, and the source check below
        // holds those call sites to them, so this drives the same items and not a copy of their text.
        var items = new[] { Crash.WallpaperAtStart, Crash.DriverCardRead, Crash.ClearRecentGames, Crash.WallpaperFromSettings };
        int alive = 0;
        foreach (var item in items)
        {
            var name = item;
            using (var after = new ManualResetEventSlim(false))
            {
                Crash.Background(name, () => { throw new InvalidOperationException("the " + name + " item failed"); });
                Crash.Background("after " + name, () => after.Set());
                if (after.Wait(TimeSpan.FromSeconds(10))) alive++;
            }
        }
        Equal(items.Length, alive, "the process lives on and keeps running work items after every background item threw");
        var missing = new System.Collections.Generic.List<string>();
        for (int i = 0; i < 200; i++)
        {
            var log = ReadShared(Crash.LogPath());
            missing.Clear();
            lock (reports)
                foreach (var item in items)
                {
                    if (!log.Contains(item)) missing.Add(item + " (no record)");
                    if (!reports.Contains(item)) missing.Add(item + " (not reported)");
                }
            if (missing.Count == 0) break;
            Thread.Sleep(50);
        }
        Equal("", string.Join(", ", missing), "every background item of the window left a record and was reported");
        Crash.Reporter = null;

        // The call sites themselves: the window starts each item through Crash.Background with the constant above,
        // and reports a background failure on the UI thread as an ordinary result of the page.
        var src = System.IO.Path.Combine(root, @"tools\win\amdgpu_wddm_control\src");
        var window = System.IO.File.ReadAllText(System.IO.Path.Combine(src, "MainForm.cs"));
        var pages = System.IO.File.ReadAllText(System.IO.Path.Combine(src, "MainForm.Pages.cs"));
        foreach (var name in new[] { "WallpaperAtStart", "DriverCardRead", "ClearRecentGames", "WallpaperFromSettings" })
            Check((window + pages).Contains("Crash.Background(Crash." + name + ","),
                "the window starts its background item through Crash.Background(Crash." + name + ")");
        var reporting = System.Text.RegularExpressions.Regex.Match(window, @"void ReportBackgroundFailure\(\)\s*\{(?<body>[\s\S]*?)\n        \}");
        Check(reporting.Success && reporting.Groups["body"].Value.Contains("BeginInvoke") &&
            reporting.Groups["body"].Value.Contains("Strings.T(\"action.background-failed\")") &&
            reporting.Groups["body"].Value.Contains("IsHandleCreated") && Strings.Has("action.background-failed"),
            "a background failure reaches the person on the UI thread, in plain words of the page");
        Check(window.Contains("Crash.Reporter = (where, e) => ReportBackgroundFailure();") && window.Contains("Crash.Reporter = null;"),
            "the window is the reporter while it lives and lets go of that when it closes");
    }

    // The file is appended to while this reads it, so both sides share it.
    static string ReadShared(string path)
    {
        if (!System.IO.File.Exists(path)) return "";
        using (var s = new System.IO.FileStream(path, System.IO.FileMode.Open, System.IO.FileAccess.Read, System.IO.FileShare.ReadWrite))
        using (var r = new System.IO.StreamReader(s))
            return r.ReadToEnd();
    }
}
