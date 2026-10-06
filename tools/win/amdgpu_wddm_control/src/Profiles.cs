// Per-application D3D12 profiles: the REG_SZ value "Experiment" of HKLM\SOFTWARE\amdgpu-wddm\D3D12\Applications\
// <image file name>, read by the D3D12 shell once per process when the process has no AMDGPU_WDDM_D3D12_EXPERIMENT
// (driver/umd/d3d12/ddi-trace.h). Syntax: lower-case letters, digits, hyphens and commas, at most 255 characters;
// unknown names are ignored by the shell. This file is pure (no registry): ProfileStore does the I/O.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.RegularExpressions;

namespace AmdgpuWddmControl
{
    public sealed class ProfileSwitch
    {
        public string Token, Title, Description;
        public ProfileSwitch(string token, string title, string description) { Token = token; Title = title; Description = description; }
    }

    public enum ProfileWriteKind { None, Set, Delete }

    public sealed class ProfileWrite
    {
        public string Image, Value;
        public ProfileWriteKind Kind;
    }

    public sealed class ProfileValue
    {
        public readonly List<string> Known = new List<string>();     // catalog switches, in catalog order
        public readonly List<string> Unknown = new List<string>();   // other names: shown as their own check boxes
    }

    public static class Profiles
    {
        public const string RegistryPath = @"SOFTWARE\amdgpu-wddm\D3D12\Applications";
        public const string ValueName = "Experiment";
        public const int MaxLength = 255;

        // The switches the shell reads (the ddi_experiment and ddi_experiment_off call sites in
        // driver/umd/d3d12; the gate is ShellTokens in test/UnitTests.cs). Keep in catalog order.
        //
        // Every behaviour the lab validated is the driver's default, so most of these names end in "-off" and
        // only ever subtract from the default (driver/umd/d3d12/ddi-trace.h). A group of GameGroups.cs that
        // carries such names is an Invert group: the box is checked while none of them is in the value. The
        // positive names raytracing-tier, recording-bind, retire-handoff and deferred-replay are still accepted
        // by the shell and do nothing, so they are not in this catalog: an older value that names them keeps
        // them as unknown names, which the support view shows and no edit drops.
        public static readonly ProfileSwitch[] Catalog =
        {
            new ProfileSwitch("raytracing-tier-off", "Do not report ray tracing support",
                "The driver tells the game that DirectX Raytracing is not available. The default reports the tier the engine computed."),
            new ProfileSwitch("present-noprimary", "Present without a primary surface",
                "Swap chain buffers are normal GPU allocations. This removes a copy in the desktop compositor."),
            new ProfileSwitch("present-cached", "Cached present buffers",
                "Swap chain buffers use cached CPU memory. Use together with \"Present without a primary surface\"."),
            new ProfileSwitch("recording-bind-off", "Do not bind command recording early",
                "Command lists read their recording state per call. The default binds it when the device is created, which costs less CPU time per draw."),
            new ProfileSwitch("retire-handoff-off", "Do not release resources on submission",
                "Submissions keep the release sequence. The default leaves it to the resource DDIs."),
            new ProfileSwitch("deferred-replay-off", "Do not record commands on worker threads",
                "Command list calls go to the GPU driver in line. The default sends them through a queue and worker threads, which can raise the frame rate in CPU-limited games."),
            new ProfileSwitch("direct-entry-off", "Do not let the engine record value-only calls",
                "The shell keeps its own entry for the value-only recording calls. The default gives it to the engine, which writes their queue entries itself."),
            new ProfileSwitch("release-two-phase-off", "Turn off the two-phase release (diagnostic)",
                "Turns off a stability fix. Use only when the developers ask for it."),
            new ProfileSwitch("import-progress-gate-off", "Turn off the import progress gate (diagnostic)",
                "Turns off a stability fix. Use only when the developers ask for it."),
            new ProfileSwitch("import-quarantine-off", "Turn off the import quarantine (diagnostic)",
                "Turns off a stability fix. Use only when the developers ask for it."),
            new ProfileSwitch("replay-log", "Write a replay log (diagnostic)",
                "The replay lines also go to a file of their own. This changes no behaviour; it costs the writes."),
        };

        static readonly Regex Syntax = new Regex("^[a-z0-9,-]*$", RegexOptions.CultureInvariant);
        static readonly Regex ImageSyntax = new Regex(@"^[A-Za-z0-9 _.()+-]{1,96}\.exe$", RegexOptions.CultureInvariant | RegexOptions.IgnoreCase);

        public static ProfileSwitch Find(string token) { return Catalog.FirstOrDefault(s => s.Token == token); }

        public static bool IsValidImage(string image)
        {
            return image != null && ImageSyntax.IsMatch(image) && image.Trim() == image && !image.StartsWith(".");
        }

        public static bool IsValidValue(string value)
        {
            return value != null && value.Length <= MaxLength && Syntax.IsMatch(value);
        }

