// The Driver card (WU-043, GUI plan v7 F-VER and G-VER, plan 497 decisions 3 and 5-8): the running release, an
// installed release that waits for the restart, and the verification outcome, each from its own source and never
// inferred from another. The reader rules are docs/gui/interfaces.md sections 0-2.
//
//   Running release   exact ONLY when a complete running-release witness of this boot, written by the engine's verify
//                     step or the start-confirm task, names the driver's reply ABI, is not followed by an install
//                     action of this boot, is bound to exactly one known package by manifest SHA256, release, version,
//                     full kmd_build, kmd_abi and KMD image SHA256 (a complete manifest mapping), and the app's own
//                     loaded-image evidence of this boot has the same image SHA256 and build. Otherwise "cannot be
//                     determined exactly": ambiguous when the mapping is complete and two or more packages share the
//                     reply ABI, unknown in every other case. The reply carries only the ABI (decision 6); a build or a
//                     release is never derived from it. Never the newest matching release, never Release\Version.
//   Installed         HKLM\SOFTWARE\amdgpu-wddm\Release\Version only (decision 7); the installer's phase says only
//                     whether it waits for the restart or the install stopped.
//   Verification      only a valid verify-*.json of the installed package (version and manifest SHA256): passed ->
//                     verified, failed -> not verified, none -> could not be checked (decision 8). Never the phase.
// Dates: the INF DriverVer date is "Driver date"; the GitHub publication date is "Released", only after a check.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.AccessControl;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text;
using System.Text.RegularExpressions;
using System.Web.Script.Serialization;
using Microsoft.Win32;

namespace AmdgpuWddmControl
{
    public enum RunningKind { Exact, Unknown, Ambiguous }
    public enum VerifyKind { Verified, Failed, Unknown }
    public enum MappingStatus { Complete, Absent, Incomplete, Conflicting }

    public sealed class ReleaseWitness
    {
        public long BootId;
        public string RecordedUtc, RecordedBy, Release, Version, ManifestSha256, KmdImageSha256, KmdBuild, KmdAbi;
    }

    // One recorded install action (state.json mutation_boot_id / mutation_utc).
    public sealed class InstallAction
    {
        public long BootId;
        public string Utc;
    }

    // One known package: a manifest the app could read in full. Name is the manifest's "name" (the witness's
    // release), ImageSha256 the hash of its payload/kmd/bc250kmd.sys, ManifestSha256 the SHA256 of the file.
    public sealed class KnownPackage
    {
        public string Name, Version, KmdBuild, KmdAbi, KmdVersion, ImageSha256, ManifestSha256, Source;
    }

    // The app's own evidence of the KMD image that Windows loaded for the started device (decision 6): read in the
    // current boot from the list of loaded drivers, never the service's file on disk.
    public sealed class LoadedImage
    {
        public bool Available, OfStartedDevice;
        public long? BootId;
        public string Sha256, KmdBuild;     // KmdBuild: the build the known manifests record for this image hash
        public string Detail;               // report only
    }

    public sealed class VerifyReport
    {
        public bool Valid;                  // the format of interfaces.md section 2, a real run with results
        public bool Passed;                 // outcome 'passed'
        public string Utc, PackageVersion, ManifestSha256, ManifestSource;
        public int PassedCount, FailedCount;
    }

    public sealed class DriverFacts
    {
        public uint? ReplyAbi;                  // BC250_KMD_VERSION of a driver reply; null: no reply
        public long? BootId;
        public ReleaseWitness Witness;          // null: none, damaged, untrusted or unknown schema
        public List<InstallAction> InstallActions = new List<InstallAction>();
        public MappingStatus Mapping = MappingStatus.Absent;
        public List<KnownPackage> Packages = new List<KnownPackage>();
        public LoadedImage Image;               // null: no evidence
        public string InstalledVersion;         // Release\Version; null when missing
        public string InstalledManifestSha256;  // SHA256 of <InstallDir>\manifest.json; null when unreadable
        public string Phase, StatePackageVersion;   // state.json (the package version only binds the phase; report)
        public List<VerifyReport> Reports = new List<VerifyReport>();
        public string DriverDate;               // INF DriverVer date as Windows stores it (M-D-YYYY)
        public string WitnessNote;              // why a present witness was not read (report only)
    }

