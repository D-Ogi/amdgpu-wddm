// The Driver card (WU-043, GUI plan v7 F-VER and G-VER): the running release, an installed release that waits for the
// restart, and the verification outcome, each from its own source and never inferred from another.
//
//   Running release   exact ONLY with a running-release witness of this boot (docs/gui/interfaces.md section 1):
//                     its BootId equals the current one, its kmd_abi and kmd_build equal the driver's reply, and no
//                     install action is recorded after it in this boot. Otherwise "cannot be determined exactly"
//                     (unknown, ambiguous or a single unwitnessed candidate); the candidates go to the report only.
//                     Never the newest matching release, never Release\Version.
//   Installed         HKLM\SOFTWARE\amdgpu-wddm\Release\Version with the installer's phase: 'installed' or
//                     'driver-pending-restart' = waiting for the restart.
//   Verification      phase 'verified' / 'verify-failed' and verify-*.json of the installed version.
// Dates: the INF DriverVer date is "Driver date"; the GitHub publication date is "Released", only after a check.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Web.Script.Serialization;
using Microsoft.Win32;

namespace AmdgpuWddmControl
{
    public enum RunningKind { NoDriver, Exact, Unknown, Unwitnessed, Ambiguous }
    public enum VerifyKind { Verified, Failed, Unknown }

    public sealed class ReleaseWitness
    {
        public long BootId;
        public string RecordedUtc, RecordedBy, Release, Version, ManifestSha256, KmdImageSha256, KmdBuild, KmdAbi;
    }

    public sealed class InstallerState
    {
        public string Phase, PackageVersion, MutationUtc, UpdatedUtc;
        public long? MutationBootId;
    }

    public sealed class KnownPackage
    {
        public string Version, KmdBuild, KmdAbi, Source;
    }

    public sealed class VerifyReport
    {
        public string Utc, PackageVersion;
        public int Passed, Failed;
    }

    public sealed class DriverFacts
    {
        public uint? ReplyVersion;              // BC250_KMD_VERSION of a driver reply; null: no reply
        public long? BootId;
        public ReleaseWitness Witness;          // null: none, damaged, untrusted or unknown schema
        public InstallerState State;            // null: no state.json
        public string InstalledVersion;         // Release\Version; null when missing
        public List<KnownPackage> Packages = new List<KnownPackage>();
        public List<VerifyReport> Reports = new List<VerifyReport>();
        public string DriverDate;               // INF DriverVer date as Windows stores it (M-D-YYYY)
        public string WitnessNote;              // why a present witness was not accepted (report only)
    }

    public sealed class DriverCardView
    {
        public RunningKind Running;
        public string RunningVersion;           // Exact only
        public List<string> Candidates = new List<string>();
        public string InstalledVersion;
        public bool InstalledPending, InstallStopped;
        public VerifyKind Verification;
        public string RunningText, InstalledText, VerificationText, DriverDateText;
        public string ReportText;               // technical detail, support report only
    }

