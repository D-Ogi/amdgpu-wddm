// Shared, cheap readers for the lab state that decides a measurement.
//
// Why this file exists: between 2026-10-02 and 2026-10-05 the lab ran 24 compute units instead of 40, because
// the tester release installs do not carry `CuMode` and nothing on the screen said so. Every Witcher 3 and
// Rise of the Tomb Raider number of those three days is a 24 CU number. The rule that follows from it: anything
// that silently changes a measurement or the lab's operating point belongs on the overlay, amber as soon as it
// differs from what the lab expects.
//
// Everything here is a registry read, a file-existence test or a snapshot another provider already took. The
// cost limit is the one the README states: a KMD escape poll every 5 s once cost a game 300 ms, so this file
// reads no escape of its own, and the two providers built on it poll every 10 s.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Web.Script.Serialization;
using Microsoft.Win32;

namespace Bc250Mon
{
    /// <summary>
    /// What the lab expects of itself, from `expectations.json` in the monitor data directory. The file is
    /// optional: without it the expectation is 40 compute units and the installer's own record of the applied
    /// defaults (`HKLM\SOFTWARE\amdgpu-wddm\Release\AppliedDefaults`).
    /// </summary>
    public sealed class LabExpectations
    {
        public const int DefaultCuMode = 40;        // all five WGPs of every shader array (docs/design/cu-mode.md)

        public int CuMode = DefaultCuMode;
        /// <summary>
        /// Expected REG_DWORDs of the miniport's `Parameters` key, over and above the installer's record. A
        /// null value switches a check off, which is how a deliberate lab deviation is admitted.
        /// </summary>
        public Dictionary<string, int?> Parameters = new Dictionary<string, int?>(StringComparer.OrdinalIgnoreCase);
        public bool FromFile;                       // the file was read (so the operator sees which rules apply)
        public string Error;                        // the file is there but does not parse
    }

    /// <summary>Reader of `expectations.json`, re-read only when its write time changes.</summary>
    public sealed class ExpectationsFile
    {
        public const string FileName = "expectations.json";
        readonly string _path;
        DateTime _write;
        long _length = -1;
        bool _read;
        LabExpectations _current = new LabExpectations();

        public ExpectationsFile(string dataDir) { _path = System.IO.Path.Combine(dataDir ?? ".", FileName); }
        public string FilePath { get { return _path; } }

        // Keyed on the write time and the length together: two writes inside one timestamp tick are rare, but
        // a missed edit of this file would silently change which rules the panels apply.
        public LabExpectations Read()
        {
            try
            {
                var info = new FileInfo(_path);
                bool exists = info.Exists;
                DateTime write = exists ? info.LastWriteTimeUtc : default(DateTime);
                long length = exists ? info.Length : -1;
                if (_read && write == _write && length == _length) return _current;
                // The keys are stored after the read succeeded, never before it: a transient failure (the
                // operator's editor holding the file) must be retried on the next poll, not latched until the
                // file's write time or length changes again.
                LabExpectations fresh = exists ? Parse(File.ReadAllText(_path)) : new LabExpectations();
                _write = write;
                _length = length;
                _read = true;
                _current = fresh;
            }
            catch (Exception e) { _current = new LabExpectations { FromFile = true, Error = e.Message }; }
            return _current;
        }

        /// <summary>
        /// `{"schema": 1, "cuMode": 40, "parameters": {"DpmMaxMHz": 1500, "KeepLog": null}}`. An unreadable
        /// file never hides the defaults: the expectations come back with their default values and an Error.
        /// </summary>
        public static LabExpectations Parse(string json)
        {
            var x = new LabExpectations { FromFile = true };
            try
            {
                var root = new JavaScriptSerializer().Deserialize<Dictionary<string, object>>(json);
                if (root == null) { x.Error = "empty"; return x; }
                object cu;
                if (root.TryGetValue("cuMode", out cu) && cu != null)
                    x.CuMode = Convert.ToInt32(cu, CultureInfo.InvariantCulture);
                object parameters;
                if (root.TryGetValue("parameters", out parameters))
                {
                    var table = parameters as Dictionary<string, object>;
                    if (table != null)
                        foreach (var pair in table)
                            x.Parameters[pair.Key] = pair.Value == null ? (int?)null
                                : Convert.ToInt32(pair.Value, CultureInfo.InvariantCulture);
                }
            }
            catch (Exception e) { x.Error = e.Message; }
            return x;
        }
    }

    /// <summary>One read of a registry key's REG_DWORD values. Other value kinds are ignored.</summary>
    public sealed class RegistryDwords
    {
        public bool Present;
        public string Error;
        public Dictionary<string, int> Values = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);

        public int? Get(string name)
        {
            int v;
            return Values.TryGetValue(name, out v) ? (int?)v : null;
        }

