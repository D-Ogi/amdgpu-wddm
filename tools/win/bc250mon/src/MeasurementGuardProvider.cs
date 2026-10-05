// Everything that can change a measurement without touching the driver: the miniport's settings against the
// defaults the installer actually applied, the marker files that arm or pause instrumentation, and the installed
// release against the kernel module that answers now.
//
// The 24 CU regression of 2026-10-05 is the general case this panel covers. `CuMode` is in no INF and in no
// installer default table, so a tester release install left it out, the driver harvested to 24, and three days
// of Witcher 3 and Rise of the Tomb Raider numbers were taken at 24 CU. Any other setting can be lost the same
// way. The rule: the live `Parameters` key is compared with `HKLM\SOFTWARE\amdgpu-wddm\Release\AppliedDefaults`,
// the record the installer writes of its own table, and with the lab's `expectations.json` on top of it.
//
// Cost: two registry key reads, up to six File.Exists, and two short file reads plus one manifest read, each
// only when the write time changed. Measured in tens of microseconds, polled every 10 seconds.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using System.Web.Script.Serialization;
using Microsoft.Win32;

namespace Bc250Mon
{
    /// <summary>The `parameters` table of `Release\AppliedDefaults`: what the installer last applied here.</summary>
    public sealed class InstalledDefaults
    {
        public bool Present;
        public string Error;
        public Dictionary<string, int> Parameters = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);

