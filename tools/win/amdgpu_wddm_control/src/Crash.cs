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
// So every background work item of the window runs through Crash.Background, which keeps the window alive and
// records what happened, and Crash.Watch records a process-fatal exception from any thread before the process
// goes. The record is a file this account can always write, next to the other per-user logs; it is diagnostic
// text, never a question to the person, and a failure to write it is never allowed to make anything worse.
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

        // The tests put the file under their own temporary directory (the build gives them one below its output, so
        // nothing of a test run reaches a real profile). The window never sets this: there is no environment
        // override and no setting, so production always writes to the account's own folder.
        public static string DirectoryForTests;

        public static string Folder()
        {
            return DirectoryForTests ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "amdgpu-wddm");
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
                // Shared: two threads that die together both get their record, and a reader of the file never
                // keeps one of them out. The bytes of one record are written in one call.
                var bytes = new UTF8Encoding(false).GetBytes(Record(where, e, DateTime.UtcNow, version));
                using (var s = new FileStream(path, FileMode.Append, FileAccess.Write, FileShare.ReadWrite))
                    s.Write(bytes, 0, bytes.Length);
            }
            catch (Exception) { }
        }

        // A background work item of the window. An exception inside it is recorded and swallowed: the window stays,
        // and the failure is in the file instead of taking the process down without a trace.
        public static void Background(string where, Action work)
        {
            System.Threading.ThreadPool.QueueUserWorkItem(_ =>
            {
                try { if (work != null) work(); }
                catch (Exception e) { Write(where, e); }
            });
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