    public sealed class DriverCardView
    {
        public RunningKind Running;
        public string RunningVersion, RunningRelease;     // Exact only
        public List<string> Candidates = new List<string>();
        public string InstalledVersion;
        public bool InstalledPending, InstallStopped;
        public VerifyKind Verification;
        public string RunningText, InstalledText, VerificationText, DriverDateText;
        public string ReportText;               // technical detail, support report only
    }

    public static class DriverCard
    {
        public const string KmdImagePath = "payload/kmd/bc250kmd.sys";
        static readonly Regex Sha = new Regex("^[0-9A-Fa-f]{64}$"), UpperSha = new Regex("^[0-9A-F]{64}$"), Build = new Regex(@"^\d+\.\d+\.\d+\.\d+$"), Abi = new Regex("^0x[0-9A-Fa-f]{8}$");

        // kmd_abi: "0x000700C7" or decimal; null when it does not parse.
        public static uint? ParseAbi(string text)
        {
            if (string.IsNullOrEmpty(text)) return null;
            uint v;
            if (text.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
                return uint.TryParse(text.Substring(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out v) ? (uint?)v : null;
            return uint.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out v) ? (uint?)v : null;
        }

        static DateTime? Utc(string s) { return Recovery.Utc(s); }
        static bool SameHash(string a, string b) { return a != null && b != null && string.Equals(a, b, StringComparison.OrdinalIgnoreCase); }

        // Rule 1 of interfaces.md section 1: every field present and well formed. null when complete.
        public static string WitnessIncomplete(ReleaseWitness w)
        {
            if (w.BootId < 0 || w.BootId > uint.MaxValue) return "boot_id";
            if (Utc(w.RecordedUtc) == null) return "recorded_utc";
            if (string.IsNullOrEmpty(w.Release)) return "release";
            if (string.IsNullOrEmpty(w.Version)) return "version";
            if (w.ManifestSha256 == null || !Sha.IsMatch(w.ManifestSha256)) return "manifest_sha256";
            if (w.KmdImageSha256 == null || !Sha.IsMatch(w.KmdImageSha256)) return "kmd_image_sha256";
            if (w.KmdBuild == null || !Build.IsMatch(w.KmdBuild)) return "kmd_build";
            if (w.KmdAbi == null || !Abi.IsMatch(w.KmdAbi)) return "kmd_abi";
            return null;
        }

        // A package of the mapping that binds the whole witness.
        static bool Binds(KnownPackage p, ReleaseWitness w)
        {
            return SameHash(p.ManifestSha256, w.ManifestSha256) && p.Name == w.Release && p.Version == w.Version && p.KmdBuild == w.KmdBuild &&
                ParseAbi(p.KmdAbi) != null && ParseAbi(p.KmdAbi) == ParseAbi(w.KmdAbi) && SameHash(p.ImageSha256, w.KmdImageSha256);
        }

        // One entry per manifest: the installed copy and the kept repair copy of one package are one package.
        static List<KnownPackage> Distinct(List<KnownPackage> packages)
        {
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            return packages.Where(p => p != null && (string.IsNullOrEmpty(p.ManifestSha256) || seen.Add(p.ManifestSha256))).ToList();
        }

        // Why the running release is not exact; null when every acceptance rule holds (interfaces.md section 1).
        public static string WitnessProblem(DriverFacts f)
        {
            var w = f.Witness;
            if (w == null) return "no running-release witness";
            if (w.RecordedBy != "verify" && w.RecordedBy != "start-confirm") return "the witness was not written by the engine's verify step or the start-confirm task";
            var missing = WitnessIncomplete(w);
            if (missing != null) return "the witness is incomplete (" + missing + ")";
            if (f.BootId == null) return "the current boot cannot be read";
            if (w.BootId != f.BootId.Value) return "the witness is from another boot (" + w.BootId + ", now " + f.BootId + ")";
            if (f.ReplyAbi == null) return "no driver reply";
            if (ParseAbi(w.KmdAbi) != f.ReplyAbi.Value) return "the witness names kmd_abi " + w.KmdAbi + ", the driver replies " + KmdReply.VersionText(f.ReplyAbi.Value);
            var recorded = Utc(w.RecordedUtc).Value;
            foreach (var a in f.InstallActions.Where(a => a != null && a.BootId == f.BootId.Value))
                if (Utc(a.Utc) == null || Utc(a.Utc) > recorded) return "an install action at " + (a.Utc ?? "an unknown time") + " came after the witness in this boot";
            if (f.Mapping != MappingStatus.Complete) return "the manifest mapping is " + f.Mapping.ToString().ToLowerInvariant();
            int bound = Distinct(f.Packages).Count(p => Binds(p, w));
            if (bound != 1) return bound == 0 ? "no known package binds the witness (manifest, release, version, build, ABI and image)" : bound + " known packages bind the witness";
            var img = f.Image;
            if (img == null || !img.Available || !img.OfStartedDevice) return "no evidence of the loaded driver image" + (img != null && img.Detail != null ? " (" + img.Detail + ")" : "");
            if (img.BootId != f.BootId) return "the loaded-image evidence is of another boot";
            if (!SameHash(img.Sha256, w.KmdImageSha256)) return "the loaded image has SHA256 " + img.Sha256 + ", the witness names " + w.KmdImageSha256;
            if (img.KmdBuild != w.KmdBuild) return "the loaded image is build " + (img.KmdBuild ?? "unknown") + ", the witness names " + w.KmdBuild;
            return null;
        }

        // Verification of the installed package only (decision 8): its newest valid report.
        public static VerifyReport InstalledReport(DriverFacts f)
        {
            if (string.IsNullOrEmpty(f.InstalledVersion) || f.InstalledManifestSha256 == null) return null;
            return f.Reports.Where(r => r != null && r.Valid && r.PackageVersion == f.InstalledVersion &&
                    string.Equals(r.ManifestSha256, f.InstalledManifestSha256, StringComparison.Ordinal))
                .OrderByDescending(r => Utc(r.Utc) ?? DateTime.MinValue).FirstOrDefault();
        }

        public static DriverCardView Decide(DriverFacts f)
        {
            var v = new DriverCardView();
            var problem = WitnessProblem(f);
            var packages = Distinct(f.Packages);
            if (problem == null) { v.Running = RunningKind.Exact; v.RunningVersion = f.Witness.Version; v.RunningRelease = f.Witness.Release; }
            else
            {
                if (f.ReplyAbi != null)
                    v.Candidates = packages.Where(p => ParseAbi(p.KmdAbi) == f.ReplyAbi.Value).Select(p => p.Version ?? "(no version)")
                        .OrderBy(x => x, StringComparer.OrdinalIgnoreCase).ToList();
                v.Running = f.Mapping == MappingStatus.Complete && v.Candidates.Count >= 2 ? RunningKind.Ambiguous : RunningKind.Unknown;
            }

            v.InstalledVersion = string.IsNullOrEmpty(f.InstalledVersion) ? null : f.InstalledVersion;
            var phase = f.StatePackageVersion != null && f.StatePackageVersion == v.InstalledVersion ? f.Phase : null;
            v.InstalledPending = v.InstalledVersion != null && (phase == "installed" || phase == "driver-pending-restart");
            v.InstallStopped = f.Phase == "install-incomplete";

            var report = InstalledReport(f);
            v.Verification = report == null ? VerifyKind.Unknown : report.Passed ? VerifyKind.Verified : VerifyKind.Failed;

            v.RunningText = v.Running == RunningKind.Exact ? Strings.T("drv.running.exact", v.RunningVersion) : Strings.T("drv.running.unknown");
            v.InstalledText = v.InstalledVersion == null ? Strings.T("drv.installed.none")
                : v.InstalledPending ? Strings.T("drv.installed.pending", v.InstalledVersion) : Strings.T("drv.installed", v.InstalledVersion);
            if (v.InstallStopped) v.InstalledText += " " + Strings.T("drv.installed.stopped");
            v.VerificationText = Strings.T(v.Verification == VerifyKind.Verified ? "drv.verify.ok" : v.Verification == VerifyKind.Failed ? "drv.verify.failed" : "drv.verify.unknown");
            var date = InfDate(f.DriverDate);
            v.DriverDateText = date == null ? Strings.T("drv.date.none") : Strings.T("drv.date", date.Value.ToString("yyyy-MM-dd", CultureInfo.InvariantCulture));

            v.ReportText = "running release: " + v.Running + (v.RunningRelease != null ? " " + v.RunningRelease + " (" + v.RunningVersion + ")" : "") +
                (problem != null ? " (" + problem + ")" : " (witness of this boot, bound to its manifest and the loaded image)") +
                (v.Candidates.Count > 0 ? "; packages sharing the reply ABI: " + string.Join(", ", v.Candidates) : "") +
                "; manifest mapping " + f.Mapping.ToString().ToLowerInvariant() + " (" + packages.Count + " packages)" +
                "; driver reply " + (f.ReplyAbi != null ? KmdReply.VersionText(f.ReplyAbi.Value) + " (0x" + f.ReplyAbi.Value.ToString("X8", CultureInfo.InvariantCulture) + ")" : "none") +
                "; loaded image " + (f.Image != null && f.Image.Available ? f.Image.Sha256 + " build " + (f.Image.KmdBuild ?? "unknown") : "unknown" + (f.Image != null && f.Image.Detail != null ? " (" + f.Image.Detail + ")" : "")) +
                "; installed " + (v.InstalledVersion ?? "unknown") + "; installer state " + (f.Phase ?? "none") + " for " + (f.StatePackageVersion ?? "none") +
                "; verification " + v.Verification + (report != null ? " (" + report.Utc + ": " + report.PassedCount + " passed, " + report.FailedCount + " failed)" : "") +
                (f.WitnessNote != null ? "; " + f.WitnessNote : "");
            return v;
        }

        // Windows stores DriverDate as M-D-YYYY.
        public static DateTime? InfDate(string text)
        {
            DateTime d;
            return DateTime.TryParseExact(text ?? "", new[] { "M-d-yyyy", "MM-dd-yyyy", "M/d/yyyy" }, CultureInfo.InvariantCulture, DateTimeStyles.None, out d) ? (DateTime?)d : null;
        }

        // Rank 5 "upgrade done and verified" (section 6, decision 5, review 927 R6): an exact witness of this boot
        // that names the installed release, that release verified and not waiting for a restart.
        public static bool UpgradeVerified(DriverCardView v)
        {
            return v.Running == RunningKind.Exact && v.InstalledVersion != null && string.Equals(v.RunningVersion, v.InstalledVersion, StringComparison.Ordinal) &&
                v.Verification == VerifyKind.Verified && !v.InstalledPending;
        }

        // The one-time verdict: upgrade verified, and not the release the app showed last (never on a first run).
        public static bool UpgradeDone(DriverCardView v, string lastSeenRelease)
        {
            return UpgradeVerified(v) && lastSeenRelease != null && !string.Equals(lastSeenRelease, v.InstalledVersion, StringComparison.OrdinalIgnoreCase);
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

        // The witness as written; completeness is a rule of WitnessProblem. null: no witness (another schema, damaged).
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

        // state.json: the phase, the package it is for and the newest install action.
        public static void ParseState(string text, DriverFacts f)
        {
            var d = Json(text);
            if (d == null) return;
            f.Phase = S(d, "phase");
            f.StatePackageVersion = S(d, "package_version");
            var boot = N(d, "mutation_boot_id");
            if (boot != null) f.InstallActions.Add(new InstallAction { BootId = boot.Value, Utc = S(d, "mutation_utc") });
        }

        // A verify report of the engine (Write-VerifyReport). Valid: the schema, not a dry run, a non-empty result list
        // with a boolean pass each, and an outcome that agrees with it ('passed' needs complete and every result).
        public static VerifyReport ParseVerify(string text)
        {
            var d = Json(text);
            if (d == null) return null;
            var r = new VerifyReport { Utc = S(d, "utc"), PackageVersion = S(d, "package_version"), ManifestSha256 = S(d, "manifest_sha256"), ManifestSource = S(d, "manifest_source") };
            object results, dry, complete;
            bool listOk = d.TryGetValue("results", out results) && results is object[] && ((object[])results).Length > 0;
            if (listOk)
                foreach (var o in (object[])results)
                {
                    var x = o as IDictionary<string, object>;
                    object pass;
                    if (x != null && x.TryGetValue("pass", out pass) && pass is bool) { if ((bool)pass) r.PassedCount++; else r.FailedCount++; }
                    else { r.FailedCount++; listOk = false; }
                }
            var outcome = S(d, "outcome");
            bool isComplete = d.TryGetValue("complete", out complete) && complete is bool && (bool)complete;
            bool notDry = d.TryGetValue("dry_run", out dry) && dry is bool && !(bool)dry;
            r.Passed = outcome == "passed";
            // manifest_source "package": a verify of a package that is not installed (no manifest in the install root); it
            // never binds the installed release (interfaces-setup.md section 9).
            r.Valid = S(d, "schema") == "amdgpu-wddm.verify-report/1" && notDry && listOk && (outcome == "failed" || (r.Passed && isComplete && r.FailedCount == 0)) &&
                r.ManifestSha256 != null && UpperSha.IsMatch(r.ManifestSha256) && r.ManifestSource != "package";
            return r;
        }

        // A manifest as a known package; null when it lacks a field the mapping needs (the mapping is then
        // incomplete). manifestSha256 is the SHA256 of the file.
        public static KnownPackage ParsePackage(string manifestJson, string manifestSha256, string source)
        {
            var d = Json(manifestJson);
            if (d == null) return null;
            string image = null;
            object list;
            if (d.TryGetValue("components", out list) && list is object[])
                foreach (var o in (object[])list)
                {
                    var c = o as IDictionary<string, object>;
                    if (c != null && S(c, "package_path") == KmdImagePath) { image = S(c, "sha256"); break; }
                }
            var p = new KnownPackage { Name = S(d, "name"), Version = S(d, "version"), KmdBuild = S(d, "kmd_build"), KmdAbi = S(d, "kmd_abi"),
                KmdVersion = S(d, "kmd_version"), ImageSha256 = image, ManifestSha256 = manifestSha256, Source = source };
            bool complete = !string.IsNullOrEmpty(p.Name) && !string.IsNullOrEmpty(p.Version) && p.KmdBuild != null && Build.IsMatch(p.KmdBuild) &&
                p.KmdAbi != null && Abi.IsMatch(p.KmdAbi) && p.ImageSha256 != null && Sha.IsMatch(p.ImageSha256) && manifestSha256 != null && Sha.IsMatch(manifestSha256);
            return complete ? p : null;
        }

        // The mapping from what was read: no manifest at all = absent; one that could not be read in full =
        // incomplete; two manifests that disagree about one release or one KMD image = conflicting.
        public static MappingStatus Mapping(int manifestsFound, List<KnownPackage> packages)
        {
            if (manifestsFound == 0) return MappingStatus.Absent;
            if (packages.Count(p => p != null) < manifestsFound) return MappingStatus.Incomplete;
            var list = Distinct(packages);
            foreach (var g in list.GroupBy(p => p.Name + "|" + p.Version)) if (g.Count() > 1) return MappingStatus.Conflicting;
            foreach (var g in list.GroupBy(p => p.ImageSha256.ToUpperInvariant()))
                if (g.Select(p => p.KmdBuild).Distinct().Count() > 1 || g.Select(p => ParseAbi(p.KmdAbi)).Distinct().Count() > 1) return MappingStatus.Conflicting;
            return MappingStatus.Complete;
        }

        // The build of a loaded image: the kmd_build the known manifests record for its hash, when they agree.
        public static string BuildOfImage(string sha256, List<KnownPackage> packages)
        {
            var builds = packages.Where(p => p != null && SameHash(p.ImageSha256, sha256)).Select(p => p.KmdBuild).Distinct().ToList();
            return builds.Count == 1 ? builds[0] : null;
        }
    }

    // The I/O of the Driver card: reads only; installer files must be owned by Administrators or SYSTEM.
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

        static bool Fixture { get { return Environment.GetEnvironmentVariable("AMDGPU_WDDM_CONTROL_INSTALLER") != null; } }

        static bool Trusted(string path)
        {
            if (Fixture) return true;    // the build's fixtures
            try
            {
                var owner = File.GetAccessControl(path, AccessControlSections.Owner).GetOwner(typeof(SecurityIdentifier));
                return owner.Equals(new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null)) || owner.Equals(new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null));
            }
            catch (Exception) { return false; }
        }