        public static InstalledDefaults Parse(string json)
        {
            var result = new InstalledDefaults();
            if (string.IsNullOrEmpty(json)) return result;
            try
            {
                var root = new JavaScriptSerializer().Deserialize<Dictionary<string, object>>(json);
                object parameters;
                if (root == null || !root.TryGetValue("parameters", out parameters)) { result.Error = "no parameters member"; return result; }
                var table = parameters as Dictionary<string, object>;
                if (table == null) { result.Error = "parameters is not an object"; return result; }
                foreach (var pair in table)
                    if (pair.Value != null && !(pair.Value is string))
                        result.Parameters[pair.Key] = Convert.ToInt32(pair.Value, CultureInfo.InvariantCulture);
                result.Present = true;
            }
            catch (Exception e) { result.Error = e.Message; }
            return result;
        }
    }

    /// <summary>One marker file. `Value` is null for a marker whose existence is the whole message.</summary>
    public sealed class LabMarker
    {
        public string Name;
        public bool Present;
        public string Value;                // trimmed content, for the files that arm something
        public DateTime WriteUtc;
        public LabMarker() { }
        public LabMarker(string name, bool present) { Name = name; Present = present; }
        public bool Armed { get { return Present && !string.IsNullOrEmpty(Value); } }
    }

    /// <summary>The installed release, its manifest, and the kernel module that answers now.</summary>
    public sealed class ReleaseView
    {
        public string Version, InstallDir, InstalledUtc;
        public string ManifestKmdAbi, ManifestKmdBuild, ManifestError;
        public uint LiveKmdVersion;         // BC250_KMD_VERSION out of the DPM snapshot; 0 when unknown

        public string LiveText { get { return LiveKmdVersion == 0 ? null : "0x" + LiveKmdVersion.ToString("X8"); } }
    }

    public sealed class MeasurementGuardView
    {
        public RegistryDwords Parameters = new RegistryDwords();
        public InstalledDefaults Installed = new InstalledDefaults();
        public List<LabMarker> Markers = new List<LabMarker>();
        public ReleaseView Release = new ReleaseView();
        public DateTime NowUtc;
    }

    /// <summary>Pure rules, as in OperatingPointRules: a view in, rows out, nothing read from the machine.</summary>
    public static class MeasurementGuardRules
    {
        /// <summary>
        /// Settings whose loss turns the GPU desktop and GPU submission off silently. A zero where the
        /// installer applied a non-zero is red, not amber: every later measurement is then a different machine.
        /// </summary>
        public static readonly string[] LatchedGates =
        { "EnableGpuPresentBlit", "EnableCddDwmInterop", "EnableGpuSubmit", "DpmMode" };

        public static bool IsLatchedGate(string name)
        {
            foreach (string gate in LatchedGates)
                if (string.Equals(gate, name, StringComparison.OrdinalIgnoreCase)) return true;
            return false;
        }

        /// <summary>
        /// What the lab expects of the miniport's `Parameters`: the installer's own record, with the
        /// expectations file on top of it, plus `CuMode`, which no installer table carries. A null expectation
        /// switches that name off.
        /// </summary>
        public static Dictionary<string, int?> Expected(InstalledDefaults installed, LabExpectations x)
        {
            var expected = new Dictionary<string, int?>(StringComparer.OrdinalIgnoreCase);
            if (installed != null && installed.Present)
                foreach (var pair in installed.Parameters) expected[pair.Key] = pair.Value;
            expected["CuMode"] = x.CuMode;
            foreach (var pair in x.Parameters) expected[pair.Key] = pair.Value;
            return expected;
        }

        public static void AddParameterRows(Panel panel, MeasurementGuardView v, LabExpectations x)
        {
            if (!v.Parameters.Present)
            {
                panel.Rows.Add(new Row("Parameters", v.Parameters.Error ?? "no bc250kmd Parameters key", Level.Warn));
                return;
            }
            Dictionary<string, int?> expected = Expected(v.Installed, x);
            var differences = new List<string>();
            Level level = Level.Good;
            int checkedNames = 0;
            foreach (var pair in Sorted(expected))
            {
                if (!pair.Value.HasValue) continue;
                checkedNames++;
                int want = pair.Value.Value;
                int? live = v.Parameters.Get(pair.Key);
                if (live.HasValue && live.Value == want) continue;
                differences.Add(pair.Key + " " + (live.HasValue ? live.Value.ToString(CultureInfo.InvariantCulture) : "absent")
                                + " (expected " + want.ToString(CultureInfo.InvariantCulture) + ")");
                bool latchedOff = IsLatchedGate(pair.Key) && want != 0 && live.HasValue && live.Value == 0;
                if (latchedOff) level = Level.Error;
                else if (level == Level.Good) level = Level.Warn;
            }
            string text;
            if (differences.Count == 0)
                text = checkedNames.ToString(CultureInfo.InvariantCulture)
                       + (checkedNames == 1 ? " value" : " values") + " as expected";
            else
            {
                var shown = differences.Count <= 3 ? differences : differences.GetRange(0, 3);
                text = string.Join("; ", shown.ToArray());
                if (differences.Count > shown.Count)
                    text += "; +" + (differences.Count - shown.Count).ToString(CultureInfo.InvariantCulture) + " more";
            }
            panel.Rows.Add(new Row("Parameters", text, level));
            if (!v.Installed.Present)
                panel.Rows.Add(new Row("Defaults", v.Installed.Error ?? ("no Release\\AppliedDefaults record, "
                                       + (x.FromFile ? "expectations.json only" : "CuMode only")), Level.Warn));
        }

        public static void AddMarkerRows(Panel panel, MeasurementGuardView v)
        {
            var set = new List<string>();
            foreach (var marker in v.Markers)
            {
                if (!marker.Present) continue;
                if (marker.Value == null) { set.Add(marker.Name); continue; }
                if (marker.Value.Length == 0) continue;                 // present but empty: it arms nothing
                string age = v.NowUtc > marker.WriteUtc && marker.WriteUtc != default(DateTime)
                    ? ", " + Age(v.NowUtc - marker.WriteUtc) + " old" : "";
                set.Add(marker.Name + " = " + Short(marker.Value) + age);
            }
            panel.Rows.Add(set.Count == 0
                ? new Row("Markers", "none set", Level.Good)
                : new Row("Markers", string.Join("; ", set.ToArray()), Level.Warn));
        }

        public static void AddReleaseRows(Panel panel, MeasurementGuardView v)
        {
            ReleaseView r = v.Release;
            if (string.IsNullOrEmpty(r.Version))
                panel.Rows.Add(new Row("Release", "no Release\\Version record", Level.Warn));
            else
            {
                string value = r.Version;
                DateTime installed;
                if (!string.IsNullOrEmpty(r.InstalledUtc) &&
                    DateTime.TryParse(r.InstalledUtc, CultureInfo.InvariantCulture,
                                      DateTimeStyles.AdjustToUniversal | DateTimeStyles.AssumeUniversal, out installed) &&
                    v.NowUtc > installed)
                    value += ", installed " + Age(v.NowUtc - installed) + " ago";
                panel.Rows.Add(new Row("Release", value));
            }

            string live = r.LiveText;
            if (live == null || string.IsNullOrEmpty(r.ManifestKmdAbi))
            {
                string why = live == null ? "live KMD version unknown"
                    : r.ManifestError ?? "no manifest kmd_abi";
                panel.Rows.Add(new Row("KMD image", why, Level.Warn));
                return;
            }
            bool same = string.Equals(live, r.ManifestKmdAbi, StringComparison.OrdinalIgnoreCase);
            panel.Rows.Add(new Row("KMD image", same
                ? live + " = the release's " + (r.ManifestKmdBuild ?? r.ManifestKmdAbi)
                : "live " + live + ", release " + r.ManifestKmdAbi +
                  (string.IsNullOrEmpty(r.ManifestKmdBuild) ? "" : " (" + r.ManifestKmdBuild + ")") +
                  ": a module was swapped",
                same ? Level.Good : Level.Error));
        }

        public static List<Row> Rows(MeasurementGuardView v, LabExpectations x)
        {
            var panel = new Panel();
            AddParameterRows(panel, v, x);
            AddMarkerRows(panel, v);
            AddReleaseRows(panel, v);
            return panel.Rows;
        }

        /// <summary>The state this panel logs once. Ages and clock values are left out on purpose.</summary>
        public static string StateKey(MeasurementGuardView v, LabExpectations x)
        {
            var parts = new List<string>();
            var differences = new List<string>();
            foreach (var pair in Sorted(Expected(v.Installed, x)))
            {
                if (!pair.Value.HasValue) continue;
                int? live = v.Parameters.Get(pair.Key);
                if (live.HasValue && live.Value == pair.Value.Value) continue;
                differences.Add(pair.Key + "=" + (live.HasValue ? live.Value.ToString(CultureInfo.InvariantCulture) : "-"));
            }
            parts.Add("parameters=" + (!v.Parameters.Present ? "absent"
                : differences.Count == 0 ? "as expected" : string.Join(",", differences.ToArray())));
            var markers = new List<string>();
            foreach (var marker in v.Markers)
                if (marker.Present && (marker.Value == null || marker.Value.Length > 0)) markers.Add(marker.Name);
            parts.Add("markers=" + (markers.Count == 0 ? "none" : string.Join(",", markers.ToArray())));
            parts.Add("release=" + (v.Release.Version ?? "-") + " kmd=" + (v.Release.LiveText ?? "-")
                      + "/" + (v.Release.ManifestKmdAbi ?? "-"));
            return string.Join("; ", parts.ToArray());
        }

        public static Level Worst(IEnumerable<Row> rows)
        {
            Level worst = Level.Good;
            foreach (var row in rows) if (row.Level > worst) worst = row.Level;
            return worst;
        }

        static List<KeyValuePair<string, int?>> Sorted(Dictionary<string, int?> table)
        {
            var list = new List<KeyValuePair<string, int?>>(table);
            list.Sort((a, b) => string.Compare(a.Key, b.Key, StringComparison.OrdinalIgnoreCase));
            return list;
        }

        public static string Short(string value)
        {
            string one = (value ?? "").Replace("\r", " ").Replace("\n", " ").Trim();
            return one.Length <= 28 ? one : one.Substring(0, 27) + "~";
        }

        public static string Age(TimeSpan span)
        {
            if (span.TotalMinutes < 1) return span.TotalSeconds.ToString("0", CultureInfo.InvariantCulture) + " s";
            if (span.TotalHours < 1) return span.TotalMinutes.ToString("0", CultureInfo.InvariantCulture) + " min";
            if (span.TotalDays < 1) return span.TotalHours.ToString("0.0", CultureInfo.InvariantCulture) + " h";
            return span.TotalDays.ToString("0.0", CultureInfo.InvariantCulture) + " d";
        }
    }

    public sealed class MeasurementGuardProvider : IProvider
    {
        public const string ReleaseKey = @"SOFTWARE\amdgpu-wddm\Release";
        // Lab files that change what the next game process does, machine-wide and silently
        // (scratch\m15\native-caps001\templates\game-runtime.ps1 and preflight-template.ps1).
        public const string PerftestMarkerPath = @"C:\BC250\tools\radv-perftest.txt";
        public const string IcdConfigPath = @"C:\BC250\tmp\amdgpu_wddm_radv.cfg";

        readonly string _dataDir;
        readonly DpmFeed _dpm;
        readonly ExpectationsFile _expectations;
        readonly FileCache _perftest, _icdConfig, _manifest;
        string _manifestAbi, _manifestBuild, _manifestError;
        string _lastKey;

        public MeasurementGuardProvider(string dataDir, DpmFeed dpm)
        {
            _dataDir = dataDir ?? ".";
            _dpm = dpm;
            _expectations = new ExpectationsFile(dataDir);
            _perftest = new FileCache(PerftestMarkerPath);
            _icdConfig = new FileCache(IcdConfigPath);
            _manifest = new FileCache(null);
        }

        public string Name { get { return "guard"; } }
        public TimeSpan Period { get { return TimeSpan.FromSeconds(10); } }

        public void Poll(State state)
        {
            var panel = new Panel { Name = Name, Title = "Measurement guard", Order = 16 };
            LabExpectations expectations = _expectations.Read();
            var view = new MeasurementGuardView
            {
                NowUtc = DateTime.UtcNow,
                Parameters = RegistryDwords.Read(Registry.LocalMachine, OperatingPointProvider.ParametersKey),
                Markers = ReadMarkers(),
                Release = ReadRelease(),
            };
            view.Installed = InstalledDefaults.Parse(ReadString(ReleaseKey, "AppliedDefaults"));

            List<Row> rows = MeasurementGuardRules.Rows(view, expectations);
            foreach (var row in rows) panel.Rows.Add(row);
            panel.Rows.Add(new Row("Sampled", DateTime.Now.ToString("HH:mm:ss") + " (10 s poll)"));
            state.SetPanel(panel);

            string key = MeasurementGuardRules.StateKey(view, expectations);
            if (key == _lastKey) return;
            _lastKey = key;
            state.Log(Name, MeasurementGuardRules.Worst(rows), key);
        }

        List<LabMarker> ReadMarkers()
        {
            var markers = new List<LabMarker>
            {
                Marker("STOP", State.StopFileName),
                Marker("graphics-summary.pause", GraphicsPipelineProvider.SummaryPauseFileName),
                Marker("graphics-api.pause", GraphicsApiProvider.PauseFileName),
                Marker("graphics-api.skip", GraphicsApiProvider.SkipFileName),
            };
            markers.Add(ContentMarker("radv-perftest.txt", _perftest));
            markers.Add(ContentMarker("radv.cfg", _icdConfig));
            return markers;
        }

        LabMarker Marker(string name, string fileName)
        {
            try { return new LabMarker(name, File.Exists(System.IO.Path.Combine(_dataDir, fileName))); }
            catch { return new LabMarker(name, false); }
        }

        static LabMarker ContentMarker(string name, FileCache cache)
        {
            cache.Refresh();
            return new LabMarker { Name = name, Present = cache.Present, Value = cache.Text ?? "", WriteUtc = cache.WriteUtc };
        }

        ReleaseView ReadRelease()
        {
            var release = new ReleaseView
            {
                Version = ReadString(ReleaseKey, "Version"),
                InstallDir = ReadString(ReleaseKey, "InstallDir"),
                InstalledUtc = ReadString(ReleaseKey, "InstalledUtc"),
            };
            DpmView dpm = _dpm != null ? _dpm.Read() : new DpmView();
            if (dpm.Have) release.LiveKmdVersion = dpm.Snapshot.Version;
            if (string.IsNullOrEmpty(release.InstallDir)) { release.ManifestError = "no InstallDir record"; return release; }
            // The release manifest is about 37 kB and it changes once per install, so it is parsed only when
            // its write time changed; every other poll reuses the two strings taken out of it.
            _manifest.Path = System.IO.Path.Combine(release.InstallDir, "manifest.json");
            _manifest.Refresh(1 << 20);
            if (_manifest.Changed)
            {
                _manifestAbi = _manifestBuild = _manifestError = null;
                try
                {
                    var root = new JavaScriptSerializer().Deserialize<Dictionary<string, object>>(_manifest.Text);
                    object abi, build;
                    if (root != null && root.TryGetValue("kmd_abi", out abi)) _manifestAbi = Convert.ToString(abi);
                    if (root != null && root.TryGetValue("kmd_build", out build)) _manifestBuild = Convert.ToString(build);
                }
                catch (Exception e) { _manifestError = "manifest.json: " + e.Message; }
            }
            if (!_manifest.Present)
            {
                _manifestAbi = _manifestBuild = null;
                release.ManifestError = _manifest.Error ?? ("no manifest.json in " + release.InstallDir);
                return release;
            }
            release.ManifestKmdAbi = _manifestAbi;
            release.ManifestKmdBuild = _manifestBuild;
            release.ManifestError = _manifestError;
            return release;
        }

        static string ReadString(string subKey, string name)
        {
            try
            {
                using (var key = Registry.LocalMachine.OpenSubKey(subKey, false))
                    return key == null ? null : key.GetValue(name) as string;
            }
            catch { return null; }
        }
    }

    /// <summary>
    /// A file read again only when its path or write time changed, so a poll normally costs one stat call.
    /// `Changed` says that this refresh re-read the file, which is when a caller has to parse it again.
    /// </summary>
    public sealed class FileCache
    {
        public string Path;
        public bool Present;
        public string Text;
        public DateTime WriteUtc;
        public bool Changed;
        public string Error;
        DateTime _read;
        string _readPath;

        public FileCache(string path) { Path = path; }

        public void Refresh() { Refresh(64 * 1024); }

        public void Refresh(int maxBytes)
        {
            Changed = false;
            try
            {
                if (string.IsNullOrEmpty(Path)) { Clear(); return; }
                var info = new FileInfo(Path);
                if (!info.Exists) { Clear(); _readPath = Path; return; }
                if (Present && _readPath == Path && _read == info.LastWriteTimeUtc) return;
                Present = true;
                Changed = true;
                Error = null;
                WriteUtc = info.LastWriteTimeUtc;
                _read = WriteUtc;
                _readPath = Path;
                // A file far larger than expected is not read: an operator's mistake must not cost a frame.
                Text = info.Length > maxBytes ? "" : File.ReadAllText(Path).Trim();
            }
            catch (Exception e) { Clear(); Error = e.Message; }
        }

        // A file we cannot read is never reported as armed.
        void Clear()
        {
            Present = false;
            Text = null;
            WriteUtc = default(DateTime);
            _read = default(DateTime);
            _readPath = null;
        }
    }
}
