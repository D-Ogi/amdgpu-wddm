// A deadline for a read-only diagnostic child. A blocked OS provider must not
// leave a collector running after the report has returned to the window.
using System;
using System.Diagnostics;
using System.Globalization;
using System.Text;
using System.Threading;

namespace AmdgpuWddmControl
{
    public static class DiagnosticWorker
    {
        // 0xE0434352: the exception code Windows reports for a .NET exception that nothing caught.
        public const int UnhandledClrException = unchecked((int)0xE0434352);

        public static string Run(ProcessStartInfo start, int timeoutMs)
        {
            var clock = Stopwatch.StartNew();
            var text = new StringBuilder();
            object sync = new object();
            start.UseShellExecute = false; start.CreateNoWindow = true;
            start.RedirectStandardOutput = true; start.StandardOutputEncoding = Encoding.UTF8;
            using (var drained = new ManualResetEvent(false))
            using (var process = new Process { StartInfo = start })
            {
                process.OutputDataReceived += (s, e) => {
                    if (e.Data == null) { try { drained.Set(); } catch (ObjectDisposedException) { } return; }
                    lock (sync) {
                        int room = StartupDiagnosticData.Limit - text.Length;
                        if (room > 1) text.Append(e.Data.Substring(0, Math.Min(e.Data.Length, room - 1))).Append('\n');
                    }
                };
                // A worker that cannot start is a line of the report, not an exception of the report (BD-142).
                try { process.Start(); }
                catch (Exception e) { return "The diagnostics worker could not start: " + StartupDiagnosticData.Describe(e); }
                process.BeginOutputReadLine();
                int remaining = Math.Max(0, timeoutMs - 250 - (int)clock.ElapsedMilliseconds);
                bool ended = process.WaitForExit(remaining);
                if (!ended) { try { process.Kill(); } catch (InvalidOperationException) { } catch (System.ComponentModel.Win32Exception) { } }
                drained.WaitOne(Math.Max(0, timeoutMs - (int)clock.ElapsedMilliseconds));
                lock (sync) {
                    string state = ended ? ExitText(process.ExitCode) : "Collection deadline reached; worker termination requested; report is partial";
                    return StartupDiagnosticData.Fit(text.ToString(), StartupDiagnosticData.Limit - 1024) + "\n" + state + ".\n";
                }
            }
        }

        // The worker's exit as the report's last line: the number as before, and what it means. The worker's own
        // codes come from StartupDiagnosticData.RunWorker; 0xE0434352 is what Windows reports for a .NET exception
        // that escaped the worker anyway (tester.29, BD-142).
        public static string ExitText(int code)
        {
            string text = "Worker exit " + code.ToString(CultureInfo.InvariantCulture);
            switch (code)
            {
                case 0: return text;
                case StartupDiagnosticData.WorkerFaulted: return text + " (an error outside the sources stopped the collection; the lines above name it)";
                case StartupDiagnosticData.WorkerOutputLost: return text + " (the worker could not write its output; the text above is all that arrived)";
                case UnhandledClrException: return text + " (0xE0434352: the worker stopped on a .NET exception that nothing caught; the Application event log holds its record)";
                default: return text + " (0x" + code.ToString("X8", CultureInfo.InvariantCulture) + ")";
            }
        }
    }
}
