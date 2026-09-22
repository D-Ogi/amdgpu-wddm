// A condensed, live view of bc250kmd's own state (owner's request, 2026-09-22: "the overlay should tell what
// the driver is doing and in what state/mode it is"). KmdProvider.cs already reads the registry trail, which
// survives even a driver that stopped answering; this one asks the driver itself, through bc250kmd_cli info
// (BC250_ESCAPE_GET_INFO), for the things the registry does not carry: version, display-only vs full WDDM
// table, which gates are open, and the full table's own present counters (blits, flips). Temperature comes
// from Driver.cs, the same bc250rd.sys method tools/win/bc250rd/temp.py drives from the development PC - not a
// second copy of it, the same shared class GpuProvider already uses.
//
// The cli is not always there (an older build, a machine where it was never copied over) and the escape can
// refuse (not an administrator, bc250kmd not the active driver): both are shown as one row, same as the rest
// of this overlay does for a data source that is not available, rather than a stack trace on the screen.
using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;

namespace Bc250Mon
{
    public sealed class KmdInfoProvider : IProvider
    {
        // driver/kmd/README.md / tools/win/bc250kmd_cli/README.md: "on the target it lives in C:\BC250\kmd\".
        public const string CliPath = @"C:\BC250\kmd\bc250kmd_cli.exe";
        const int TimeoutMs = 5000;

        readonly Driver _driver;
        string _lastNote;

        public KmdInfoProvider(Driver driver) { _driver = driver; }
        public string Name { get { return "kmdinfo"; } }
        public TimeSpan Period { get { return TimeSpan.FromSeconds(5); } }

        public void Poll(State state)
        {
            var p = new Panel { Name = Name, Title = "bc250kmd live", Order = 15 };

            if (!File.Exists(CliPath))
            {
                Note(state, p, "bc250kmd_cli.exe not found at " + CliPath, Level.Warn);
            }
            else
            {
                string stdout, stderr;
                int code = Run(CliPath, "info", TimeoutMs, out stdout, out stderr);
                if (!ParseInto(p, stdout))
                {
                    // Nothing recognisable came back: a refusal ("refused: caller is not an administrator"), the
                    // escape failing outright, or the process not starting at all - one line, not an empty panel.
                    string line = FirstNonEmptyLine(stdout) ?? FirstNonEmptyLine(stderr) ??
                                  "bc250kmd_cli info exited " + code + " with no output";
                    Note(state, p, line, Level.Warn);
                }
                else _lastNote = null;
            }

            try
            {
                double t = _driver.ReadTemperature();
                Level lvl = t >= GpuProvider.ErrorC ? Level.Error : t >= GpuProvider.WarnC ? Level.Warn : Level.Good;
                p.Rows.Add(new Row("Temperature", t.ToString("0.0", CultureInfo.InvariantCulture) + " C", lvl));
            }
            catch (Exception e)
            {
                p.Rows.Add(new Row("Temperature", e.Message, Level.Error));
            }

            state.SetPanel(p);
        }

        // Logged only on change, like every other provider here - a live panel refreshed every 5 seconds must
        // not spam the log with the same line forever.
        void Note(State state, Panel p, string line, Level level)
        {
            p.Rows.Add(new Row("info", line, level));
            if (_lastNote != line) state.Log(Name, level, "bc250kmd_cli info: " + line);
            _lastNote = line;
        }

        static string FirstNonEmptyLine(string text)
        {
            if (string.IsNullOrEmpty(text)) return null;
            foreach (var raw in text.Split('\n'))
            {
                string line = raw.Trim(' ', '\r', '\t');
                if (line.Length > 0) return line;
            }
            return null;
        }

        // bc250kmd_cli.c's Info(): fixed left-hand labels ("version", "last stage", "mode", "presents",
        // "counters", "gates"), one per line - matched by prefix rather than parsed as key/value pairs, since
        // the labels are the contract (the cli and this file both come from this repository) and the values
        // are already formatted for a human to read as they are.
        static bool ParseInto(Panel p, string output)
        {
            bool any = false;
            if (string.IsNullOrEmpty(output)) return false;
            foreach (var raw in output.Split('\n'))
            {
                string line = raw.TrimEnd('\r');
                if (Row(p, line, "version", "Version")) any = true;
                else if (Row(p, line, "last stage", "Stage", IsRefusedStage)) any = true;
                else if (Row(p, line, "mode", "Mode", IsFullTable)) any = true;
                else if (Row(p, line, "presents", "Presents")) any = true;
                else if (Row(p, line, "counters", "Counters")) any = true;
                else if (Row(p, line, "gates", "Gates")) any = true;
            }
            return any;
        }

        static bool Row(Panel p, string line, string prefix, string label) { return Row(p, line, prefix, label, null); }

        static bool Row(Panel p, string line, string prefix, string label, Func<string, Level> level)
        {
            if (line.Length <= prefix.Length || string.Compare(line, 0, prefix, 0, prefix.Length, StringComparison.OrdinalIgnoreCase) != 0)
                return false;
            string value = line.Substring(prefix.Length).Trim();
            p.Rows.Add(new Row(label, value, level != null ? level(value) : Level.Info));
            return true;
        }

        static Level IsRefusedStage(string value)
        {
            // KmdStages.RefusedByGuard / .StartFailed (90, 91) - the same numbers KmdProvider's own Stage row
            // colours red.
            return value.StartsWith("90 ") || value.StartsWith("91 ") ? Level.Error : Level.Info;
        }

        static Level IsFullTable(string value)
        {
            return value.IndexOf("FULL WDDM", StringComparison.OrdinalIgnoreCase) >= 0 ? Level.Warn : Level.Info;
        }

        // Async stdout/stderr reads: the cli's output is a few hundred bytes, but reading one stream fully
        // before starting the other is the classic way to deadlock a child process that writes to both.
        static int Run(string exe, string args, int timeoutMs, out string stdout, out string stderr)
        {
            var outBuf = new StringBuilder();
            var errBuf = new StringBuilder();
            var psi = new ProcessStartInfo(exe, args)
            {
                UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true, CreateNoWindow = true,
            };
            try
            {
                using (var proc = new Process { StartInfo = psi })
                {
                    proc.OutputDataReceived += (s, e) => { if (e.Data != null) outBuf.Append(e.Data).Append('\n'); };
                    proc.ErrorDataReceived += (s, e) => { if (e.Data != null) errBuf.Append(e.Data).Append('\n'); };
                    proc.Start();
                    proc.BeginOutputReadLine();
                    proc.BeginErrorReadLine();
                    if (!proc.WaitForExit(timeoutMs))
                    {
                        try { proc.Kill(); } catch { }
                        stdout = outBuf.ToString();
                        stderr = "bc250kmd_cli info timed out after " + timeoutMs + " ms";
                        return -1;
                    }
                    stdout = outBuf.ToString();
                    stderr = errBuf.ToString();
                    return proc.ExitCode;
                }
            }
            catch (Exception e)
            {
                stdout = "";
                stderr = e.Message;
                return -1;
            }
        }
    }
}