        // Splits a stored value. A value outside the syntax is one unknown entry, so the editor shows it and keeps it
        // out of a save (Compose refuses it) instead of half-parsing it.
        public static ProfileValue Parse(string value)
        {
            var result = new ProfileValue();
            if (string.IsNullOrEmpty(value)) return result;
            if (!IsValidValue(value)) { result.Unknown.Add(value); return result; }
            var seen = new HashSet<string>();
            foreach (var raw in value.Split(','))
            {
                if (raw.Length == 0 || raw == "none" || !seen.Add(raw)) continue;
                if (Find(raw) != null) result.Known.Add(raw); else result.Unknown.Add(raw);
            }
            result.Known.Sort((a, b) => Array.FindIndex(Catalog, s => s.Token == a).CompareTo(Array.FindIndex(Catalog, s => s.Token == b)));
            return result;
        }

        // The value to store: the chosen catalog switches in catalog order, then the unknown names as they were.
        // Throws when the result would be outside the shell's syntax (the shell would then ignore all of it).
        public static string Compose(IEnumerable<string> known, IEnumerable<string> unknown)
        {
            var chosen = new HashSet<string>(known ?? Enumerable.Empty<string>());
            foreach (var t in chosen) if (Find(t) == null) throw new ArgumentException("not a catalog switch: " + t);
            var parts = Catalog.Where(s => chosen.Contains(s.Token)).Select(s => s.Token).ToList();
            foreach (var u in unknown ?? Enumerable.Empty<string>()) if (!parts.Contains(u)) parts.Add(u);
            var value = string.Join(",", parts);
            if (!IsValidValue(value)) throw new ArgumentException("the switch list is outside the driver's syntax or longer than " + MaxLength + " characters");
            return value;
        }

        // The settings rule (owner, 2026-10-03): the stored value is exactly the checked names, nothing is written for
        // an unchecked switch, and no checked name at all removes the application's key, so no empty value is left.
        // stored: the stored value, null when the key does not exist; chosen: every checked name, catalog or not.
        public static ProfileWrite PlanWrite(string image, string stored, IEnumerable<string> chosen)
        {
            var names = (chosen ?? Enumerable.Empty<string>()).Distinct().ToList();
            if (names.Count == 0)
                return new ProfileWrite { Image = image, Kind = stored == null ? ProfileWriteKind.None : ProfileWriteKind.Delete };
            var value = Compose(names.Where(n => Find(n) != null), names.Where(n => Find(n) == null));
            // The same set in another order (the installer writes its own order) is not a change.
            var before = Parse(stored);
            bool same = stored != null && IsValidValue(stored) && new HashSet<string>(before.Known.Concat(before.Unknown)).SetEquals(names);
            return new ProfileWrite { Image = image, Value = value, Kind = same ? ProfileWriteKind.None : ProfileWriteKind.Set };
        }
    }

    // DPM settings under HKLM\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters (docs/design/dpm.md, "Settings and
    // boot guard"): DpmMode 0 fixed / 1 dpm, DpmMaxMHz 1000-2000 on the 100 MHz grid, absent = 1500. Read by the KMD
    // at device start only.
    public static class DpmSettings
    {
        public const string RegistryPath = @"SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters";
        public const uint DefaultMaxMHz = 1500, HardMaxMHz = 2000, MinMHz = 1000;
        public static readonly uint[] CeilingChoices = { 1000, 1100, 1200, 1300, 1400, 1500, 1600, 1700, 1800, 1900, 2000 };

        public static bool IsValidCeiling(uint mhz) { return Array.IndexOf(CeilingChoices, mhz) >= 0; }
        public static bool IsValidMode(uint mode) { return mode == 0 || mode == 1; }

        // What the KMD will use for a stored value: absent = default, out of range = fixed clock (INVALID_SETTING).
        public static string Describe(uint? mode, uint? maxMHz)
        {
            string m = mode == null ? "Fixed (default)" : mode == 1 ? "Automatic" : mode == 0 ? "Fixed" : "Invalid (" + mode + ")";
            string c = maxMHz == null ? DefaultMaxMHz + " MHz (default)" : maxMHz + " MHz";
            return m + ", ceiling " + c;
        }

        // The writes of the Performance page under the settings rule: DpmMode 1 only while "automatic" is checked
        // (unchecking removes it; a 0 the driver stored after a fallback means the same as no value and stays),
        // DpmMaxMHz only while a ceiling is checked (unchecking removes it).
        public static List<RegWrite> PlanWrites(uint? storedMode, uint? storedMax, bool automatic, uint? ceiling)
        {
            var writes = new List<RegWrite>();
            if (automatic) { if (storedMode != 1) writes.Add(RegWrite.Dword(RegistryPath, "DpmMode", 1)); }
            else if (storedMode == 1) writes.Add(RegWrite.Remove(RegistryPath, "DpmMode"));
            if (ceiling != null) { if (storedMax != ceiling) writes.Add(RegWrite.Dword(RegistryPath, "DpmMaxMHz", ceiling.Value)); }
            else if (storedMax != null) writes.Add(RegWrite.Remove(RegistryPath, "DpmMaxMHz"));
            return writes;
        }
    }
}