        static byte[] ReadSmall(string path)
        {
            try { return File.Exists(path) && new FileInfo(path).Length <= 4 << 20 && Trusted(path) ? File.ReadAllBytes(path) : null; }
            catch (Exception) { return null; }
        }

        static string Text(byte[] b) { return b == null ? null : new UTF8Encoding(false).GetString(b).TrimStart('﻿'); }

        static string Hash(byte[] b)
        {
            if (b == null) return null;
            using (var sha = SHA256.Create()) return BitConverter.ToString(sha.ComputeHash(b)).Replace("-", "");
        }

        public static DriverFacts Read(uint? replyAbi, long? bootId, string installedVersion, string installDir, string driverDate)
        {
            var f = new DriverFacts { ReplyAbi = replyAbi, BootId = bootId, InstalledVersion = installedVersion, DriverDate = driverDate };
            var witnessPath = Path.Combine(Installer, "running-release.json");
            var text = Text(ReadSmall(witnessPath));
            f.Witness = text != null ? DriverCard.ParseWitness(text) : null;
            if (text == null && File.Exists(witnessPath)) f.WitnessNote = "running-release.json is not owned by Administrators or SYSTEM, or unreadable";
            else if (text != null && f.Witness == null) f.WitnessNote = "running-release.json is damaged or of an unknown schema";
            var state = Text(ReadSmall(Path.Combine(Installer, "state.json")));
            if (state != null) DriverCard.ParseState(state, f);
            try
            {
                var dir = Path.Combine(Installer, "verify");
                if (Directory.Exists(dir))
                    foreach (var file in Directory.GetFiles(dir, "verify-*.json").OrderByDescending(x => x, StringComparer.OrdinalIgnoreCase).Take(20))
                    {
                        var r = DriverCard.ParseVerify(Text(ReadSmall(file)));
                        if (r != null) f.Reports.Add(r);
                    }
            }
            catch (Exception) { }

            var manifests = new List<string>();
            string installed = string.IsNullOrEmpty(installDir) ? null : Path.Combine(installDir, "manifest.json");
            if (installed != null && File.Exists(installed)) manifests.Add(installed);
            try
            {
                var kept = Path.Combine(Installer, "packages");
                if (Directory.Exists(kept)) manifests.AddRange(Directory.GetDirectories(kept).Select(d => Path.Combine(d, "manifest.json")).Where(File.Exists));
            }
            catch (Exception) { }
            var parsed = new List<KnownPackage>();
            foreach (var m in manifests)
            {
                var bytes = ReadSmall(m);
                var sha = Hash(bytes);
                if (m == installed) f.InstalledManifestSha256 = sha;
                parsed.Add(bytes != null ? DriverCard.ParsePackage(Text(bytes), sha, m) : null);
            }
            f.Mapping = DriverCard.Mapping(manifests.Count, parsed);
            f.Packages = parsed.Where(p => p != null).ToList();
            f.Image = LoadedKmd(bootId, f.Packages);
            return f;
        }