    public static class DriverCard
    {
        // kmd_abi: "0x000700C7" or decimal; null when it does not parse.
        public static uint? ParseAbi(string text)
        {
            if (string.IsNullOrEmpty(text)) return null;
            uint v;
            if (text.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
                return uint.TryParse(text.Substring(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out v) ? (uint?)v : null;
            return uint.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out v) ? (uint?)v : null;
        }

        // kmd_build "0.7.199.1": its first three parts against the reply's major.minor.revision.
        public static bool BuildMatches(string kmdBuild, uint reply)
        {
            var parts = (kmdBuild ?? "").Split('.');
            uint a, b, c;
            return parts.Length >= 3 && uint.TryParse(parts[0], NumberStyles.None, CultureInfo.InvariantCulture, out a) &&
                uint.TryParse(parts[1], NumberStyles.None, CultureInfo.InvariantCulture, out b) && uint.TryParse(parts[2], NumberStyles.None, CultureInfo.InvariantCulture, out c) &&
                a == reply >> 24 && b == ((reply >> 16) & 0xFF) && c == (reply & 0xFFFF);
        }

        static DateTime? Utc(string s) { return Recovery.Utc(s); }

        // Why the witness is not the exact running release; null when it is.
        public static string WitnessProblem(DriverFacts f)
        {
            var w = f.Witness;
            if (w == null) return "no running-release witness";
            if (f.BootId == null) return "the current boot cannot be read";
            if (w.BootId != f.BootId.Value) return "the witness is from another boot (" + w.BootId + ", now " + f.BootId + ")";
            if (f.ReplyVersion == null) return "no driver reply";
            if (ParseAbi(w.KmdAbi) != f.ReplyVersion.Value) return "the witness names kmd_abi " + w.KmdAbi + ", the driver replies " + KmdReply.VersionText(f.ReplyVersion.Value);
            if (!BuildMatches(w.KmdBuild, f.ReplyVersion.Value)) return "the witness names kmd_build " + w.KmdBuild + ", the driver replies " + KmdReply.VersionText(f.ReplyVersion.Value);
            if (Utc(w.RecordedUtc) == null) return "the witness has no valid time";
            var s = f.State;
            if (s != null && s.MutationBootId == f.BootId && (Utc(s.MutationUtc) == null || Utc(s.MutationUtc) > Utc(w.RecordedUtc)))
                return "an install action at " + (s.MutationUtc ?? "an unknown time") + " came after the witness in this boot";
            if (string.IsNullOrEmpty(w.Version)) return "the witness names no version";
            return null;
        }

        public static DriverCardView Decide(DriverFacts f)
        {
            var v = new DriverCardView();
            var problem = WitnessProblem(f);
            if (problem == null) { v.Running = RunningKind.Exact; v.RunningVersion = f.Witness.Version; }
            else if (f.ReplyVersion == null) v.Running = RunningKind.NoDriver;
            else
            {
                v.Candidates = f.Packages.Where(p => ParseAbi(p.KmdAbi) == f.ReplyVersion.Value && BuildMatches(p.KmdBuild, f.ReplyVersion.Value))
                    .Select(p => p.Version).Where(x => !string.IsNullOrEmpty(x)).Distinct(StringComparer.OrdinalIgnoreCase).OrderBy(x => x, StringComparer.OrdinalIgnoreCase).ToList();
                v.Running = v.Candidates.Count == 0 ? RunningKind.Unknown : v.Candidates.Count == 1 ? RunningKind.Unwitnessed : RunningKind.Ambiguous;
            }

            v.InstalledVersion = string.IsNullOrEmpty(f.InstalledVersion) ? null : f.InstalledVersion;
            var phase = f.State != null && f.State.PackageVersion == v.InstalledVersion ? f.State.Phase : null;
            v.InstalledPending = v.InstalledVersion != null && (phase == "installed" || phase == "driver-pending-restart");
            v.InstallStopped = f.State != null && f.State.Phase == "install-incomplete";

            // Verification of the installed release.
            var report = f.Reports.Where(r => v.InstalledVersion != null && r.PackageVersion == v.InstalledVersion && Utc(r.Utc) != null)
                .OrderByDescending(r => Utc(r.Utc)).FirstOrDefault();
            if (phase == "verified" && (report == null || report.Failed == 0)) v.Verification = VerifyKind.Verified;
            else if (phase == "verify-failed" || (report != null && report.Failed > 0)) v.Verification = VerifyKind.Failed;
            else if (phase == null && report != null && report.Failed == 0 && report.Passed > 0) v.Verification = VerifyKind.Verified;
            else v.Verification = VerifyKind.Unknown;

            switch (v.Running)
            {
                case RunningKind.Exact: v.RunningText = Strings.T("drv.running.exact", v.RunningVersion); break;
                case RunningKind.NoDriver: v.RunningText = Strings.T("drv.running.none"); break;
                default: v.RunningText = Strings.T("drv.running.unknown"); break;
            }
            v.InstalledText = v.InstalledVersion == null ? Strings.T("drv.installed.none")
                : v.InstalledPending ? Strings.T("drv.installed.pending", v.InstalledVersion) : Strings.T("drv.installed", v.InstalledVersion);
            if (v.InstallStopped) v.InstalledText += " " + Strings.T("drv.installed.stopped");
            v.VerificationText = Strings.T(v.Verification == VerifyKind.Verified ? "drv.verify.ok" : v.Verification == VerifyKind.Failed ? "drv.verify.failed" : "drv.verify.unknown");
            var date = InfDate(f.DriverDate);
            v.DriverDateText = date == null ? Strings.T("drv.date.none") : Strings.T("drv.date", date.Value.ToString("yyyy-MM-dd", CultureInfo.InvariantCulture));

            v.ReportText = "running release: " + v.Running + (v.RunningVersion != null ? " " + v.RunningVersion : "") + (problem != null ? " (" + problem + ")" : " (witness of this boot)") +
                (v.Candidates.Count > 0 ? "; candidates sharing the driver identity: " + string.Join(", ", v.Candidates) : "") +
                "; driver reply " + (f.ReplyVersion != null ? KmdReply.VersionText(f.ReplyVersion.Value) + " (0x" + f.ReplyVersion.Value.ToString("X8", CultureInfo.InvariantCulture) + ")" : "none") +
                "; installed " + (v.InstalledVersion ?? "none") + ", phase " + (f.State != null ? f.State.Phase + " for " + f.State.PackageVersion : "none") +
                "; verification " + v.Verification + (report != null ? " (" + report.Utc + ": " + report.Passed + " passed, " + report.Failed + " failed)" : "") +
                (f.WitnessNote != null ? "; " + f.WitnessNote : "");
            return v;
        }

        // Windows stores DriverDate as M-D-YYYY.
        public static DateTime? InfDate(string text)
        {
            DateTime d;
            return DateTime.TryParseExact(text ?? "", new[] { "M-d-yyyy", "MM-dd-yyyy", "M/d/yyyy" }, CultureInfo.InvariantCulture, DateTimeStyles.None, out d) ? (DateTime?)d : null;
        }

        // "upgrade done" (section 6 rank 5): the installed release verified, and not the one the app showed last.
        public static bool UpgradeDone(DriverCardView v, string lastSeenRelease)
        {
            return v.Verification == VerifyKind.Verified && v.InstalledVersion != null && !v.InstalledPending &&
                lastSeenRelease != null && !string.Equals(lastSeenRelease, v.InstalledVersion, StringComparison.OrdinalIgnoreCase);
        }

        // ---- parsing (pure) ----------------------------------------------------------------------------------------

        static IDictionary<string, object> Json(string text)
        {
            try { return new JavaScriptSerializer { MaxJsonLength = 4 << 20 }.DeserializeObject(text ?? "") as IDictionary<string, object>; }
            catch (Exception) { return null; }
        }

        static string S(IDictionary<string, object> d, string k)
        {
            object v;
            return d != null && d.TryGetValue(k, out v) && v is string ? (string)v : null;
        }

        static long? N(IDictionary<string, object> d, string k)
        {
            object v;
            if (d == null || !d.TryGetValue(k, out v) || v == null) return null;
            if (v is int) return (int)v;
            if (v is long) return (long)v;
            return null;
        }

        public static ReleaseWitness ParseWitness(string text)
        {
            var d = Json(text);
            if (d == null || N(d, "schema") != 1 || N(d, "boot_id") == null) return null;
            var by = S(d, "recorded_by");
            if (by != "verify" && by != "start-confirm") return null;
            return new ReleaseWitness
            {
                BootId = N(d, "boot_id").Value, RecordedUtc = S(d, "recorded_utc"), RecordedBy = by, Release = S(d, "release"), Version = S(d, "version"),
                ManifestSha256 = S(d, "manifest_sha256"), KmdImageSha256 = S(d, "kmd_image_sha256"), KmdBuild = S(d, "kmd_build"), KmdAbi = S(d, "kmd_abi"),
            };
        }

        public static InstallerState ParseState(string text)
        {
            var d = Json(text);
            if (d == null) return null;
            return new InstallerState { Phase = S(d, "phase"), PackageVersion = S(d, "package_version"), UpdatedUtc = S(d, "updated_utc"),
                MutationBootId = N(d, "mutation_boot_id"), MutationUtc = S(d, "mutation_utc") };
        }

        public static VerifyReport ParseVerify(string text)
        {
            var d = Json(text);
            if (d == null) return null;
            object results;
            var r = new VerifyReport { Utc = S(d, "utc"), PackageVersion = S(d, "package_version") };
            if (d.TryGetValue("results", out results) && results is object[])
                foreach (var o in (object[])results)
                {
                    var x = o as IDictionary<string, object>;
                    object pass;
                    if (x != null && x.TryGetValue("pass", out pass) && pass is bool) { if ((bool)pass) r.Passed++; else r.Failed++; }
                    else r.Failed++;      // a result without a pass flag is not a pass
                }
            return r;
        }

        public static KnownPackage ParsePackage(string manifestJson, string source)
        {
            try
            {
                var m = ManifestCheck.Parse(manifestJson);
                return new KnownPackage { Version = m.Version, KmdBuild = m.KmdBuild, KmdAbi = m.KmdAbi, Source = source };
            }
            catch (Exception) { return null; }
        }
    }

