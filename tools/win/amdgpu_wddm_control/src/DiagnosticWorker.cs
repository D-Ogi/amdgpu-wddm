// A deadline for a read-only diagnostic child. A blocked OS provider must not
// leave a collector running after the report has returned to the window.
using System;
using System.Diagnostics;
using System.Text;
using System.Threading;

namespace AmdgpuWddmControl
{
    public static class DiagnosticWorker
    {
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
                process.Start(); process.BeginOutputReadLine();
                int remaining = Math.Max(0, timeoutMs - 250 - (int)clock.ElapsedMilliseconds);
                bool ended = process.WaitForExit(remaining);
                if (!ended) { try { process.Kill(); } catch (InvalidOperationException) { } catch (System.ComponentModel.Win32Exception) { } }
                drained.WaitOne(Math.Max(0, timeoutMs - (int)clock.ElapsedMilliseconds));
                lock (sync) {
                    string state = ended ? "Worker exit " + process.ExitCode : "Collection deadline reached; worker termination requested; report is partial";
                    return StartupDiagnosticData.Fit(text.ToString(), StartupDiagnosticData.Limit - 4096) + "\n" + state + ".\n";
                }
            }
        }
    }
}