        // The loaded bc250kmd.sys: the module Windows lists as loaded, its file's SHA256, valid only when the file was
        // not created or written after this boot started (as the engine's witness writer requires).
        static LoadedImage LoadedKmd(long? bootId, List<KnownPackage> packages)
        {
            var img = new LoadedImage();
            try
            {
                var module = LoadedDrivers().FirstOrDefault(n => string.Equals(Path.GetFileName(n), "bc250kmd.sys", StringComparison.OrdinalIgnoreCase));
                if (module == null) { img.Detail = "bc250kmd.sys is not among the loaded drivers"; return img; }
                var path = Win32Path(module);
                var boot = BootUtc();
                if (path == null || !File.Exists(path)) { img.Detail = "the loaded module " + module + " has no readable file"; return img; }
                if (boot == null || bootId == null) { img.Detail = "the start of this boot cannot be read"; return img; }
                var fi = new FileInfo(path);
                if (fi.CreationTimeUtc > boot.Value || fi.LastWriteTimeUtc > boot.Value) { img.Detail = "the loaded image file was replaced after this boot started"; return img; }
                using (var s = File.OpenRead(path)) using (var sha = SHA256.Create()) img.Sha256 = BitConverter.ToString(sha.ComputeHash(s)).Replace("-", "");
                img.Available = true; img.OfStartedDevice = true; img.BootId = bootId;
                img.KmdBuild = DriverCard.BuildOfImage(img.Sha256, packages);
                img.Detail = path;
            }
            catch (Exception e) { img.Available = false; img.Detail = "the loaded image cannot be read (" + e.Message + ")"; }
            return img;
        }