    // The I/O of the Driver card: reads only; files must be owned by Administrators or SYSTEM.
    public static class DriverCardProbe
    {
        static string Installer
        {
            get
            {
                var over = Environment.GetEnvironmentVariable("AMDGPU_WDDM_CONTROL_INSTALLER");
                return !string.IsNullOrEmpty(over) ? over : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "amdgpu-wddm", "installer");
            }
        }

        static bool Trusted(string path)
        {
            if (Environment.GetEnvironmentVariable("AMDGPU_WDDM_CONTROL_INSTALLER") != null) return true;    // the build's fixtures
            try
            {
                var owner = File.GetAccessControl(path, AccessControlSections.Owner).GetOwner(typeof(SecurityIdentifier));
                return owner.Equals(new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null)) || owner.Equals(new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null));
            }
            catch (Exception) { return false; }
        }

        static string ReadSmall(string path)
        {
            try { return File.Exists(path) && new FileInfo(path).Length <= 4 << 20 && Trusted(path) ? File.ReadAllText(path) : null; }
            catch (Exception) { return null; }
        }

        public static DriverFacts Read(uint? replyVersion, long? bootId, string installedVersion, string installDir, string driverDate)
        {
            var f = new DriverFacts { ReplyVersion = replyVersion, BootId = bootId, InstalledVersion = installedVersion, DriverDate = driverDate };
            var witnessPath = Path.Combine(Installer, "running-release.json");
            var text = ReadSmall(witnessPath);
            f.Witness = text != null ? DriverCard.ParseWitness(text) : null;
            if (text == null && File.Exists(witnessPath)) f.WitnessNote = "running-release.json is not owned by Administrators or SYSTEM, or unreadable";
            else if (text != null && f.Witness == null) f.WitnessNote = "running-release.json is damaged or of an unknown schema";
            var state = ReadSmall(Path.Combine(Installer, "state.json"));
            f.State = state != null ? DriverCard.ParseState(state) : null;
            try
            {
                var dir = Path.Combine(Installer, "verify");
                if (Directory.Exists(dir))
                    foreach (var file in Directory.GetFiles(dir, "verify-*.json").OrderByDescending(x => x, StringComparer.OrdinalIgnoreCase).Take(20))
                    {
                        var r = DriverCard.ParseVerify(ReadSmall(file));
                        if (r != null) f.Reports.Add(r);
                    }
            }
            catch (Exception) { }
            var manifests = new List<string>();
            if (!string.IsNullOrEmpty(installDir)) manifests.Add(Path.Combine(installDir, "manifest.json"));
            try
            {
                var kept = Path.Combine(Installer, "packages");
                if (Directory.Exists(kept)) manifests.AddRange(Directory.GetDirectories(kept).Select(d => Path.Combine(d, "manifest.json")));
            }
            catch (Exception) { }
            foreach (var m in manifests)
            {
                var t = ReadSmall(m);
                var p = t != null ? DriverCard.ParsePackage(t, m) : null;
                if (p != null) f.Packages.Add(p);
            }
            return f;
        }

        public static long? BootId()
        {
            try
            {
                using (var k = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management\PrefetchParameters"))
                    return k != null && k.GetValue("BootId") is int ? (long?)(uint)(int)k.GetValue("BootId") : null;
            }
            catch (Exception) { return null; }
        }
    }
}
