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

    static void CrashTests()
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
