// The last words of a window that ends by itself (BD-174). On 2026-10-14 the control app disappeared between two
// clicks on the Graphics page: no Application Error, no WER report, no .NET Runtime event, and nothing in
// control-actions.log. Reading the exit paths explains why such a death can be silent:
//
//   * This program never calls Environment.Exit or Application.Exit, never closes its main form from code and has
//     no catch-all that would, so a clean exit of Application.Run cannot come from its own code.
//   * An exception on the UI thread does not end the process either: the image ships no app.config and never calls
//     Application.SetUnhandledExceptionMode, so WinForms stays in its Automatic mode with jitDebugging off, shows
//     its own dialog and keeps the window alive.
//   * An exception that escapes a ThreadPool work item DOES end the process at once, and a machine with Windows
//     Error Reporting switched off (the lab is debloated) writes no event for it. That is the one silent path.
//
// Nobody can say which work item fired that day, so the whole class is closed instead of watched: every
// background body of this application runs through one of the two guards below, and test/GuiTests.cs (G-SRC)
// refuses a QueueUserWorkItem, a Task.Run, a StartNew or a new Thread whose body does not.
//
//   * Crash.Guard, for a body nobody waits for: the failure is recorded, the person is told in the ordinary way
//     through Crash.Reporter, and the exception is swallowed, so the window lives on.
//   * Crash.Watched, for a body a task waits for: the failure is recorded and thrown on, so the waiter's own
//     continuation reports it exactly as it always did ("the change was not made", "the report failed").
//
// Crash.Watch records a process-fatal exception from any thread before the process goes. The record is a file this
// account can always write, next to the other per-user logs; it is diagnostic text, never a question to the
// person, and a failure to write it is never allowed to make anything worse.
using System;
using System.Globalization;
using System.IO;
using System.Text;

namespace AmdgpuWddmControl
{
    public static class Crash
    {
        public const int MaxRecordChars = 8000;        // one record; a stack of a recursion is cut, not kept whole
        public const long MaxLogBytes = 1 << 20;       // the file is started again above this

        // The places of the window's own background items. The window and the tests name them from here, so a test
        // drives the same item the window starts instead of a copy of its text (BD-174).
        public const string WallpaperAtStart = "wallpaper at start";
        public const string DriverCardRead = "driver card read";
        public const string ClearRecentGames = "clear recent games";
        public const string WallpaperFromSettings = "wallpaper from Settings";

        // The tests put the file under their own temporary directory (the build gives them one below its output, so
        // nothing of a test run reaches a real profile). The window never sets this, and an installed application
        // has no setting for it, so it writes to the account's own folder.
        public static string DirectoryForTests;

