// The pure parts behind the plan screen: the settings-impact plan in plain words (WU-006, WU-044), the release notes
// (C16: the English file with a visible label when no translation is attached), the prepared-folder check (WU-051)
// and the quoting of one command line.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;

namespace AmdgpuWddmSetup
{
    public sealed class SettingLine
    {
        public string TextId, DecisionId;
        public object[] Args = new object[0];
    }

    public static class SettingsView
    {
        // The settings a user knows from the control app get a line each; every other driver value is counted in one
        // line ("other driver settings"). Registry names never reach the window: they are in the support file.
        public static List<SettingLine> Lines(SettingsPlan plan)
        {
            var lines = new List<SettingLine>();
            if (plan == null) return lines;
            int other = 0, otherChanged = 0;
            foreach (var r in plan.Rows)
            {
                // kept, same and driver-closed keep what the computer holds now; every other decision writes r.Value.
                var shown = r.Decision == "kept" || r.Decision == "same" || r.Decision == "driver-closed" ? (r.Current ?? r.Value) : r.Value;
                var decision = "settings.decision." + (r.Decision ?? "set");
                string group = r.Group ?? "", name = r.Name ?? "";
                SettingLine line = null;
                if (group == "parameters" && name == "DpmMaxMHz") line = new SettingLine { TextId = "settings.clock-limit", Args = new[] { Json.Show(shown) } };
                else if (group == "parameters" && name == "DpmMode") line = new SettingLine { TextId = Number(shown) == 0 ? "settings.auto-clock.off" : "settings.auto-clock.on" };
                else if (group == "parameters" && name == "CuMode") line = new SettingLine { TextId = "settings.cores", Args = new[] { Json.Show(shown) } };
                else if (group == "desktop_router" && name == "DwmForceCpu") line = new SettingLine { TextId = Number(shown) == 1 ? "settings.desktop.cpu" : "settings.desktop.gpu" };
                // The D3D11 route (tester.11 on): gpu-default sends Direct3D 11 and 10.1 applications to the graphics chip,
                // while Windows' own applications and Direct3D 10.0 stay on the processor; allowlist sends only the listed ones.
                else if (group == "app_router" && name == "Mode" && Json.Show(shown) == "gpu-default") line = new SettingLine { TextId = "settings.d3d11.gpu" };
                else if (group == "app_router" && name == "Mode" && Json.Show(shown) == "allowlist") line = new SettingLine { TextId = "settings.d3d11.listed" };
                else if (group.StartsWith("d3d12:", StringComparison.Ordinal)) line = new SettingLine { TextId = "settings.game", Args = new object[] { group.Substring(6) } };
                if (line == null)
                {
                    other++;
                    if (r.Decision != "same" && r.Decision != "kept" && r.Decision != "driver-closed") otherChanged++;
                    continue;
                }
                line.DecisionId = decision;
                lines.Add(line);
            }
            if (other > 0) lines.Add(new SettingLine { TextId = "settings.other", Args = new object[] { other, otherChanged }, DecisionId = null });
            return lines;
        }

        static long Number(object v)
        {
            if (v is int) return (int)v;
            if (v is long) return (long)v;
            if (v is decimal) return (long)(decimal)v;
            long n;
            return v is string && long.TryParse((string)v, NumberStyles.Integer, CultureInfo.InvariantCulture, out n) ? n : -1;
        }
    }

    public sealed class ReleaseNotes
    {
        public string File, Language, Text;
        public bool EnglishFallback;    // the window's language has no notes file: the English text, labelled

        public static readonly string[] Sections = { "New", "Fixed", "Known issues", "Settings affected" };

        public static ReleaseNotes Load(string packageDir, string language)
        {
            if (string.IsNullOrEmpty(packageDir)) return null;
            if (language != "en")
            {
                var local = Path.Combine(packageDir, "RELEASE-NOTES." + language + ".md");
                if (System.IO.File.Exists(local)) return new ReleaseNotes { File = local, Language = language, Text = Plain(System.IO.File.ReadAllText(local, Encoding.UTF8)) };
            }
            var en = Path.Combine(packageDir, "RELEASE-NOTES.md");
            if (!System.IO.File.Exists(en)) return null;
            return new ReleaseNotes { File = en, Language = "en", EnglishFallback = language != "en", Text = Plain(System.IO.File.ReadAllText(en, Encoding.UTF8)) };
        }