        static string Win32Path(string name)
        {
            var windir = Environment.GetFolderPath(Environment.SpecialFolder.Windows);
            if (name.StartsWith(@"\SystemRoot\", StringComparison.OrdinalIgnoreCase)) return Path.Combine(windir, name.Substring(12));
            if (name.StartsWith(@"\??\", StringComparison.Ordinal)) return name.Substring(4);
            if (name.StartsWith(@"\Windows\", StringComparison.OrdinalIgnoreCase)) return Path.Combine(windir, name.Substring(9));
            return Path.IsPathRooted(name) && !name.StartsWith(@"\", StringComparison.Ordinal) ? name : null;
        }

        static DateTime? _bootUtc;

        // Win32_OperatingSystem.LastBootUpTime, as the engine reads it; once per process.
        static DateTime? BootUtc()
        {
            if (_bootUtc != null) return _bootUtc;
            try
            {
                using (var q = new System.Management.ManagementObjectSearcher("SELECT LastBootUpTime FROM Win32_OperatingSystem"))
                    foreach (System.Management.ManagementObject o in q.Get())
                        using (o) _bootUtc = System.Management.ManagementDateTimeConverter.ToDateTime((string)o["LastBootUpTime"]).ToUniversalTime();
            }
            catch (Exception) { }
            return _bootUtc;
        }

        static IEnumerable<string> LoadedDrivers()
        {
            int needed;
            EnumDeviceDrivers(null, 0, out needed);
            if (needed <= 0) return new string[0];
            var bases = new IntPtr[needed / IntPtr.Size + 16];
            if (!EnumDeviceDrivers(bases, bases.Length * IntPtr.Size, out needed)) return new string[0];
            var list = new List<string>();
            for (int i = 0; i < Math.Min(bases.Length, needed / IntPtr.Size); i++)
            {
                if (bases[i] == IntPtr.Zero) continue;
                var sb = new StringBuilder(1024);
                if (GetDeviceDriverFileNameW(bases[i], sb, sb.Capacity) > 0) list.Add(sb.ToString());
            }
            return list;
        }

        [DllImport("psapi.dll", SetLastError = true)]
        static extern bool EnumDeviceDrivers([Out] IntPtr[] bases, int cb, out int needed);

        [DllImport("psapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern int GetDeviceDriverFileNameW(IntPtr imageBase, StringBuilder name, int size);

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