        public static RegistryDwords Read(RegistryKey hive, string subKey)
        {
            var result = new RegistryDwords();
            try
            {
                using (var key = hive.OpenSubKey(subKey, false))
                {
                    if (key == null) return result;
                    result.Present = true;
                    foreach (string name in key.GetValueNames())
                    {
                        object value = key.GetValue(name);
                        if (value is int) result.Values[name] = (int)value;
                    }
                }
            }
            catch (Exception e) { result.Error = e.Message; }
            return result;
        }
    }

    /// <summary>One DPM snapshot out of the DpmFeed, with its age.</summary>
    public sealed class DpmView
    {
        public bool Have;
        public DpmSnapshot Snapshot;
        public DateTime Utc;
        public string Error;

        public const uint FlagRunning = 1, FlagGoverning = 2, FlagPending = 4, FlagConfirmed = 8,
                          FlagPaused = 16, FlagStable = 32, FlagSession = 64;
        public const uint ModeFixed = 0, ModeDpm = 1;
        public const int StaleSeconds = 30;         // the feed samples 4 times a second: older than this is stale
    }

    /// <summary>
    /// The last DPM snapshot TelemetryProvider read, so the operating-point panel costs no escape of its own.
    /// The last good snapshot is kept after a failure and its age is reported, never hidden.
    /// </summary>
    public sealed class DpmFeed
    {
        readonly object _lock = new object();
        readonly Func<DateTime> _clock;
        bool _have;
        DpmSnapshot _last;
        DateTime _utc;
        string _error;

        public DpmFeed() : this(() => DateTime.UtcNow) { }
        public DpmFeed(Func<DateTime> clock) { _clock = clock; }

        public void Publish(DpmSnapshot s)
        {
            lock (_lock) { _have = true; _last = s; _utc = _clock(); _error = null; }
        }
        public void Fail(string error) { lock (_lock) _error = error; }

        public DpmView Read()
        {
            lock (_lock)
                return new DpmView { Have = _have, Snapshot = _last, Utc = _utc, Error = _error };
        }
    }

    /// <summary>Names mirrored from the driver's enumerations. `test_lab_state.py` fails the build on a drift.</summary>
    public static class LabNames
    {
        // enum bc250_cu_reason, driver/shim/include/bc250_cu_mode.h.
        public static readonly string[] CuReason =
        {
            "none",                                 // NONE
            "CuMode is neither 24 nor 40",          // INVALID_SETTING
            "CuDisableWgp outside the part",        // INVALID_DISABLE
            "an earlier 40 CU start was never confirmed",    // PENDING_UNCONFIRMED
            "the pending mark could not be written",// REGISTRY
            "not a 1002:13FE",                      // NOT_THIS_DEVICE
            "RLC power gating is on",               // POWER_GATING
            "stock registers not as measured",      // STOCK_UNEXPECTED
            "a written value did not read back",    // READBACK
            "stock did not read back either",       // RESTORE_FAILED
            "GFX bring-up stopped before the stage",// NOT_RUN
            "topology outside what this supports",  // TOPOLOGY
        };
        // enum bc250_dpm_throttle, driver/shim/include/bc250_dpm.h.
        public static readonly string[] DpmThrottle =
        {
            "none",             // NONE
            "thermal-soft",     // THERMAL_SOFT, stepped down at the hot limit
            "thermal-hard",     // THERMAL_HARD, at the thermal floor
            "sensor",           // SENSOR, no temperature reading
            "max-setting",      // MAX_SETTING, DpmMaxMHz
            "stable",           // STABLE, SetStablePowerState pins the floor
            "smu",              // SMU, the governor stopped after SMU failures
            "fixed",            // FIXED, this start is fixed-lab
            "thermal-warm",     // THERMAL_WARM, a raise refused in the warm zone
            "thermal-ramp",     // THERMAL_RAMP, a raise cut to one level or held
        };
        public static string CuReasonName(uint reason)
        {
            return reason < CuReason.Length ? CuReason[reason] : "reason " + reason;
        }
        public static string DpmThrottleName(uint throttle)
        {
            return throttle < DpmThrottle.Length ? DpmThrottle[throttle] : "throttle " + throttle;
        }
        /// <summary>BC250_INTEROP_SWITCH_BLIT | BC250_INTEROP_SWITCH_CDD as the operator reads them.</summary>
        public static string InteropSwitches(int bits)
        {
            if (bits == 0) return "none";
            var parts = new List<string>();
            if ((bits & 1) != 0) parts.Add("blit");
            if ((bits & 2) != 0) parts.Add("cdd");
            if ((bits & ~3) != 0) parts.Add("0x" + (bits & ~3).ToString("X", CultureInfo.InvariantCulture));
            return string.Join("+", parts.ToArray());
        }
        // g_InteropReason of driver/kmd/interop_policy.c, enum bc250_interop_reason. Two numbers are unused: the
        // enumeration is numbered like enum bc250_dpm_reason where the meaning is the same.
        public static readonly string[] InteropReason =
        { "none", "not-requested", "invalid-setting", "unused", "unclean", "registry", "unused", "not-run" };
        public static string InteropReasonName(uint reason)
        {
            return reason < InteropReason.Length ? InteropReason[reason]
                : "reason " + reason.ToString(CultureInfo.InvariantCulture);
        }
        // BC250_INTEROP_END_*, bc250kmd_escape.h.
        public static string InteropEnd(int end)
        {
            switch (end)
            {
                case 0: return "none";
                case 1: return "device stop";
                case 2: return "last user gone";
                case 3: return "system power";
                case 4: return "adapter D3";
                default: return "end " + end.ToString(CultureInfo.InvariantCulture);
            }
        }
    }
}