        // Markdown as plain text: headings without '#', list items with a bullet, code marks and emphasis removed,
        // the wrapped lines of a paragraph or a list item joined, so that the text box wraps them itself.
        public static string Plain(string markdown)
        {
            var output = new List<string>();
            bool open = false;      // the last output line is a paragraph or list item that a next line continues
            foreach (var raw in (markdown ?? "").Replace("\r\n", "\n").Split('\n'))
            {
                var t = raw.TrimEnd().Replace("`", "").Replace("**", "");
                if (t.Trim().Length == 0) { if (output.Count > 0 && output[output.Count - 1].Length > 0) output.Add(""); open = false; continue; }
                if (t.StartsWith("#", StringComparison.Ordinal))
                {
                    if (output.Count > 0 && output[output.Count - 1].Length > 0) output.Add("");
                    output.Add(t.TrimStart('#').Trim());
                    open = false;
                    continue;
                }
                if (Regex.IsMatch(t, @"^\s*[-*] ")) { output.Add("• " + Regex.Replace(t, @"^\s*[-*] ", "")); open = true; continue; }
                if (open) { output[output.Count - 1] += " " + t.Trim(); continue; }
                output.Add(t.Trim());
                open = true;
            }
            while (output.Count > 0 && output[output.Count - 1].Length == 0) output.RemoveAt(output.Count - 1);
            return string.Join("\r\n", output);
        }

        // Whether the notes have the four sections (the release builder refuses notes without them).
        public static List<string> MissingSections(string markdown)
        {
            var heads = Regex.Matches(markdown ?? "", @"(?m)^##\s+(.+?)\s*$").Cast<Match>().Select(m => m.Groups[1].Value).ToList();
            return Sections.Where(s => !heads.Contains(s)).ToList();
        }
    }

    public static class PreparedFolder
    {
        public const string Schema = "amdgpu-wddm.offline-set/1";

        // null when dir is a prepared folder (offline-set.json of the known schema, its manifest and its installer),
        // else the reason's string id. version: the release it holds.
        public static string Check(string dir, out string version)
        {
            version = null;
            if (string.IsNullOrWhiteSpace(dir) || !Directory.Exists(dir)) return "folder.missing";
            var set = Path.Combine(dir, "offline-set.json");
            if (!File.Exists(set)) return "folder.not-prepared";
            var o = Json.Parse(SafeRead(set));
            if (o == null || Json.Str(o, "schema") != Schema) return "folder.unknown";
            if (!File.Exists(Path.Combine(dir, "manifest.json")) || !File.Exists(Path.Combine(dir, "installer", "install.ps1"))) return "folder.incomplete";
            version = Json.Str(o, "version");
            return null;
        }

        static string SafeRead(string path) { try { return File.ReadAllText(path, Encoding.UTF8); } catch (Exception) { return null; } }
    }

    public static class CommandLine
    {
        // One argument as CommandLineToArgvW and the C runtime read it back: quoted when it has a space, a tab or a
        // quote or is empty; backslashes doubled only in front of a quote.
        public static string Quote(string arg)
        {
            if (arg == null) arg = "";
            if (arg.Length > 0 && arg.IndexOfAny(new[] { ' ', '\t', '"', '\n', '\v' }) < 0) return arg;
            var b = new StringBuilder("\"");
            int slashes = 0;
            foreach (var ch in arg)
            {
                if (ch == '\\') { slashes++; continue; }
                if (ch == '"') { b.Append('\\', slashes * 2 + 1); b.Append('"'); slashes = 0; continue; }
                b.Append('\\', slashes); slashes = 0; b.Append(ch);
            }
            b.Append('\\', slashes * 2);
            b.Append('"');
            return b.ToString();
        }

        public static string Join(IEnumerable<string> args) { return string.Join(" ", args.Select(Quote)); }
    }
}
