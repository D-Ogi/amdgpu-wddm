// setup.exe --apply <dir> [--wait-pid <n>]: installs one update that the control application downloaded and checked
// (docs/design/self-update.md). The engine is installer\apply.ps1 of the package this setup.exe belongs to, which is
// the installed release's kept repair set: the installed release checks the next one. This window asks no question:
// one small window while the engine runs, then one message with the outcome. A driver update ends with the setup
// window of the new release, which installs it as every update is installed.
//
// This file is the pure part (the script, the engine arguments, the result and its words), in the unit tests; the window
// is ApplyWindow.cs.
using System;
using System.Collections.Generic;
using System.IO;

namespace AmdgpuWddmSetup
{
    public sealed class ApplyResult
    {
        public string Outcome, Reason, MessageId, HandoffSetup, Detail, Release;
        public bool Restart;
    }

    public static class ApplyUpdate
    {
        // The outcomes of apply.ps1 and its exit codes: 0 applied, up-to-date or handoff; 2 refused; 3 rolled back;
        // 6 failed. The window ends with the same code.
        public static readonly string[] Outcomes = { "applied", "up-to-date", "handoff", "refused", "rolled-back", "failed" };
        public const string Fallback = "apply.failed";
        public const int EngineBoundMinutes = 20;     // the driver package is a large zip, unpacked and checked
        public const int WaitPidSeconds = 120;

        // apply.ps1 of this package: a kept repair set and a prepared folder hold setup.exe in the package root, the
        // unpacked release holds it beside files\, and --package names the package root itself.
        public static string FindScript(string exeDir, string package, Func<string, bool> exists)
        {
            var candidates = new List<string>();
            if (!string.IsNullOrEmpty(package)) candidates.Add(Path.Combine(package, "installer", "apply.ps1"));
            if (!string.IsNullOrEmpty(exeDir))
            {
                candidates.Add(Path.Combine(exeDir, "installer", "apply.ps1"));
                candidates.Add(Path.Combine(exeDir, "files", "installer", "apply.ps1"));
            }
            foreach (var c in candidates) if (exists(c)) return c;
            return null;
        }

        public static string EngineArguments(string script, string updateDir, string resultFile)
        {
            return CommandLine.Join(new[] { "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", script,
                "-UpdateDir", updateDir, "-ResultFile", resultFile });
        }

        // The result file of apply.ps1 (schema nagi.apply-result/1); null when it is not one.
        public static ApplyResult ParseResult(string json)
        {
            IDictionary<string, object> o;
            try { o = Json.Parse(json); } catch (Exception) { return null; }
            if (o == null || Json.Str(o, "schema") != "nagi.apply-result/1") return null;
            var r = new ApplyResult
            {
                Outcome = Json.Str(o, "outcome"), Reason = Json.Str(o, "reason"), MessageId = Json.Str(o, "message"),
                HandoffSetup = Json.Str(o, "handoff_setup"), Detail = Json.Str(o, "detail"), Release = Json.Str(o, "release"),
                Restart = Json.Bool(o, "restart"),
            };
            if (Array.IndexOf(Outcomes, r.Outcome) < 0) return null;
            return r;
        }

        // The words of a result: its message id when the tables have it, else the words of a failed update. A missing
        // result is a failed update too: the engine stopped before it wrote one.
        public static string MessageOf(ApplyResult r)
        {
            if (r == null || string.IsNullOrEmpty(r.MessageId) || !r.MessageId.StartsWith("apply.", StringComparison.Ordinal)) return Fallback;
            return Strings.Has(r.MessageId + ".title") && Strings.Has(r.MessageId + ".body") ? r.MessageId : Fallback;
        }

        public static int ExitCodeOf(ApplyResult r)
        {
            if (r == null) return 6;
            switch (r.Outcome)
            {
                case "refused": return 2;
                case "rolled-back": return 3;
                case "failed": return 6;
                default: return 0;
            }
        }

        // The setup window of the new release, started for a driver update: only a setup.exe under the protected update
        // folder that apply.ps1 unpacked and checked.
        public static bool HandoffAllowed(ApplyResult r, string updateRoot)
        {
            if (r == null || r.Outcome != "handoff" || string.IsNullOrEmpty(r.HandoffSetup) || string.IsNullOrEmpty(updateRoot)) return false;
            string full, root;
            try { full = Path.GetFullPath(r.HandoffSetup); root = Path.GetFullPath(Path.Combine(updateRoot, "driver")).TrimEnd('\\') + "\\"; }
            catch (Exception) { return false; }
            return full.StartsWith(root, StringComparison.OrdinalIgnoreCase) && string.Equals(Path.GetFileName(full), "setup.exe", StringComparison.OrdinalIgnoreCase);
        }

        public static string UpdateRoot()
        {
            return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "amdgpu-wddm", "update");
        }
    }
}