        // AMDGPU_WDDM_CONTROL_STATE moves the app's state directory, which the build's own gates set, so a gate
        // that makes the application record something writes it below the build and not into a profile. A lab
        // build's only (BD-167): the gates of a release build run its lab twin, and the release image holds no
        // name of a diagnostic variable.
        public static string Folder()
        {
            if (DirectoryForTests != null) return DirectoryForTests;
#if AMDGPU_WDDM_LAB_FLAVOUR
            var over = Environment.GetEnvironmentVariable("AMDGPU_WDDM_CONTROL_STATE");
            return string.IsNullOrEmpty(over)
                ? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "amdgpu-wddm")
                : over;
#else
            return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "amdgpu-wddm");
#endif
        }

        public static string LogPath() { return Path.Combine(Folder(), "crash.log"); }

        // One record, as it goes into the file. Pure, so the tests read it: the moment, where it happened, the
        // exception's type, message and stack, each inner exception, and nothing longer than MaxRecordChars.
        public static string Record(string where, Exception e, DateTime utc, string version)
        {
            var w = new StringBuilder();
            w.Append(utc.ToString("yyyy-MM-ddTHH:mm:ss'Z'", CultureInfo.InvariantCulture));
            w.Append(' ').Append(string.IsNullOrEmpty(where) ? "unknown" : where);
            if (!string.IsNullOrEmpty(version)) w.Append(" (").Append(version).Append(')');
            w.Append(Environment.NewLine);
            if (e == null) w.Append("no exception object").Append(Environment.NewLine);
            for (var x = e; x != null; x = x.InnerException)
            {
                string message, stack;
                // An exception whose own ToString or Message throws must not take the record with it.
                try { message = x.Message; } catch (Exception) { message = "<the message could not be read>"; }
                try { stack = x.StackTrace; } catch (Exception) { stack = null; }
                w.Append("  ").Append(x.GetType().FullName).Append(": ").Append(message).Append(Environment.NewLine);
                if (!string.IsNullOrEmpty(stack)) w.Append(stack).Append(Environment.NewLine);
            }
            var text = w.ToString();
            if (text.Length > MaxRecordChars) text = text.Substring(0, MaxRecordChars) + "[cut]" + Environment.NewLine;
            return text;
        }

        static readonly object Gate = new object();

        // Never throws, whatever the exception or the file system do.
        public static void Write(string where, Exception e)
        {
            try
            {
                string version = null;
                try { version = System.Reflection.Assembly.GetExecutingAssembly().GetName().Version.ToString(); } catch (Exception) { }
                var path = LogPath();
                Directory.CreateDirectory(Folder());
                try { var f = new FileInfo(path); if (f.Exists && f.Length > MaxLogBytes) File.Delete(path); } catch (Exception) { }
                var bytes = new UTF8Encoding(false).GetBytes(Record(where, e, DateTime.UtcNow, version));
                // Two records written at the same instant must not land on the same bytes. FileMode.Append of this
                // framework writes at the offset the stream found when it was opened, so two writers would
                // overwrite one another's record, and the first test of the four background items lost two records
                // exactly that way. The lock keeps the threads of this process apart, and FileShare.Read keeps
                // another process of ours out of the file for the few bytes of one record, while a reader of the
                // file may hold it open the whole time. The bytes of one record are written in one call.
                lock (Gate)
                    for (int attempt = 0; ; attempt++)
                    {
                        try
                        {
                            using (var s = new FileStream(path, FileMode.Append, FileAccess.Write, FileShare.Read))
                                s.Write(bytes, 0, bytes.Length);
                            return;
                        }
                        catch (IOException) { if (attempt == 4) return; System.Threading.Thread.Sleep(20); }
                    }
            }
            catch (Exception) { }
        }

        // How the person is told. The window sets this while it lives and marshals the call to the UI thread, where
        // a background failure becomes an ordinary error on the page. A test, a render and the helper processes set
        // nothing, and then a failure is recorded and nothing else happens.
        public static Action<string, Exception> Reporter;

        static void Report(string where, Exception e)
        {
            var r = Reporter;
            if (r == null) return;
            try { r(where, e); } catch (Exception) { }
        }

        // A body nobody waits for: recorded, reported, swallowed. The window lives on, and the failure is in the
        // file and on the page instead of taking the process down without a trace.
        public static void Guard(string where, Action work)
        {
            try { if (work != null) work(); }
            catch (Exception e) { Write(where, e); Report(where, e); }
        }

        // A body a task waits for: recorded and thrown on. The waiter's continuation runs on the UI thread and says
        // what did not happen, so the person is told once, by the step that failed, and not twice.
        public static T Watched<T>(string where, Func<T> work)
        {
            try { return work(); }
            catch (Exception e) { Write(where, e); throw; }
        }

        public static void Watched(string where, Action work)
        {
            try { if (work != null) work(); }
            catch (Exception e) { Write(where, e); throw; }
        }

        // A background work item of the window, guarded.
        public static void Background(string where, Action work)
        {
            System.Threading.ThreadPool.QueueUserWorkItem(_ => Guard(where, work));
        }

        // The process's last words from any thread. It changes no behaviour: the runtime ends the process anyway.
        static bool _watching;

        public static void Watch()
        {
            if (_watching) return;
            _watching = true;
            AppDomain.CurrentDomain.UnhandledException += (s, e) => Write("unhandled" + (e.IsTerminating ? " (terminating)" : ""), e.ExceptionObject as Exception);
        }
    }
}
